#include "kern/ahci.h"
#include "kern/alloc.h"
#include "inc/string.h"
// #include <kernel/interrupts/irqs.h>
// #include <driver/storage/storage.h>
#include <kern/pmap.h>
#include <kern/pci.h>
#include <kern/ata.h>
#include <kern/ata_commands.h>
#include <kern/pcireg.h>
#include <kern/alloc.h>
// for sleep
#include <kern/syscall.h>

#define MASK_PAGE_4K(size)          ((uint64_t)(size) & 0xFFFFFFFFFFFFF000)     // Get only the page address.
#define MASK_PAGEFLAGS_4K(size)     ((uint64_t)(size) & ~0xFFFFFFFFFFFFF000)    // Get only the page flags.
#define PCI_INTERFACE_MASS_STORAGE_SATA_VENDOR_AHCI 0x01
#define PAGE_SIZE_4K                    0x1000


void sleep(uint64_t ms) { // тут секунды
    uint64_t now = syscall(SYS_gettime, 0, 0, 0, 0, 0);
    uint64_t end = now + ms;
    cprintf("time = %lu\n", now);
    if (end < now) {
        panic("end < now");
    }
    while ((now = syscall(SYS_gettime, 0, 0, 0, 0, 0)) < end) {
    }
}

void *paging_device_alloc(uint64_t startPhys, uint64_t endPhys) {
    return mmio_map_region(startPhys, endPhys - startPhys);
}

static void ahci_port_cmd_start(ahci_port_t *ahciPort) {
    // Get controller and port.
    struct ahci_controller_t *ahciController = ahciPort->Controller;
    volatile ahci_port_memory_t *portMemory = ahciController->Memory->Ports + ahciPort->Number;

    // Wait until port stops.
    while (portMemory->CommandStatus.CommandListRunning);

    // Start port.
    portMemory->CommandStatus.FisReceiveEnabled = true;
    portMemory->CommandStatus.Started = true;
}

static void ahci_port_cmd_stop(ahci_port_t *ahciPort) {
    // Get controller and port.
    struct ahci_controller_t *ahciController = ahciPort->Controller;
    volatile ahci_port_memory_t *portMemory = ahciController->Memory->Ports + ahciPort->Number;

    // Stop port.
    portMemory->CommandStatus.Started = false;

    // Wait until port stops.
    uint32_t timeout = 500;
    while (portMemory->CommandStatus.FisReceiveRunning && portMemory->CommandStatus.CommandListRunning) {
        // Was the timeout reached?
        if (timeout == 0) {
            cprintf("AHCI: Timeout waiting for port %u to stop!\n", ahciPort->Number);
            break;
        }

        sleep(1);
        timeout--;
    }

    // Stop recieving FISes.
    portMemory->CommandStatus.FisReceiveEnabled = false;
}

static void ahci_port_init_memory(ahci_port_t *ahciPort) {
    // Get controller and port.
    struct ahci_controller_t *ahciController = ahciPort->Controller;
    volatile ahci_port_memory_t *portMemory = ahciController->Memory->Ports + ahciPort->Number;

    // Stop port.
    ahci_port_cmd_stop(ahciPort);

    // Get physical addresses of command list.
    uint64_t commandListPhys = 0;
    // if (!paging_get_phys((uintptr_t)ahciPort->CommandList, &commandListPhys))
    //     panic("AHCI: Attempted to use nonpaged address for command list!\n");
    portMemory->CommandListBaseAddress = commandListPhys;
    memset(ahciPort->CommandList, 0, AHCI_COMMAND_LIST_SIZE);

    // Get physical addresses of received FISes.
    uint64_t fisPhys = 0;
    // if (!paging_get_phys((uintptr_t)ahciPort->ReceivedFis, &fisPhys))
    //     panic("AHCI: Attempted to use nonpaged address for command list!\n");
    portMemory->FisBaseAddress = fisPhys;
    memset(ahciPort->ReceivedFis, 0, sizeof(ahci_received_fis_t));
}

