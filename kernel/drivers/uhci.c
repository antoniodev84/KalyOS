#include "usb.h"
#define UHCI_DESCRIPTORS 768
struct uhci_td {
    volatile uint32_t link, status;
    uint32_t token, buffer;
} __attribute__((aligned(16)));
struct uhci_qh {
    volatile uint32_t horizontal, element;
    uint32_t padding[2];
} __attribute__((aligned(16)));
struct uhci_pipe {
    struct uhci_qh qh;
    struct uhci_td *descriptors;
    uint8_t *buffer;
    bool active, control;
    unsigned count, first_data, data_count;
    size_t requested;
    struct uhci_pipe *next;
};
struct uhci {
    struct usb_host host;
    uint16_t base;
    struct uhci_qh *head;
    uint32_t *frames;
    unsigned next_address;
    struct uhci_pipe *pipes;
};
static bool connected(struct usb_host *host, unsigned port) {
    struct uhci *h = host->private;
    return (inw(h->base + 0x10 + port * 2) & 1) != 0;
}
static int reset_port(struct usb_host *host, unsigned port, unsigned *speed) {
    struct uhci *h = host->private;
    uint16_t reg = h->base + 0x10 + port * 2;
    uint16_t value = inw(reg) & ~0x0aU;
    if (!(value & 1)) {
        return -19;
    }
    outw(reg, (value | 0x200) & ~4U);
    hw_delay(50000);
    outw(reg, inw(reg) & ~0x20aU);
    hw_delay(10000);
    outw(reg, (inw(reg) & ~0x0aU) | 4);
    hw_delay(10000);
    value = inw(reg);
    if (!(value & 4)) {
        return -EIO;
    }
    *speed = value & 256 ? 2 : 1;
    return 0;
}
static struct uhci_pipe *pipe_create(struct usb_device *device) {
    struct uhci *h = device->host->private;
    uint64_t page = pmm_alloc(), descriptors = pmm_alloc_contiguous(3),
             buffer = pmm_alloc_contiguous(4);
    if (!page || !descriptors || !buffer) {
        if (page) {
            pmm_free(page);
        }
        for (unsigned i = 0; descriptors && i < 3; i++) {
            pmm_free(descriptors + i * PAGE);
        }
        for (unsigned i = 0; buffer && i < 4; i++) {
            pmm_free(buffer + i * PAGE);
        }
        return NULL;
    }
    struct uhci_pipe *p = phys(page);
    p->descriptors = phys(descriptors);
    p->buffer = phys(buffer);
    p->qh.element = 1;
    p->qh.horizontal = h->head->horizontal;
    p->next = h->pipes;
    h->pipes = p;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->head->horizontal = physical(&p->qh) | 2;
    return p;
}
static void descriptor(struct usb_device *device, struct uhci_pipe *pipe, unsigned index,
                       unsigned endpoint, unsigned pid, unsigned toggle, uint8_t *buffer,
                       unsigned length, bool short_detect) {
    struct uhci_td *td = &pipe->descriptors[index];
    td->status = (1U << 23) | (3U << 27) | 0x7ff | (device->speed == 2 ? 1U << 26 : 0) |
                 (short_detect ? 1U << 29 : 0);
    td->token = pid | (device->address << 8) | (endpoint << 15) | (toggle << 19) |
                (((length - 1) & 0x7ff) << 21);
    td->buffer = physical(buffer);
    td->link = physical(&pipe->descriptors[index + 1]) | 4;
}
static void start(struct uhci_pipe *p) {
    p->descriptors[p->count - 1].link = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    p->qh.element = physical(p->descriptors);
    p->active = true;
}
static int finish(struct uhci_pipe *p, bool wait) {
    uint64_t end = hw_time_us() + (wait ? 3000000 : 0);
    do {
        bool pending = false;
        unsigned actual = 0;
        for (unsigned i = 0; i < p->count; i++) {
            struct uhci_td *td = &p->descriptors[i];
            uint32_t status = td->status;
            if (status & (1U << 23)) {
                pending = true;
                break;
            }
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (status & 0x00760000) {
                p->qh.element = 1;
                p->active = false;
                return -EIO;
            }
            unsigned length = ((status & 0x7ff) + 1) & 0x7ff;
            unsigned requested = (((td->token >> 21) & 0x7ff) + 1) & 0x7ff;
            if (i >= p->first_data && i < p->first_data + p->data_count) {
                actual += length;
                if (length < requested) {
                    if (p->control) {
                        unsigned last = p->count - 1;
                        if (p->descriptors[last].status & (1U << 23)) {
                            p->qh.element = physical(&p->descriptors[last]);
                            pending = true;
                        } else if (p->descriptors[last].status & 0x00760000) {
                            p->qh.element = 1;
                            p->active = false;
                            return -EIO;
                        }
                    }
                    break;
                }
            }
        }
        if (!pending) {
            p->qh.element = 1;
            p->active = false;
            return actual;
        }
        if (!wait) {
            return -EAGAIN;
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    p->qh.element = 1;
    p->active = false;
    return -110;
}
static int control(struct usb_device *device, const struct usb_request *request, void *data) {
    struct uhci_pipe *p = device->private;
    if (!p) {
        p = pipe_create(device);
        if (!p) {
            return -ENOMEM;
        }
        device->private = p;
    }
    unsigned length = request->length;
    if (length > 4096) {
        return -EINVAL;
    }
    bool input = request->type & 0x80;
    memcpy(p->buffer, request, 8);
    if (length && !input) {
        memcpy(p->buffer + PAGE, data, length);
    }
    descriptor(device, p, 0, 0, 0x2d, 0, p->buffer, 8, false);
    unsigned count = 1, offset = 0, toggle = 1;
    while (offset < length) {
        unsigned chunk = MIN(length - offset, device->ep0_size);
        descriptor(device, p, count++, 0, input ? 0x69 : 0xe1, toggle, p->buffer + PAGE + offset,
                   chunk, input);
        offset += chunk;
        toggle ^= 1;
    }
    p->first_data = 1;
    p->data_count = count - 1;
    descriptor(device, p, count++, 0, input ? 0xe1 : 0x69, 1, p->buffer, 0, false);
    p->count = count;
    p->control = true;
    p->requested = length;
    start(p);
    int result = finish(p, true);
    if (result >= 0 && input) {
        memcpy(data, p->buffer + PAGE, result);
    }
    return result;
}
static int transfer(struct usb_device *device, struct usb_endpoint *ep, void *data, size_t length,
                    bool wait) {
    if (!device->online || length > 16384) {
        return -19;
    }
    struct uhci_pipe *p = ep->private;
    if (!p) {
        p = pipe_create(device);
        if (!p) {
            return -ENOMEM;
        }
        ep->private = p;
    }
    bool input = ep->address & 128;
    if (!p->active) {
        if (!input) {
            memcpy(p->buffer, data, length);
        }
        unsigned count = 0, offset = 0, toggle = ep->toggle;
        do {
            if (count == UHCI_DESCRIPTORS) {
                return -90;
            }
            unsigned chunk = MIN(length - offset, ep->packet_size);
            descriptor(device, p, count++, ep->address & 15, input ? 0x69 : 0xe1, toggle,
                       p->buffer + offset, chunk, input);
            offset += chunk;
            toggle ^= 1;
        } while (offset < length);
        p->control = false;
        p->first_data = 0;
        p->data_count = p->count = count;
        p->requested = length;
        start(p);
    }
    int result = finish(p, wait);
    if (result >= 0) {
        if ((unsigned)result > length) {
            return -EIO;
        }
        if (input) {
            memcpy(data, p->buffer, result);
        }
        unsigned packets = (result + ep->packet_size - 1) / ep->packet_size;
        if (!result || ((size_t)result < p->requested && result % ep->packet_size == 0)) {
            packets++;
        }
        ep->toggle ^= packets & 1;
    }
    return result;
}
static int address_device(struct usb_device *device) {
    struct uhci *h = device->host->private;
    if (h->next_address == 127) {
        return -ENOSPC;
    }
    unsigned next = ++h->next_address;
    int result = usb_control(device, 0, 5, next, 0, NULL, 0);
    if (result < 0) {
        return result;
    }
    device->address = next;
    hw_delay(2000);
    return 0;
}
static int reset_endpoint(struct usb_device *device, struct usb_endpoint *ep) {
    (void)device;
    struct uhci_pipe *p = ep->private;
    if (p) {
        p->qh.element = 1;
        p->active = false;
    }
    ep->toggle = 0;
    return 0;
}
static void release_pipe(struct uhci *h, struct uhci_pipe *target) {
    if (!target) {
        return;
    }
    struct uhci_pipe **link = &h->pipes, *previous = NULL;
    while (*link && *link != target) {
        previous = *link;
        link = &(*link)->next;
    }
    if (!*link) {
        return;
    }
    *link = target->next;
    if (previous) {
        previous->qh.horizontal = target->qh.horizontal;
    } else {
        h->head->horizontal = target->qh.horizontal;
    }
    for (unsigned i = 0; i < 3; i++) {
        pmm_free(physical(target->descriptors) + i * PAGE);
    }
    for (unsigned i = 0; i < 4; i++) {
        pmm_free(physical(target->buffer) + i * PAGE);
    }
    pmm_free(physical(target));
}
static void stop(struct usb_device *device) {
    struct uhci *h = device->host->private;
    uint16_t saved = inw(h->base);
    outw(h->base, saved & ~1U);
    uint64_t end = hw_time_us() + 1000000;
    while (!(inw(h->base + 2) & 0x20)) {
        if (hw_time_us() >= end) {
            kprintf("UHCI halt failed; DMA resources retained\n");
            outw(h->base, saved);
            return;
        }
    }
    release_pipe(h, device->private);
    device->private = NULL;
    for (unsigned i = 0; i < device->endpoint_count; i++) {
        release_pipe(h, device->endpoints[i].private);
        device->endpoints[i].private = NULL;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    outw(h->base, saved);
}
void uhci_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    uint32_t bar = pci_read(bus, dev, fn, 0x20);
    if (!(bar & 1) || !(bar & ~3U) || (bar & ~3U) > 65503) {
        return;
    }
    uint16_t base = bar & ~3U;
    pci_write(bus, dev, fn, 4, (pci_read(bus, dev, fn, 4) & 65535) | 5 | 0x400);
    outw(base, 0);
    outw(base + 4, 0);
    outw(base, 2);
    uint64_t end = hw_time_us() + 1000000;
    while (inw(base) & 2) {
        if (hw_time_us() >= end) {
            return;
        }
    }
    struct uhci *h = kalloc(sizeof(*h));
    uint64_t frames = pmm_alloc(), head = pmm_alloc();
    if (!h || !frames || !head) {
        kfree(h);
        if (frames) {
            pmm_free(frames);
        }
        if (head) {
            pmm_free(head);
        }
        return;
    }
    h->base = base;
    h->frames = phys(frames);
    h->head = phys(head);
    h->head->horizontal = h->head->element = 1;
    for (unsigned i = 0; i < 1024; i++) {
        h->frames[i] = head | 2;
    }
    outl(base + 8, frames);
    outw(base + 6, 0);
    outb(base + 12, 64);
    outw(base + 2, 0xffff);
    outw(base, 0xc1);
    h->host.private = h;
    h->host.ports = 2;
    h->host.connected = connected;
    h->host.reset_port = reset_port;
    h->host.address_device = address_device;
    h->host.control = control;
    h->host.transfer = transfer;
    h->host.reset_endpoint = reset_endpoint;
    h->host.stop = stop;
    usb_register_host(&h->host);
    kprintf("UHCI companion ready\n");
}
