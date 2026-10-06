/* kernel/pci/pci.c - Nova OS 阶段十九：PCI 枚举实现 */
#include <stdint.h>

#include "pci.h"
#include "../../lib/io.h"

#define PCI_CONFIG_ADDR 0xCF8u
#define PCI_CONFIG_DATA 0xCFCu

static uint32_t pci_make_addr(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t reg)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)func << 8) | (reg & 0xFCu);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t reg)
{
    outl(PCI_CONFIG_ADDR, pci_make_addr(d->bus, d->dev, d->func, reg));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(const struct pci_dev *d, uint8_t reg)
{
    uint32_t v = pci_read32(d, reg & 0xFCu);
    return (uint16_t)((v >> ((reg & 3) * 8)) & 0xFFFF);
}

void pci_write32(const struct pci_dev *d, uint8_t reg, uint32_t val)
{
    outl(PCI_CONFIG_ADDR, pci_make_addr(d->bus, d->dev, d->func, reg));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(const struct pci_dev *d, uint8_t reg, uint16_t val)
{
    uint32_t v = pci_read32(d, reg & 0xFCu);
    uint32_t shift = (reg & 3) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)val << shift);
    pci_write32(d, reg & 0xFCu, v);
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    for (uint8_t bus = 0; bus < 1; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            /* 读 vendor id（寄存器 0x00） */
            outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, 0, 0));
            uint16_t vid = (uint16_t)(inl(PCI_CONFIG_DATA) & 0xFFFF);
            if (vid == 0xFFFF || vid == 0)
                continue;
            /* 读 device id（寄存器 0x02） */
            outl(PCI_CONFIG_ADDR, pci_make_addr(bus, dev, 0, 0x02));
            uint16_t did =
                (uint16_t)((inl(PCI_CONFIG_DATA) >> 16) & 0xFFFF);

            if (vid == vendor && did == device) {
                out->bus = bus;
                out->dev = dev;
                out->func = 0;
                out->vendor = vid;
                out->device = did;
                for (int i = 0; i < 6; i++)
                    out->bar[i] = pci_read32(out, 0x10 + i * 4);
                out->irq = (uint8_t)pci_read32(out, 0x3C);
                return 1;
            }
        }
    }
    return 0;
}
