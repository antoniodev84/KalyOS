#include "net.h"
#include "usb.h"
static struct lock pci_lock;
uint32_t pci_read(uint8_t b, uint8_t d, uint8_t f, uint8_t r) {
    lock(&pci_lock);
    outl(0xcf8,
         0x80000000 | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (r & 252));
    uint32_t v = inl(0xcfc);
    unlock(&pci_lock);
    return v;
}
void pci_write(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint32_t v) {
    lock(&pci_lock);
    outl(0xcf8,
         0x80000000 | ((uint32_t)b << 16) | ((uint32_t)d << 11) | ((uint32_t)f << 8) | (r & 252));
    outl(0xcfc, v);
    unlock(&pci_lock);
}
void pci_init(void) {
    for (unsigned b = 0; b < 256; b++) {
        for (unsigned d = 0; d < 32; d++) {
            for (unsigned f = 0; f < 8; f++) {
                uint32_t id = pci_read(b, d, f, 0);
                if ((id & 65535) == 65535) {
                    if (!f) {
                        break;
                    }
                    continue;
                }
                uint32_t cls = pci_read(b, d, f, 8) >> 8;
                if ((cls >> 8) == 0x0403) {
                    hda_probe(b, d, f);
                }
                if (cls == 0x0c0300) {
                    uhci_probe(b, d, f);
                }
                if (cls == 0x0c0320) {
                    ehci_probe(b, d, f);
                }
                if (cls == 0x0c0330) {
                    xhci_probe(b, d, f);
                }
                if ((cls >> 8) == 0x0200) {
                    nic_probe(b, d, f);
                }
                if (cls == 0x010601) {
                    ahci_probe(b, d, f);
                }
                if (!f && !(pci_read(b, d, 0, 12) & 0x800000)) {
                    break;
                }
            }
        }
    }
}
