#include "VoodooSMBusControllerDriver.hpp"
#include "VoodooSMBusDeviceNub.hpp"

OSDefineMetaClassAndStructors(VoodooSMBusControllerDriver, IOService)

#define super IOService

bool VoodooSMBusControllerDriver::init(OSDictionary *dict) {
    bool result = super::init(dict);
    IOLog("Initializing\n");
    
    // For now, we support only one slave device
    device_nubs = OSArray::withCapacity(1);

    adapter = reinterpret_cast<i801_adapter*>(IOMalloc(sizeof(i801_adapter)));
    adapter->awake = true;
    
    return result;
}

void VoodooSMBusControllerDriver::free(void) {
    IOLog("Freeing\n");
    IOFree(adapter, sizeof(i801_adapter));
    OSSafeReleaseNULL(device_nubs);
    super::free();
}

IOService *VoodooSMBusControllerDriver::probe(IOService *provider, SInt32 *score) {
    IOService *result = super::probe(provider, score);
    IOLog("Probing\n");
    
    return result;
}

bool VoodooSMBusControllerDriver::start(IOService *provider) {
    bool result = super::start(provider);
    IOLog("Starting\n");
    
    pci_device = OSDynamicCast(IOPCIDevice, provider);
    
    if (!(pci_device = OSDynamicCast(IOPCIDevice, provider))) {
        IOLog("Failed to cast provider\n");
        return false;
    }
   
    adapter->provider = provider;
    adapter->pci_device = pci_device;
    adapter->name = getMatchedName(adapter->provider);
    
    pci_device->retain();
    if (!pci_device->open(this)) {
        IOLog("%s::%s Could not open provider\n", getName(), pci_device->getName());
        return false;
    }
    
    uint32_t host_config = pci_device->configRead8(SMBHSTCFG);
    if ((host_config & SMBHSTCFG_HST_EN) == 0) {
        IOLog("SMBus disabled\n");
        return false;
    }
    
    // TODO why 0xfffe
    adapter->smba = pci_device->configRead16(ICH_SMB_BASE) & 0xFFFE;
    IOLog("SMBA: %lu", adapter->smba);
    
    if (host_config & SMBHSTCFG_SMB_SMI_EN) {
        IOLog("No PCI IRQ. Poll mode is not implemented. Unloading.\n");
        return false;
    }
    pci_device->setIOEnable(true);
    
    adapter->original_hstcfg = host_config;
    adapter->original_slvcmd = pci_device->ioRead8(SMBSLVCMD(adapter));
    adapter->features |= FEATURE_I2C_BLOCK_READ;
    adapter->features |= FEATURE_IRQ;
    //adapter->features |= FEATURE_SMBUS_PEC;
    
    
    
    adapter->features |= FEATURE_BLOCK_BUFFER;
    adapter->features |= FEATURE_HOST_NOTIFY;
    
    adapter->retries = 4;
    //adapter->timeout = 200;
    
    enableHostNotify();
    
    
    work_loop = reinterpret_cast<IOWorkLoop*>(getWorkLoop());
    if (!work_loop) {
        IOLog("%s Could not get work loop\n", getName());
        goto exit;
    }
    
    interrupt_source =
    IOInterruptEventSource::interruptEventSource(this, OSMemberFunctionCast(IOInterruptEventAction, this, &VoodooSMBusControllerDriver::handleInterrupt), provider);
    
    if (!interrupt_source || work_loop->addEventSource(interrupt_source) != kIOReturnSuccess) {
        IOLog("%s Could not add interrupt source to work loop\n", getName());
        goto exit;
    }
    interrupt_source->enable();
    
    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s Could not open command gate\n", getName());
        goto exit;
    }
    work_loop->retain();
    
    publishNub(ELAN_TOUCHPAD_ADDRESS);
    registerService();

    IOLog("Everything went well: %s", adapter->name);
    return result;
    
exit:
    releaseResources();
    return false;
}

void VoodooSMBusControllerDriver::releaseResources() {
    IOLog("Releasing resources");
    
    IOLog("- disableHostNotify");
    
    disableHostNotify();
    
    IOLog("- Device nubs");

    if (device_nubs) {
        while (device_nubs->getCount() > 0) {
            VoodooSMBusDeviceNub *device_nub = reinterpret_cast<VoodooSMBusDeviceNub*>(device_nubs->getLastObject());
            device_nub->detach(this);
            device_nubs->removeObject(device_nubs->getCount() - 1);
            OSSafeReleaseNULL(device_nub);
        }
    }
    
    IOLog("- command gate ");

    if (command_gate) {
        work_loop->removeEventSource(command_gate);
        command_gate->release();
        command_gate = NULL;
    }
    
    IOLog("- interrupt_source ");

    if (interrupt_source) {
        interrupt_source->disable();
        work_loop->removeEventSource(interrupt_source);
        interrupt_source->release();
        interrupt_source = NULL;
    }
    
    if (work_loop) {
        work_loop->release();
        work_loop = NULL;
    }
    
    pci_device->close(this);
    pci_device->release();
}


void VoodooSMBusControllerDriver::stop(IOService *provider) {
    IOLog("Stopping\n");
    
    releaseResources();
    super::stop(provider);
}


IOReturn VoodooSMBusControllerDriver::publishNub(UInt8 address) {
    IOLog("%s::%s Publishing nub\n", getName(), adapter->name);
    
    VoodooSMBusDeviceNub* device_nub = OSTypeAlloc(VoodooSMBusDeviceNub);
    
    
    if (!device_nub || !device_nub->init()) {
        IOLog("%s::%s Could not initialise nub", getName(), adapter->name);
        goto exit;
    }
    
    if (!device_nub->attach(this, address)) {
        IOLog("%s::%s Could not attach nub", getName(), adapter->name);
        goto exit;
    }
    
    if (!device_nub->start(this)) {
        IOLog("%s::%s Could not start nub", getName(), adapter->name);
        goto exit;
    }
    
    device_nubs->setObject(device_nub);
    
    return kIOReturnSuccess;
    
exit:
    OSSafeReleaseNULL(device_nub);
    
    return kIOReturnError;
}

