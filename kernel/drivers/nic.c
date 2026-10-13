#include "net.h"
#define RING_SIZE 64
struct intel_descriptor {
    uint64_t address;
    uint16_t length, checksum;
    uint8_t status, errors;
    uint16_t special;
} PACKED;
struct realtek_descriptor {
    volatile uint32_t command, vlan;
    uint64_t address;
} PACKED;
struct nic {
    volatile uint8_t *mmio;
    bool realtek;
    unsigned rx_index, tx_index;
    void *rx_ring, *tx_ring;
    uint8_t *rx_buffers, *tx_buffers;
};
static uint32_t reg32(struct nic *n, unsigned offset) {
    return *(volatile uint32_t *)(n->mmio + offset);
}
static void set32(struct nic *n, unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(n->mmio + offset) = value;
}
static void set16(struct nic *n, unsigned offset, uint16_t value) {
    *(volatile uint16_t *)(n->mmio + offset) = value;
}
static bool link_status(struct netif *interface) {
    struct nic *n = interface->private;
    return n->realtek ? (n->mmio[0x6c] & 2) != 0 : (reg32(n, 8) & 2) != 0;
}
static int transmit(struct netif *interface, const void *data, size_t length) {
    struct nic *n = interface->private;
    if (length > 1514) {
        return -90;
    }
    unsigned index = n->tx_index;
    uint8_t *buffer = n->tx_buffers + index * 2048;
    if (n->realtek) {
        struct realtek_descriptor *d = (struct realtek_descriptor *)n->tx_ring + index;
        if (d->command & (1U << 31)) {
            return -EAGAIN;
        }
        memcpy(buffer, data, length);
        if (length < 60) {
            memset(buffer + length, 0, 60 - length);
        }
        d->vlan = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->command =
            (1U << 31) | (index == RING_SIZE - 1 ? 1U << 30 : 0) | (3U << 28) | MAX(length, 60);
        n->mmio[0x38] = 0x40;
    } else {
        struct intel_descriptor *d = (struct intel_descriptor *)n->tx_ring + index;
        if (!(d->status & 1)) {
            return -EAGAIN;
        }
        memcpy(buffer, data, length);
        d->length = length;
        d->checksum = 0x0b00;
        d->status = 0;
        d->errors = 0;
        d->special = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        set32(n, 0x3818, (index + 1) % RING_SIZE);
    }
    n->tx_index = (index + 1) % RING_SIZE;
    return 0;
}
static int receive(struct netif *interface, void *buffer, size_t capacity) {
    struct nic *n = interface->private;
    unsigned index = n->rx_index;
    unsigned length;
    bool error;
    if (n->realtek) {
        struct realtek_descriptor *d = (struct realtek_descriptor *)n->rx_ring + index;
        uint32_t command = d->command;
        if (command & (1U << 31)) {
            return 0;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        length = command & 0x3fff;
        error = (command & (1U << 21)) || (command & (3U << 28)) != (3U << 28) || length < 4;
        length = length >= 4 ? length - 4 : 0;
        if (!error && length <= capacity) {
            memcpy(buffer, n->rx_buffers + index * 2048, length);
        }
        d->vlan = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        d->command = (1U << 31) | (index == RING_SIZE - 1 ? 1U << 30 : 0) | 2048;
    } else {
        volatile struct intel_descriptor *d = (struct intel_descriptor *)n->rx_ring + index;
        if (!(d->status & 1)) {
            return 0;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        length = d->length;
        error = d->errors || !(d->status & 2);
        if (!error && length <= capacity) {
            memcpy(buffer, n->rx_buffers + index * 2048, length);
        }
        d->status = 0;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        set32(n, 0x2818, index);
    }
    n->rx_index = (index + 1) % RING_SIZE;
    return error || length > capacity ? -EIO : (int)length;
}
static void release(struct nic *n) {
    if (n->rx_ring) {
        pmm_free(physical(n->rx_ring));
    }
    if (n->tx_ring) {
        pmm_free(physical(n->tx_ring));
    }
    for (unsigned i = 0; i < RING_SIZE * 2048 / PAGE; i++) {
        if (n->rx_buffers) {
            pmm_free(physical(n->rx_buffers) + i * PAGE);
        }
        if (n->tx_buffers) {
            pmm_free(physical(n->tx_buffers) + i * PAGE);
        }
    }
    kfree(n);
}
void nic_probe(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t id = pci_read(bus, device, function, 0);
    uint16_t vendor = id, model = id >> 16;
    bool realtek = vendor == 0x10ec && (model == 0x8168 || model == 0x8169);
    bool intel = vendor == 0x8086 && (model == 0x100e || model == 0x100f || model == 0x10d3 ||
                                      model == 0x10ea || model == 0x1502 || model == 0x1503);
    if ((!realtek && !intel) || net_interface_count == NET_INTERFACES) {
        return;
    }
    struct nic *n = kalloc(sizeof(*n));
    if (!n) {
        return;
    }
    n->realtek = realtek;
    n->mmio = pci_mmio(bus, device, function, realtek ? 2 : 0, 0x20000);
    if (!n->mmio && realtek) {
        n->mmio = pci_mmio(bus, device, function, 1, 0x1000);
    }
    if (!n->mmio) {
        kfree(n);
        return;
    }
    n->rx_ring = phys(pmm_alloc());
    n->tx_ring = phys(pmm_alloc());
    n->rx_buffers = phys(pmm_alloc_contiguous(RING_SIZE * 2048 / PAGE));
    n->tx_buffers = phys(pmm_alloc_contiguous(RING_SIZE * 2048 / PAGE));
    if (physical(n->rx_ring) == 0 || physical(n->tx_ring) == 0 || physical(n->rx_buffers) == 0 ||
        physical(n->tx_buffers) == 0) {
        if (!physical(n->rx_ring)) {
            n->rx_ring = NULL;
        }
        if (!physical(n->tx_ring)) {
            n->tx_ring = NULL;
        }
        if (!physical(n->rx_buffers)) {
            n->rx_buffers = NULL;
        }
        if (!physical(n->tx_buffers)) {
            n->tx_buffers = NULL;
        }
        release(n);
        return;
    }
    struct netif interface = {
        .private = n, .send = transmit, .receive = receive, .link_status = link_status, .up = true};
    if (realtek) {
        n->mmio[0x37] = 0x10;
        uint64_t timeout = hw_time_us() + 1000000;
        while (n->mmio[0x37] & 0x10) {
            if (hw_time_us() >= timeout) {
                release(n);
                return;
            }
        }
        n->mmio[0x50] = 0xc0;
        set16(n, 0x3c, 0);
        set16(n, 0x3e, 0xffff);
        set16(n, 0xe0, 0x20);
        for (unsigned i = 0; i < 6; i++) {
            interface.mac[i] = n->mmio[i];
        }
        for (unsigned i = 0; i < RING_SIZE; i++) {
            struct realtek_descriptor *rx = (struct realtek_descriptor *)n->rx_ring + i;
            struct realtek_descriptor *tx = (struct realtek_descriptor *)n->tx_ring + i;
            rx->address = physical(n->rx_buffers + i * 2048);
            rx->command = (1U << 31) | (i == RING_SIZE - 1 ? 1U << 30 : 0) | 2048;
            tx->address = physical(n->tx_buffers + i * 2048);
            tx->command = i == RING_SIZE - 1 ? 1U << 30 : 0;
        }
        set32(n, 0x20, physical(n->tx_ring));
        set32(n, 0x24, 0);
        set32(n, 0xe4, physical(n->rx_ring));
        set32(n, 0xe8, 0);
        n->mmio[0xec] = 0x3f;
        set16(n, 0xda, 1518);
        set32(n, 0x40, 0x03000700);
        set32(n, 0x44, 0x0000e70a);
        n->mmio[0x37] = 0x0c;
        n->mmio[0x50] = 0;
    } else {
        set32(n, 0xd8, 0xffffffff);
        set32(n, 0, reg32(n, 0) | (1U << 26));
        if (hw_wait32((volatile uint32_t *)n->mmio, 1U << 26, 0, 1000000)) {
            release(n);
            return;
        }
        hw_delay(20000);
        set32(n, 0xd8, 0xffffffff);
        set32(n, 0, (reg32(n, 0) & ~((1U << 31) | (1U << 3))) | (1U << 6));
        uint32_t low = reg32(n, 0x5400), high = reg32(n, 0x5404);
        if (!(high & (1U << 31))) {
            kprintf("Intel NIC invalid receive address\n");
            release(n);
            return;
        }
        for (unsigned i = 0; i < 4; i++) {
            interface.mac[i] = low >> (i * 8);
        }
        interface.mac[4] = high;
        interface.mac[5] = high >> 8;
        for (unsigned i = 0; i < 128; i++) {
            set32(n, 0x5200 + i * 4, 0);
        }
        for (unsigned i = 0; i < RING_SIZE; i++) {
            struct intel_descriptor *rx = (struct intel_descriptor *)n->rx_ring + i;
            struct intel_descriptor *tx = (struct intel_descriptor *)n->tx_ring + i;
            rx->address = physical(n->rx_buffers + i * 2048);
            tx->address = physical(n->tx_buffers + i * 2048);
            tx->status = 1;
        }
        set32(n, 0x2800, physical(n->rx_ring));
        set32(n, 0x2804, 0);
        set32(n, 0x2808, RING_SIZE * 16);
        set32(n, 0x2810, 0);
        set32(n, 0x2818, RING_SIZE - 1);
        set32(n, 0x2828, 0x01010000);
        set32(n, 0x3800, physical(n->tx_ring));
        set32(n, 0x3804, 0);
        set32(n, 0x3808, RING_SIZE * 16);
        set32(n, 0x3810, 0);
        set32(n, 0x3818, 0);
        set32(n, 0x3828, 0x01010000);
        set32(n, 0x410, 0x0060200a);
        set32(n, 0x400, 2 | 8 | (15U << 4) | (64U << 12));
        set32(n, 0x100, 2 | (1U << 15) | (1U << 26));
    }
    interface.link = link_status(&interface);
    int index = net_register(&interface);
    if (index < 0) {
        if (realtek) {
            n->mmio[0x37] = 0;
        } else {
            set32(n, 0x100, 0);
            set32(n, 0x400, 0);
        }
        release(n);
        return;
    }
    kprintf("NIC %s %s ready\n", net_interfaces[index].name,
            realtek           ? "RTL8168/8169"
            : model == 0x100e ? "e1000"
                              : "e1000e");
}