static bool ahci_port_reset(ahci_port_t *ahciPort) {
    // Get controller and port.
    struct ahci_controller_t *ahciController = ahciPort->Controller;
    volatile ahci_port_memory_t *portMemory = ahciController->Memory->Ports + ahciPort->Number;

    // Stop port.
    ahci_port_cmd_stop(ahciPort);

    // Enable reset bit for 10ms (as per 10.4.2 Port Reset).
    cprintf("AHCI: Resetting port %u...\n", ahciPort->Number);
    portMemory->SataControl.DeviceDetectionInitialization = AHCI_SATA_STATUS_DETECT_INIT_RESET;
    sleep(10);
    portMemory->SataControl.DeviceDetectionInitialization = AHCI_SATA_STATUS_DETECT_INIT_NO_ACTION;

    // Wait for port to be ready.
    uint32_t timeout = 1000;
    while (portMemory->SataStatus.Data.DeviceDetection != AHCI_SATA_STATUS_DETECT_CONNECTED) {
        // Was the timeout reached?
        if (timeout == 0) {
            cprintf("AHCI: Timeout waiting for port %u to reset!\n", ahciPort->Number);
            return false;
        }

        sleep(1);
        timeout--;
    }

    // Restart port.
    ahci_port_cmd_start(ahciPort);

    // Clear port error register.
    portMemory->SataError.RawValue = -1;

    // Wait for device to be ready.
    timeout = 1000;
    while (portMemory->TaskFileData.Status.Data.Busy || portMemory->TaskFileData.Status.Data.DataRequest
        || portMemory->TaskFileData.Status.Data.Error) {
        // Was the timeout reached?
        if (timeout == 0) {
            cprintf("AHCI: Timeout waiting for driver on port %u to be ready!\n", ahciPort->Number);
            return false;
        }

        sleep(1);
        timeout--;
    }
    return true;
}

static bool ahci_probe_port(ahci_port_t *ahciPort) {
    // Get controller and port.
    struct ahci_controller_t *ahciController = ahciPort->Controller;
    volatile ahci_port_memory_t *portMemory = ahciController->Memory->Ports + ahciPort->Number;
    // Check status.
    ahciPort->Type = AHCI_DEV_TYPE_NONE;
    if (portMemory->SataStatus.Data.DeviceDetection != AHCI_SATA_STATUS_DETECT_CONNECTED)
        return false;
    if (portMemory->SataStatus.Data.InterfacePowerManagement != AHCI_SATA_STATUS_IPM_ACTIVE)
        return false; 

    // Determine type.
    switch (portMemory->Signature.Value) {
        case AHCI_SIG_ATA:
            ahciPort->Type = AHCI_DEV_TYPE_SATA;
            return true;

        case AHCI_SIG_ATAPI:
            ahciPort->Type = AHCI_DEV_TYPE_SATA_ATAPI;
            return true;
    }
    return false;
}

static bool ahci_take_ownership(struct ahci_controller_t *ahciController) {
    // As per "10.6.3 OS declares ownership request" in the AHCI spec.
    // Set OS ownership bit.
    ahciController->Memory->BiosHandoff.OsOwnedSemaphore = true;

    // Wait 25ms and check if BIOS is still busy. If so, wait another 2 seconds.
    sleep(25);
    if (ahciController->Memory->BiosHandoff.BiosBusy)
        sleep(2000);

    // Wait for BIOS to give up ownership.
    uint32_t timeout = 200;
    while (ahciController->Memory->BiosHandoff.BiosOwnedSemaphore) {
        // Was the timeout reached?
        if (timeout == 0) {
            cprintf("AHCI: Timeout waiting for ownership of controller!\n");
            return false;
        }

        sleep(10);
        timeout--;
    }

    // Ownership acquired.
    cprintf("AHCI: Ownership acquired!\n");
    return true;
}

