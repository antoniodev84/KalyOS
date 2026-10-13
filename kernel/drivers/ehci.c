#include "usb.h"
struct ehci_td {
    volatile uint32_t next, alternate, token;
    uint32_t buffer[5], extended[5], padding[3];
} __attribute__((aligned(32)));
struct ehci_qh {
    volatile uint32_t horizontal, endpoint, capabilities, current;
    struct {
        volatile uint32_t next, alternate, token;
        uint32_t buffer[5], extended[5];
    } overlay;
    uint32_t padding[3];
} __attribute__((aligned(32)));
struct ehci_pipe {
    struct ehci_qh qh;
    struct ehci_td td[3];
    uint8_t *buffer;
    bool active;
    size_t requested;
    struct ehci_pipe *next;
};
struct ehci {
    struct usb_host host;
    volatile uint32_t *registers;
    struct ehci_qh *head;
    uint32_t *periodic;
    struct ehci_pipe *pipes, *interrupt_pipes;
    unsigned next_address;
};
static uint32_t address(const void *p) {
    return physical(p);
}
static struct ehci_pipe *pipe_create(struct usb_device *device, struct usb_endpoint *ep) {
    struct ehci *h = device->host->private;
    uint64_t page = pmm_alloc();
    uint64_t buffer = pmm_alloc_contiguous(5);
    if (!page || !buffer) {
        if (page) {
            pmm_free(page);
        }
        for (unsigned i = 0; buffer && i < 5; i++) {
            pmm_free(buffer + i * PAGE);
        }
        return NULL;
    }
    struct ehci_pipe *p = phys(page);
    p->buffer = phys(buffer);
    unsigned endpoint = ep ? ep->address & 15 : 0;
    unsigned packet_size = ep ? ep->packet_size : device->ep0_size;
    unsigned speed = device->speed == 3 ? 2 : device->speed == 2 ? 1 : 0;
    p->qh.endpoint = device->address | (endpoint << 8) | (speed << 12) | (1U << 14) |
                     (packet_size << 16) | (8U << 28) |
                     (!endpoint && device->speed != 3 ? 1U << 27 : 0);
    p->qh.capabilities = (1U << 30) | (ep && ep->type == 3 ? 1 : 0);
    if (device->speed != 3) {
        struct usb_device *tt = device->parent, *child = device;
        while (tt && tt->speed != 3) {
            child = tt;
            tt = tt->parent;
        }
        if (!tt) {
            for (unsigned i = 0; i < 5; i++) {
                pmm_free(buffer + i * PAGE);
            }
            pmm_free(page);
            return NULL;
        }
        p->qh.capabilities |= (tt->address << 16) | (child->hub_port << 23) | 0x1c01;
    }
    p->qh.overlay.next = p->qh.overlay.alternate = 1;
    if (ep && ep->type == 3) {
        p->next = h->interrupt_pipes;
        h->interrupt_pipes = p;
        p->qh.horizontal = p->next ? address(&p->next->qh) | 2 : 1;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        for (unsigned i = 0; i < 1024; i++) {
            h->periodic[i] = address(&p->qh) | 2;
        }
    } else {
        p->next = h->pipes;
        h->pipes = p;
        p->qh.horizontal = h->head->horizontal;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        h->head->horizontal = address(&p->qh) | 2;
    }
    return p;
}
static void td_init(struct ehci_td *td, uint8_t *buffer, size_t length, unsigned pid,
                    unsigned toggle) {
    memset(td, 0, sizeof(*td));
    td->next = td->alternate = 1;
    td->token = (toggle << 31) | (length << 16) | (3U << 10) | (pid << 8) | 0x80;
    td->buffer[0] = address(buffer);
    for (unsigned i = 1; i < 5; i++) {
        td->buffer[i] = (address(buffer) & ~0xfffU) + i * PAGE;
    }
}
static void start(struct ehci_pipe *p) {
    p->qh.overlay.token = 0;
    p->qh.overlay.alternate = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->qh.overlay.next = address(p->td);
    p->active = true;
}
static int finish(struct ehci_pipe *p, unsigned last, unsigned timeout) {
    uint64_t end = hw_time_us() + timeout;
    do {
        uint32_t token = p->td[last].token;
        if (!(token & 0x80)) {
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            p->active = false;
            return token & 0x7c ? -EIO : 0;
        }
        if (p->qh.overlay.token & 0x7c) {
            p->active = false;
            return -EIO;
        }
        if (!timeout) {
            return -EAGAIN;
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    p->qh.overlay.next = 1;
    p->qh.overlay.token = 0x40;
    p->active = false;
    return -110;
}
static int control(struct usb_device *device, const struct usb_request *request, void *data) {
    struct ehci_pipe *p = device->private;
    if (!p) {
        p = pipe_create(device, NULL);
        if (!p) {
            return -ENOMEM;
        }
        device->private = p;
    }
    size_t length = request->length;
    if (length > 16384) {
        return -EINVAL;
    }
    p->qh.endpoint = (p->qh.endpoint & ~0x7fU) | device->address;
    p->qh.endpoint = (p->qh.endpoint & ~(0x7ffU << 16)) | ((uint32_t)device->ep0_size << 16);
    memcpy(p->buffer, request, 8);
    bool input = request->type & 0x80;
    if (length && !input) {
        memcpy(p->buffer + PAGE, data, length);
    }
    td_init(&p->td[0], p->buffer, 8, 2, 0);
    unsigned last = length ? 2 : 1;
    p->td[0].next = address(&p->td[1]);
    if (length) {
        td_init(&p->td[1], p->buffer + PAGE, length, input ? 1 : 0, 1);
        p->td[1].next = p->td[1].alternate = address(&p->td[2]);
    }
    td_init(&p->td[last], p->buffer, 0, input ? 0 : 1, 1);
    start(p);
    int error = finish(p, last, 1000000);
    if (error) {
        return error;
    }
    unsigned actual = length ? length - ((p->td[1].token >> 16) & 0x7fff) : 0;
    if (length && input) {
        memcpy(data, p->buffer + PAGE, actual);
    }
    return actual;
}
static int transfer(struct usb_device *device, struct usb_endpoint *ep, void *data, size_t length,
                    bool wait) {
    if (!device->online || length > 16384) {
        return -19;
    }
    struct ehci_pipe *p = ep->private;
    if (!p) {
        p = pipe_create(device, ep);
        if (!p) {
            return -ENOMEM;
        }
        ep->private = p;
    }
    bool input = ep->address & 0x80;
    if (!p->active) {
        if (!input) {
            memcpy(p->buffer, data, length);
        }
        td_init(&p->td[0], p->buffer, length, input ? 1 : 0, ep->toggle);
        p->requested = length;
        start(p);
    }
    int error = finish(p, 0, wait ? 3000000 : 0);
    if (error) {
        return error;
    }
    size_t actual = p->requested - ((p->td[0].token >> 16) & 0x7fff);
    if (actual > length) {
        return -EIO;
    }
    if (input) {
        memcpy(data, p->buffer, actual);
    }
    unsigned packets = (actual + ep->packet_size - 1) / ep->packet_size;
    if (!actual || (actual < p->requested && actual % ep->packet_size == 0)) {
        packets++;
    }
    ep->toggle ^= packets & 1;
    return actual;
}
static bool connected(struct usb_host *host, unsigned port) {
    struct ehci *h = host->private;
    return (h->registers[0x44 / 4 + port] & ((1U << 13) | 1)) == 1;
}
static int reset_port(struct usb_host *host, unsigned port, unsigned *speed) {
    struct ehci *h = host->private;
    volatile uint32_t *p = &h->registers[0x44 / 4 + port];
    uint32_t value = *p & ~0x2aU;
    if (!(value & 1)) {
        return -19;
    }
    *p = (value | (1U << 8) | (1U << 12)) & ~4U;
    hw_delay(50000);
    *p = (*p & ~(0x2aU | (1U << 8)));
    hw_delay(20000);
    if (!(*p & 4)) {
        *p = (*p & ~0x2aU) | (1U << 13);
        return -95;
    }
    *speed = 3;
    return 0;
}
static int address_device(struct usb_device *device) {
    struct ehci *h = device->host->private;
    if (h->next_address >= 127) {
        return -ENOSPC;
    }
    unsigned next = ++h->next_address;
    int error = usb_control(device, 0, 5, next, 0, NULL, 0);
    if (error < 0) {
        return error;
    }
    device->address = next;
    hw_delay(2000);
    return 0;
}
static int reset_endpoint(struct usb_device *device, struct usb_endpoint *ep) {
    (void)device;
    struct ehci_pipe *p = ep->private;
    if (p) {
        p->qh.overlay.next = p->qh.overlay.alternate = 1;
        p->qh.overlay.token = 0;
        p->active = false;
    }
    ep->toggle = 0;
    return 0;
}
static void release_pipe(struct ehci *h, struct ehci_pipe *target) {
    if (!target) {
        return;
    }
    struct ehci_pipe **lists[] = {&h->pipes, &h->interrupt_pipes};
    for (unsigned kind = 0; kind < 2; kind++) {
        struct ehci_pipe **link = lists[kind], *previous = NULL;
        while (*link && *link != target) {
            previous = *link;
            link = &(*link)->next;
        }
        if (!*link) {
            continue;
        }
        *link = target->next;
        if (previous) {
            previous->qh.horizontal = target->qh.horizontal;
        } else if (!kind) {
            h->head->horizontal = target->qh.horizontal;
        }
        if (kind) {
            for (unsigned i = 0; i < 1024; i++) {
                h->periodic[i] = h->interrupt_pipes ? physical(&h->interrupt_pipes->qh) | 2 : 1;
            }
        }
        for (unsigned i = 0; i < 5; i++) {
            pmm_free(physical(target->buffer) + i * PAGE);
        }
        pmm_free(physical(target));
        return;
    }
}
static void stop(struct usb_device *device) {
    struct ehci *h = device->host->private;
    uint32_t saved = h->registers[0];
    h->registers[0] = saved & ~0x30U;
    if (hw_wait32(&h->registers[1], (1U << 14) | (1U << 15), 0, 1000000)) {
        kprintf("EHCI schedule halt failed; DMA resources retained\n");
        h->registers[0] = saved;
        return;
    }
    release_pipe(h, device->private);
    device->private = NULL;
    for (unsigned i = 0; i < device->endpoint_count; i++) {
        release_pipe(h, device->endpoints[i].private);
        device->endpoints[i].private = NULL;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->registers[0] = saved;
}
void ehci_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    volatile uint8_t *cap = pci_mmio(bus, dev, fn, 0, 0x1000);
    if (!cap || cap[0] < 0x10) {
        return;
    }
    unsigned extended = (*(volatile uint32_t *)(cap + 8) >> 8) & 255;
    for (unsigned count = 0; extended >= 0x40 && count < 32; count++) {
        uint32_t legacy = pci_read(bus, dev, fn, extended);
        if ((legacy & 255) == 1) {
            pci_write(bus, dev, fn, extended, legacy | (1U << 24));
            uint64_t end = hw_time_us() + 1000000;
            while (pci_read(bus, dev, fn, extended) & (1U << 16)) {
                if (hw_time_us() >= end) {
                    return;
                }
            }
            pci_write(bus, dev, fn, extended + 4, 0);
        }
        extended = (legacy >> 8) & 255;
    }
    volatile uint32_t *r = (void *)(cap + cap[0]);
    r[0] = 0;
    if (hw_wait32(&r[1], 1U << 12, 1U << 12, 1000000)) {
        return;
    }
    r[0] = 2;
    if (hw_wait32(&r[0], 2, 0, 1000000)) {
        return;
    }
    struct ehci *h = kalloc(sizeof(*h));
    uint64_t head = pmm_alloc(), periodic = pmm_alloc();
    if (!h || !head || !periodic) {
        kfree(h);
        if (head) {
            pmm_free(head);
        }
        if (periodic) {
            pmm_free(periodic);
        }
        return;
    }
    h->registers = r;
    h->head = phys(head);
    h->periodic = phys(periodic);
    h->head->horizontal = head | 2;
    h->head->endpoint = (1U << 15) | (2U << 12);
    h->head->overlay.next = h->head->overlay.alternate = 1;
    for (unsigned i = 0; i < 1024; i++) {
        h->periodic[i] = 1;
    }
    r[2] = 0;
    r[4] = 0;
    r[5] = periodic;
    r[6] = head;
    r[0x40 / 4] = 1;
    r[1] = 0x3f;
    r[0] = (8U << 16) | 0x31;
    if (hw_wait32(&r[1], 1U << 12, 0, 1000000)) {
        r[0] = 0;
        if (!hw_wait32(&r[1], 1U << 12, 1U << 12, 1000000)) {
            pmm_free(head);
            pmm_free(periodic);
            kfree(h);
        } else {
            kprintf("EHCI halt failed; DMA resources retained\n");
        }
        return;
    }
    h->host.private = h;
    h->host.ports = *(volatile uint32_t *)(cap + 4) & 15;
    h->host.reset_port = reset_port;
    h->host.connected = connected;
    h->host.address_device = address_device;
    h->host.control = control;
    h->host.transfer = transfer;
    h->host.stop = stop;
    h->host.reset_endpoint = reset_endpoint;
    usb_register_host(&h->host);
    kprintf("EHCI %u ports ready\n", (uint64_t)h->host.ports);
}