IOWorkLoop* VoodooSMBusControllerDriver::getWorkLoop() {
    // Do we have a work loop already?, if so return it NOW.
    if ((vm_address_t) work_loop >> 1)
        return work_loop;
    
    if (OSCompareAndSwap(0, 1, reinterpret_cast<IOWorkLoop*>(&work_loop))) {
        // Construct the workloop and set the cntrlSync variable
        // to whatever the result is and return
        work_loop = IOWorkLoop::workLoop();
    } else {
        while (reinterpret_cast<IOWorkLoop*>(work_loop) == reinterpret_cast<IOWorkLoop*>(1)) {
            // Spin around the cntrlSync variable until the
            // initialization finishes.
            thread_block(0);
        }
    }
    
    return work_loop;
}

void VoodooSMBusControllerDriver::handleInterrupt(OSObject* owner, IOInterruptEventSource* src, int intCount) {
    IOLog("Interrupt occured\n");
    
    u16 pcists;
    u8 status;
    

    if (adapter->features & FEATURE_HOST_NOTIFY) {
        status = adapter->inb_p(SMBSLVSTS(adapter));
        if (status & SMBSLVSTS_HST_NTFY_STS) {
            
            /*
             * With the tested platforms, reading SMBNTFDDAT (22 + (p)->smba)
             * always returns 0. Our current implementation doesn't provide
             * data, so we just ignore it.
             */
            //i2c_handle_smbus_host_notify(&priv->adapter, addr);
            IOLog("i2c_handle_smbus_host_notify\n");
        
            /* clear Host Notify bit and return */
            adapter->outb_p(SMBSLVSTS_HST_NTFY_STS, SMBSLVSTS(adapter));
            return;
        }
    }
    
    status = adapter->inb_p(SMBHSTSTS(adapter));
    PrintBitFieldExpanded(status);

    if (status & SMBHSTSTS_BYTE_DONE)
        i801_isr_byte_done(adapter);
    
    /*
     * Clear irq sources and report transaction result.
     * ->status must be cleared before the next transaction is started.
     */
    status &= SMBHSTSTS_INTR | STATUS_ERROR_FLAGS;
    if (status) {
        adapter->outb_p(status, SMBHSTSTS(adapter));
        adapter->status = status;
        command_gate->commandWakeup(&adapter->status);
    }
    
}




void VoodooSMBusControllerDriver::enableHostNotify() {
    
    if(!(adapter->original_slvcmd & SMBSLVCMD_HST_NTFY_INTREN)) {
        pci_device->ioWrite8(SMBSLVCMD(adapter), SMBSLVCMD_HST_NTFY_INTREN | adapter->original_slvcmd);
    }

    /* clear Host Notify bit to allow a new notification */
    pci_device->ioWrite8(SMBSLVSTS(adapter), SMBSLVSTS_HST_NTFY_STS);
}

void VoodooSMBusControllerDriver::disableHostNotify() {
    pci_device->ioWrite8(SMBSLVCMD(adapter), adapter->original_slvcmd);
}

IOReturn VoodooSMBusControllerDriver::ReadBlockData(VoodooSMBusSlaveDevice *client, u8 command, u8 *values) {
    union i2c_smbus_data data;
    IOReturn status;
    
    status = transfer(client, I2C_SMBUS_READ, command, I2C_SMBUS_BLOCK_DATA, &data);
    if (status != kIOReturnSuccess)
        return status;
    
    memcpy(values, &data.block[1], data.block[0]);
    return data.block[0];
}

IOReturn VoodooSMBusControllerDriver::WriteByteData(VoodooSMBusSlaveDevice *client, u8 command, u8 value) {
    union i2c_smbus_data data;
    data.byte = value;
    
    return transfer(client, I2C_SMBUS_WRITE, command, I2C_SMBUS_BYTE_DATA, &data);
}


IOReturn VoodooSMBusControllerDriver::WriteByte(VoodooSMBusSlaveDevice *client, u8 value) {
    return transfer(client, I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}


// i2c_smbus_xfer
IOReturn VoodooSMBusControllerDriver::transfer(VoodooSMBusSlaveDevice *client, char  read_write, u8 command, int protocol, union i2c_smbus_data *data) {
    VoodooSMBusControllerMessage message = {
        .slave_device = client,
        .read_write = read_write,
        .command = command,
        .protocol = protocol,
    };
    
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooSMBusControllerDriver::transferGated), &message, data, command_gate);
}

// __i2c_smbus_xfer
IOReturn VoodooSMBusControllerDriver::transferGated(VoodooSMBusControllerMessage *message, union i2c_smbus_data *data, IOCommandGate* command_gate) {
    int _try;
    s32 res;

    VoodooSMBusSlaveDevice* slave_device = message->slave_device;
    //slave_device->flags &= I2C_M_TEN | I2C_CLIENT_PEC | I2C_CLIENT_SCCB;
    
    /* Retry automatically on arbitration loss */
    for (res = 0, _try = 0; _try <= adapter->retries; _try++) {
        IOLog("%d, %du, %d, %d, %d", slave_device->addr, 0, message->read_write, message->command, message->protocol);
        
        res = i801_access(adapter, slave_device->addr, 0, message->read_write, message->command, message->protocol, data, command_gate);
        if (res != -EAGAIN)
            break;
    }
    
    return kIOReturnSuccess;
}