int ahci_init(struct pci_func *pciDevice) { 
    // ICheck that the device is an AHCI controller, and that the BAR is correct.
    pci_func_enable(pciDevice);
    // sleep(20);
    if (!(PCI_CLASS(pciDevice->dev_class) == PCI_CLASS_MASS_STORAGE /* && PCI_SUBCLASS(pciDevice->dev_class) == PCI_SUBCLASS_MASS_STORAGE_SATA && pciDevice->Interface == PCI_INTERFACE_MASS_STORAGE_SATA_VENDOR_AHCI*/))
        return 0;
    // if (!(!pciDevice->BaseAddresses[5].PortMapped && pciDevice->BaseAddresses[5].BaseAddress != 0)) { // проверяем что адрес AHCI корректный
    if (!pciDevice->reg_base[5]) {
        cprintf("%ld\n", pciDevice->reg_size[5]);
        cprintf("AHCI: Invalid base address. Aborting!\n");
        return 0;
    }

    // Create controller object and map to memory.
    cprintf("AHCI: Initializing controller at 0x%X...\n", pciDevice->BaseAddresses[5].BaseAddress);
    // выделяем память и зануляем
    
    struct ahci_controller_t *ahciController = (struct ahci_controller_t*)(unsigned long long)test_alloc(sizeof(struct ahci_controller_t));
    memset(ahciController, 0, sizeof(struct ahci_controller_t));


    ahciController->BaseAddress = pciDevice->BaseAddresses[5].BaseAddress;
    ahciController->Memory = (ahci_memory_t*)((uintptr_t)paging_device_alloc(MASK_PAGE_4K(ahciController->BaseAddress), MASK_PAGE_4K(ahciController->BaseAddress))
        + MASK_PAGEFLAGS_4K(ahciController->BaseAddress));

    cprintf("AHCI: Capabilities: 0x%X.\n", ahciController->Memory->Capabilities.RawValue);
    cprintf("AHCI: Current AHCI controller setting: %s.\n", ahciController->Memory->GlobalControl.AhciEnabled ? "on" : "off");
    if (ahciController->Memory->CapabilitiesExtended.Handoff)
        cprintf("AHCI: BIOS handoff required.\n");

    // Get ownership.
    if (!ahci_take_ownership(ahciController)) {
        cprintf("AHCI: Failed to get ownership. Aborting!\n");
        return 0;
    }

    // Enable AHCI on controller.
    ahciController->Memory->GlobalControl.AhciEnabled = true;

    // Get port count and create port pointer array.
    ahciController->PortCount = ahciController->Memory->Capabilities.Data.PortCount + 1;
    ahciController->Ports = (ahci_port_t**)(unsigned long long)test_alloc(sizeof(ahci_port_t*) * ahciController->PortCount);
    memset(ahciController->Ports, 0, sizeof(ahci_port_t*) * ahciController->PortCount);

    // Page to allocate command lists from.
    // uint32_t commandListPage = pmm_pop_frame_nonlong();
    uint32_t commandListPage = 0;
    ahci_command_header_t *commandLists = (ahci_command_header_t*)(unsigned long long)paging_device_alloc(commandListPage, commandListPage); // mmio_map_region???
    memset(commandLists, 0, PAGE_SIZE_4K);
    uint8_t commandListsAllocated = 0;
    const uint8_t maxCommandLists = PAGE_SIZE_4K / AHCI_COMMAND_LIST_SIZE; // 4 is the max that can be allocated from a 4KB page.

    // Page to allocated the recieved FIS structures from.
    // uint32_t receivedFisPage = pmm_pop_frame_nonlong();
    uint32_t receivedFisPage = 0;
    ahci_received_fis_t *recievedFises = (ahci_received_fis_t*)(unsigned long long)paging_device_alloc(receivedFisPage, receivedFisPage);
    memset(recievedFises, 0, PAGE_SIZE_4K);
    uint8_t recievedFisesAllocated = 0;
    const uint8_t maxRecievedFises = PAGE_SIZE_4K / sizeof(ahci_received_fis_t); // 16 is the max that can be allocated from a 4KB page.

    // Detect and create ports.
    uint32_t enabledPorts = 0;
    for (uint8_t port = 0; port < ahciController->PortCount; port++) {
        if (ahciController->Memory->PortsImplemented & (1 << port)) {
            // If no more command lists can be allocated, pop another page.
            if (commandListsAllocated == maxCommandLists) {
                // commandListPage = pmm_pop_frame_nonlong();
                commandLists = (ahci_command_header_t*)(unsigned long long)paging_device_alloc(commandListPage, commandListPage);
                memset(commandLists, 0, PAGE_SIZE_4K);
                commandListsAllocated = 0;
            }

            // If no more recived FIS structures can be allocated, pop another page.
            if (recievedFisesAllocated == maxRecievedFises) {
                // receivedFisPage = pmm_pop_frame_nonlong();
                recievedFises = (ahci_received_fis_t*)(unsigned long long)paging_device_alloc(receivedFisPage, receivedFisPage);
                memset(recievedFises, 0, PAGE_SIZE_4K);
                recievedFisesAllocated = 0;
            }

            // Create port object.
            ahciController->Ports[port] = (ahci_port_t*)(unsigned long long)test_alloc(sizeof(ahci_port_t));
            memset(ahciController->Ports[port], 0, sizeof(ahci_port_t));
            ahciController->Ports[port]->Controller = ahciController;
            ahciController->Ports[port]->Number = port;
            ahciController->Ports[port]->CommandList = commandLists + (AHCI_COMMAND_LIST_COUNT * commandListsAllocated);
            commandListsAllocated++;
            ahciController->Ports[port]->ReceivedFis = recievedFises + recievedFisesAllocated;
            recievedFisesAllocated++;

            // Stop port.
            ahci_port_cmd_stop(ahciController->Ports[port]);

            // Initialize port's memory.
            ahci_port_init_memory(ahciController->Ports[port]);      
            enabledPorts++;
        }
    }

    cprintf("AHCI: Version major 0x%X, minor 0x%X\n", ahciController->Memory->Version.Major, ahciController->Memory->Version.Minor);
    cprintf("AHCI: Total ports: %u (%u enabled)\n", ahciController->PortCount, enabledPorts);

    // Software needs to wait at least 500 ms for ports to be idle, as per spec.
    sleep(700);

    // Reset and probe ports.
    for (uint32_t port = 0; port < ahciController->PortCount; port++) {
        // Skip over disabled ports.
        if (ahciController->Ports[port] == NULL)
            continue;

        // Reset port.
        ahci_port_reset(ahciController->Ports[port]);

        // Probe port.
        cprintf("AHCI: Probing port (status 0x%X) %u...\n", ahciController->Memory->Ports[port].SataStatus.Data.DeviceDetection, port);
        if (ahci_probe_port(ahciController->Ports[port])) {
            switch (ahciController->Ports[port]->Type) {
                case AHCI_DEV_TYPE_SATA:
                    cprintf("AHCI: Found SATA drive on port %u.\n", port);
                    break;

                case AHCI_DEV_TYPE_SATA_ATAPI:
                    cprintf("AHCI: Found SATA ATAPI drive on port %u.\n", port);
                    break;
            }
        } 
    }


    ahci_port_t *hddPort = ahciController->Ports[0];
    

    cprintf("moving on\n");
    // uint32_t cmdTablePage = pmm_pop_frame_nonlong();
    uint32_t cmdTablePage = 0;
    ahci_command_table_t *cmdTable = (ahci_command_table_t*)(unsigned long long)paging_device_alloc(cmdTablePage, cmdTablePage);
    memset(cmdTable, 0, PAGE_SIZE_4K);

    // uint32_t cmdTable2Page = pmm_pop_frame_nonlong();
    uint32_t cmdTable2Page = 0;
    ahci_command_table_t *cmdTable2 = (ahci_command_table_t*)(unsigned long long)paging_device_alloc(cmdTable2Page, cmdTable2Page);
    memset(cmdTable2, 0, PAGE_SIZE_4K);
    // uint32_t ss = sizeof(ahci_received_fis_t);
    
    // uint32_t dataPage = pmm_pop_frame_nonlong();
    uint32_t dataPage = 0;
    uint16_t *dataPtr = (uint16_t*)(unsigned long long)paging_device_alloc(dataPage, dataPage);
    memset(dataPtr, 0, 0x1000);

    // uint32_t data2Page = pmm_pop_frame_nonlong();
    uint32_t data2Page = 0;
    uint16_t *data2Ptr = (uint16_t*)(unsigned long long)paging_device_alloc(data2Page, data2Page);
    memset(data2Ptr, 0, 0x1000);

    ata_identify_result_2_t* ata = (ata_identify_result_2_t*)dataPtr;
    // uint32_t fff = sizeof(ata_identify_result_2_t);

    ahci_port_cmd_stop(hddPort);
   /* hddPort->CommandList[0].CommandTableBaseAddress = cmdTablePage;
    hddPort->CommandList[0].CommandFisLength = 4;
    hddPort->CommandList[0].PhyRegionDescTableLength = 1;
    hddPort->CommandList[0].Reset = true;
    hddPort->CommandList[0].ClearBusyUponOk = true;
    cmdTable->PhysRegionDescTable[0].DataBaseAddress = dataPage;
    cmdTable->PhysRegionDescTable[0].DataByteCount = 0x1000 - 1;*/
    ahci_fis_reg_host_to_device_t *h2d = (ahci_fis_reg_host_to_device_t*)&cmdTable->CommandFis;
    h2d->FisType = 0x27;
    h2d->IsCommand = false;
    h2d->ControlReg = 0x02;

    hddPort->CommandList[1].CommandTableBaseAddress = cmdTable2Page;
    hddPort->CommandList[1].CommandFisLength = 4;
    hddPort->CommandList[1].PhyRegionDescTableLength = 1;
    hddPort->CommandList[1].Reset = false;
    hddPort->CommandList[1].ClearBusyUponOk = false;
    cmdTable2->PhysRegionDescTable[0].DataBaseAddress = data2Page;
    cmdTable2->PhysRegionDescTable[0].DataByteCount = 0x1000 - 1;
    ahci_fis_reg_host_to_device_t *h2d2 = (ahci_fis_reg_host_to_device_t*)&cmdTable->CommandFis;
    h2d2->FisType = 0x27;
    h2d2->IsCommand = false;
    h2d2->ControlReg = 0x00;

    memset(cmdTable, 0, PAGE_SIZE_4K);

    // Print info.
    cprintf("AHCI port ssts 0x%X\n", ahciController->Memory->Ports[0].SataStatus.RawValue);
    cprintf("AHCI: status 0x%X, error 0x%X\n", ahciController->Memory->Ports[0].TaskFileData.Status.RawValue, ahciController->Memory->Ports[0].TaskFileData.Error);
    cprintf("AHCI: Version major 0x%X, minor 0x%X\n", ahciController->Memory->Version.Major, ahciController->Memory->Version.Minor);
    cprintf("AHCI: Total ports: %u (%u enabled)\n", ahciController->PortCount, enabledPorts);

    hddPort->CommandList[0].CommandTableBaseAddress = cmdTablePage;
    hddPort->CommandList[0].CommandFisLength = 4;
    hddPort->CommandList[0].PhyRegionDescTableLength = 1;
    hddPort->CommandList[0].Reset = false;
    hddPort->CommandList[0].ClearBusyUponOk = false;
    cmdTable->PhysRegionDescTable[0].DataBaseAddress = dataPage;
    cmdTable->PhysRegionDescTable[0].DataByteCount = 0x1000 - 1;
    h2d->FisType = 0x27;
    h2d->ControlReg = 0x00;
    h2d->IsCommand = true;
    h2d->CommandReg = 0xEC;
    h2d->Count = 1;
    h2d->Device = 0x40;

    // enable command.
    ahciController->Memory->Ports[0].InterruptsStatus.RawValue = -1;
    ahciController->Memory->Ports[0].SataError.RawValue = -1;
    cprintf("int 0x%X\n", ahciController->Memory->Ports[0].InterruptsEnabled.RawValue);
    ahciController->Memory->Ports[0].SataError.RawValue = -1;

    ahci_port_cmd_start(hddPort);
    ahciController->Memory->Ports[0].CommandsIssued = 1;
    while (true) {
        if ((ahciController->Memory->Ports[0].CommandsIssued & 1) == 0)
            break;

        if ((ahciController->Memory->Ports[0].TaskFileData.Error))
        {
           cprintf("Error: 0x%X\n", ahciController->Memory->Ports[0].TaskFileData.Error);
           break;
        }

        sleep(1000);
        cprintf("AHCI: status 0x%X, error 0x%X\n", ahciController->Memory->Ports[0].TaskFileData.Status.RawValue, ahciController->Memory->Ports[0].TaskFileData.Error);
        cprintf("AHCI: general: 0x%X\n", ata->GeneralConfig);
        cprintf("AHCI: int status 0x%X\n", ahciController->Memory->Ports[0].InterruptsStatus.RawValue);
        cprintf("AHCI sata error 0x%X\n", ahciController->Memory->Ports[0].SataError.RawValue);
        ahciController->Memory->Ports[0].SataError.RawValue = -1;
    }

    char model[ATA_MODEL_LENGTH+1];
    strncpy(model, ata->Model, ATA_MODEL_LENGTH);
    model[ATA_MODEL_LENGTH] = '\0';


    cprintf("AHCI: Model: %s\n", model);
    cprintf("AHCI: Integrity: 0x%X\n", ata->ChecksumValidityIndicator);
    while(true);


}
