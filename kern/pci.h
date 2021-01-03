#ifndef JOS_KERN_PCI_H
#define JOS_KERN_PCI_H

#include <inc/types.h>

#define PCI_BAR_COUNT				6

// PCI subsystem interface
enum { pci_res_bus, pci_res_mem, pci_res_io, pci_res_max };

struct pci_bus;

typedef struct {
    bool PortMapped;
    bool AddressIs64bits;
    bool Prefetchable;

    uint32_t BaseAddress;
} pci_base_register_t;

struct pci_func {
    struct pci_bus *bus;	// Primary bus for bridges

    uint64_t dev;
    uint64_t func;

    uint64_t dev_id;
    uint64_t dev_class; // contain class and subclass
    // uint8_t Subclass;

    uint64_t reg_base[6]; 
    uint64_t reg_size[6];
    uint8_t irq_line;

    uint16_t VendorId;


    struct pci_func *Parent;
    struct pci_func *Next;

    
    uint8_t Interface;

    pci_base_register_t BaseAddresses[PCI_BAR_COUNT];


    // Interrupt handler.
    bool (*InterruptHandler)(struct pci_func *pciDevice);
    void *DriverObject;
};

struct pci_bus {
    struct pci_func *parent_bridge;
    uint64_t busno;
};

int  pci_init(void);
void pci_func_enable(struct pci_func *f);

#endif
