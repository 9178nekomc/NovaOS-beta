/* kernel/pci/pci.h - Nova OS 阶段十九：极简 PCI 枚举接口
 *
 * 配置空间经 0xCF8/0xCFC 端口访问（类型 1）。
 */
#ifndef NOVA_PCI_H
#define NOVA_PCI_H

#include <stdint.h>

#define PCI_VENDOR_INTEL    0x8086u
#define PCI_DEV_E1000_82540 0x100Eu   /* QEMU 默认 e1000 型号 */

struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor;
    uint16_t device;
    uint32_t bar[6];         /* 读到的原始 BAR 值 */
    uint8_t irq;
};

/*
 * 枚举总线 0 的设备，匹配 vendor/device；找到返回 1 并填 *out，
 * 否则返回 0。
 */
int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);

uint32_t pci_read32(const struct pci_dev *d, uint8_t reg);
uint16_t pci_read16(const struct pci_dev *d, uint8_t reg);
void pci_write32(const struct pci_dev *d, uint8_t reg, uint32_t val);
void pci_write16(const struct pci_dev *d, uint8_t reg, uint16_t val);

#endif /* NOVA_PCI_H */
