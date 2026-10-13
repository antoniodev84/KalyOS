#include "usb.h"
#define XHCI_RING_ENTRIES 256
struct xhci_trb {
    uint64_t parameter;
    uint32_t status;
    volatile uint32_t control;
} __attribute__((aligned(16)));
struct xhci_ring {
    struct xhci_trb *trbs;
    unsigned enqueue, cycle;
};
struct xhci_pipe {
    struct xhci_ring ring;
    uint8_t *buffer;
    unsigned dci;
    bool active, complete, control_transfer;
    uint64_t data_pointer;
    size_t requested;
    unsigned completion, residual;
};
struct xhci_device {
    uint8_t *input, *output;
    struct xhci_pipe ep0;
    struct xhci_pipe *pipes[32];
};
struct xhci {
    struct usb_host host;
    volatile uint8_t *capability;
    volatile uint32_t *operational, *doorbells, *interrupter;
    struct xhci_ring command;
    struct xhci_trb *events;
    uint64_t *dcbaa, *event_segments, *scratchpads;
    unsigned event_index, event_cycle, context_size, max_slots, scratchpad_count;
    uint64_t command_pointer;
    unsigned command_completion, command_slot;
    bool command_complete;
    struct usb_device *devices[256];
};
static int ring_init(struct xhci_ring *ring) {
    uint64_t page = pmm_alloc();
    if (!page) {
        return -ENOMEM;
    }
    ring->trbs = phys(page);
    ring->cycle = 1;
    ring->trbs[XHCI_RING_ENTRIES - 1] = (struct xhci_trb){page, 0, (6U << 10) | 2};
    return 0;
}
static uint64_t ring_push(struct xhci_ring *ring, uint64_t parameter, uint32_t status,
                          uint32_t control) {
    struct xhci_trb *t = &ring->trbs[ring->enqueue];
    t->parameter = parameter;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->control = control | ring->cycle;
    uint64_t pointer = physical(t);
    if (++ring->enqueue == XHCI_RING_ENTRIES - 1) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        ring->trbs[XHCI_RING_ENTRIES - 1].control = (6U << 10) | 2 | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    return pointer;
}
static void process_events(struct xhci *h) {
    for (unsigned count = 0; count < XHCI_RING_ENTRIES; count++) {
        struct xhci_trb *t = &h->events[h->event_index];
        uint32_t control = t->control;
        if ((control & 1) != h->event_cycle) {
            break;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        unsigned type = (control >> 10) & 63;
        unsigned completion = t->status >> 24;
        if (type == 33 && (t->parameter & ~15UL) == h->command_pointer) {
            h->command_completion = completion;
            h->command_slot = control >> 24;
            h->command_complete = true;
        } else if (type == 32) {
            unsigned slot = control >> 24, dci = (control >> 16) & 31;
            struct usb_device *device = h->devices[slot];
            if (device && device->private) {
                struct xhci_device *d = device->private;
                struct xhci_pipe *pipe = d->pipes[dci];
                if (pipe && pipe->active) {
                    if (pipe->control_transfer && completion == 13 &&
                        (t->parameter & ~15UL) == pipe->data_pointer) {
                        pipe->residual = t->status & 0xffffff;
                    } else {
                        pipe->completion = completion;
                        if (!pipe->control_transfer) {
                            pipe->residual = t->status & 0xffffff;
                        }
                        pipe->complete = true;
                    }
                }
            }
        }
        if (++h->event_index == XHCI_RING_ENTRIES) {
            h->event_index = 0;
            h->event_cycle ^= 1;
        }
        *(volatile uint64_t *)(h->interrupter + 6) = physical(&h->events[h->event_index]) | 8;
    }
}
static int command(struct xhci *h, unsigned type, uint64_t parameter, uint32_t flags) {
    h->command_complete = false;
    h->command_pointer = ring_push(&h->command, parameter, 0, (type << 10) | flags);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->doorbells[0] = 0;
    uint64_t end = hw_time_us() + 1000000;
    do {
        process_events(h);
        if (h->command_complete) {
            return h->command_completion == 1 ? 0 : -EIO;
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    return -110;
}
static uint32_t *context(struct xhci *h, struct xhci_device *d, unsigned index) {
    return (void *)(d->input + index * h->context_size);
}
static int pipe_init(struct xhci_pipe *p, unsigned dci) {
    int error = ring_init(&p->ring);
    if (error) {
        return error;
    }
    uint64_t buffer = pmm_alloc_contiguous(4);
    if (!buffer) {
        pmm_free(physical(p->ring.trbs));
        return -ENOMEM;
    }
    p->buffer = phys(buffer);
    p->dci = dci;
    return 0;
}
static bool connected(struct usb_host *host, unsigned port) {
    struct xhci *h = host->private;
    return (h->operational[0x400 / 4 + port * 4] & 1) != 0;
}
static int reset_port(struct usb_host *host, unsigned port, unsigned *speed) {
    struct xhci *h = host->private;
    volatile uint32_t *p = &h->operational[0x400 / 4 + port * 4];
    if (!(*p & 1)) {
        return -19;
    }
    *p = (*p & ~0x00fe0002U) | (1U << 9);
    hw_delay(20000);
    unsigned port_speed = (*p >> 10) & 15;
    if (port_speed != 4 || !(*p & 2)) {
        *p = (*p & ~0x00fe0002U) | (port_speed == 4 ? 1U << 31 : 1U << 4);
        if (hw_wait32(p, (1U << 4) | (1U << 31), 0, 1000000)) {
            return -110;
        }
    }
    hw_delay(10000);
    if (!(*p & 2)) {
        return -EIO;
    }
    *speed = (*p >> 10) & 15;
    return *speed >= 1 && *speed <= 4 ? 0 : -95;
}
static int address_device(struct usb_device *device) {
    struct xhci *h = device->host->private;
    int error = command(h, 9, 0, 0);
    if (error) {
        return error;
    }
    unsigned slot = h->command_slot;
    if (!slot || slot > h->max_slots) {
        return -EIO;
    }
    struct xhci_device *d = kalloc(sizeof(*d));
    uint64_t input = pmm_alloc(), output = pmm_alloc();
    if (!d || !input || !output) {
        kfree(d);
        if (input) {
            pmm_free(input);
        }
        if (output) {
            pmm_free(output);
        }
        command(h, 10, 0, slot << 24);
        return -ENOMEM;
    }
    d->input = phys(input);
    d->output = phys(output);
    error = pipe_init(&d->ep0, 1);
    if (error) {
        pmm_free(input);
        pmm_free(output);
        kfree(d);
        command(h, 10, 0, slot << 24);
        return error;
    }
    d->pipes[1] = &d->ep0;
    device->slot = slot;
    device->private = d;
    h->devices[slot] = device;
    h->dcbaa[slot] = output;
    uint32_t *control = context(h, d, 0);
    control[1] = 3;
    uint32_t *slot_context = context(h, d, 1);
    slot_context[0] = device->route | (device->speed << 20) | (1U << 27);
    slot_context[1] = (device->port + 1) << 16;
    if (device->speed < 3) {
        struct usb_device *tt = device->parent, *child = device;
        while (tt && tt->speed != 3) {
            child = tt;
            tt = tt->parent;
        }
        if (tt) {
            slot_context[2] = tt->slot | (child->hub_port << 8) | (tt->hub_tt_think << 16);
        }
    }
    uint32_t *ep0 = context(h, d, 2);
    ep0[1] = (3U << 1) | (4U << 3) | ((uint32_t)device->ep0_size << 16);
    *(uint64_t *)(ep0 + 2) = physical(d->ep0.ring.trbs) | 1;
    ep0[4] = 8;
    error = command(h, 11, input, slot << 24);
    if (!error) {
        device->address = ((uint32_t *)d->output)[3] & 255;
    }
    return error;
}
static int finish(struct usb_device *device, struct xhci_pipe *p, bool wait) {
    struct xhci *h = device->host->private;
    uint64_t end = hw_time_us() + (wait ? 3000000 : 0);
    do {
        process_events(h);
        if (p->complete) {
            p->active = false;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (p->completion != 1 && p->completion != 13) {
                return -EIO;
            }
            if (p->residual > p->requested) {
                return -EIO;
            }
            return p->requested - p->residual;
        }
        if (!wait) {
            return -EAGAIN;
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    command(h, 15, 0, (device->slot << 24) | (p->dci << 16));
    p->active = false;
    return -110;
}
static int control(struct usb_device *device, const struct usb_request *request, void *data) {
    struct xhci *h = device->host->private;
    struct xhci_device *d = device->private;
    if (!d || request->length > 16384) {
        return -EINVAL;
    }
    struct xhci_pipe *p = &d->ep0;
    uint32_t *output_ep0 = (void *)(d->output + h->context_size);
    unsigned state = output_ep0[0] & 7;
    if (state == 2 || state == 3) {
        int error = 0;
        if (state == 2) {
            error = command(h, 14, 0, (device->slot << 24) | (1U << 16));
        }
        if (!error) {
            uint64_t pointer = physical(&p->ring.trbs[p->ring.enqueue]) | p->ring.cycle;
            error = command(h, 16, pointer, (device->slot << 24) | (1U << 16));
        }
        if (error) {
            return error;
        }
    }
    if ((output_ep0[1] >> 16) != device->ep0_size) {
        memset(d->input, 0, PAGE);
        context(h, d, 0)[1] = 2;
        memcpy(context(h, d, 2), output_ep0, h->context_size);
        context(h, d, 2)[1] = (context(h, d, 2)[1] & 65535) | ((uint32_t)device->ep0_size << 16);
        int error = command(h, 13, physical(d->input), device->slot << 24);
        if (error) {
            return error;
        }
    }
    bool input = request->type & 0x80;
    unsigned length = request->length;
    if (!input && length) {
        memcpy(p->buffer, data, length);
    }
    uint64_t setup;
    memcpy(&setup, request, 8);
    unsigned transfer_type = length ? input ? 3 : 2 : 0;
    ring_push(&p->ring, setup, 8, (2U << 10) | (1U << 6) | (transfer_type << 16));
    if (length) {
        p->data_pointer = ring_push(&p->ring, physical(p->buffer), length,
                                    (3U << 10) | (1U << 2) | (input ? 1U << 16 : 0));
    }
    ring_push(&p->ring, 0, 0, (4U << 10) | (1U << 5) | (!length || !input ? 1U << 16 : 0));
    p->requested = length;
    p->residual = 0;
    p->control_transfer = true;
    p->active = true;
    p->complete = false;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->doorbells[device->slot] = 1;
    int result = finish(device, p, true);
    if (result >= 0 && input) {
        memcpy(data, p->buffer, result);
    }
    return result;
}
static unsigned interval(unsigned speed, struct usb_endpoint *ep) {
    if (ep->type != 3) {
        return 0;
    }
    if (speed >= 3) {
        return MIN(MAX(ep->interval, 1), 16) - 1;
    }
    unsigned value = MAX(ep->interval, 1), exponent = 0;
    while ((1U << exponent) < value && exponent < 7) {
        exponent++;
    }
    return exponent + 3;
}
static int configure(struct usb_device *device) {
    struct xhci *h = device->host->private;
    struct xhci_device *d = device->private;
    memset(d->input, 0, PAGE);
    memcpy(context(h, d, 1), d->output, h->context_size);
    context(h, d, 0)[1] = 1;
    unsigned highest = 1;
    for (unsigned i = 0; i < device->endpoint_count; i++) {
        struct usb_endpoint *ep = &device->endpoints[i];
        unsigned dci = (ep->address & 15) * 2 + !!(ep->address & 0x80);
        if (!dci || dci >= 32 || d->pipes[dci]) {
            return -EINVAL;
        }
        struct xhci_pipe *p = kalloc(sizeof(*p));
        if (!p) {
            return -ENOMEM;
        }
        int error = pipe_init(p, dci);
        if (error) {
            kfree(p);
            return error;
        }
        ep->private = p;
        d->pipes[dci] = p;
        context(h, d, 0)[1] |= 1U << dci;
        uint32_t *ctx = context(h, d, dci + 1);
        unsigned type = ep->type == 2 ? 2 : ep->type == 3 ? 3 : 1;
        if (ep->address & 0x80) {
            type += 4;
        }
        ctx[0] = interval(device->speed, ep) << 16;
        ctx[1] = (3U << 1) | (type << 3) | ((uint32_t)ep->packet_size << 16);
        *(uint64_t *)(ctx + 2) = physical(p->ring.trbs) | 1;
        ctx[4] = ep->type == 3 ? ((uint32_t)ep->packet_size << 16) | ep->packet_size : 1024;
        highest = MAX(highest, dci);
    }
    context(h, d, 1)[0] = (context(h, d, 1)[0] & ~(31U << 27)) | (highest << 27);
    return command(h, 12, physical(d->input), device->slot << 24);
}
static int configure_hub(struct usb_device *device) {
    struct xhci *h = device->host->private;
    struct xhci_device *d = device->private;
    memset(d->input, 0, PAGE);
    context(h, d, 0)[1] = 1;
    uint32_t *slot = context(h, d, 1);
    memcpy(slot, d->output, h->context_size);
    slot[0] |= (1U << 26) | (device->multi_tt ? 1U << 25 : 0);
    slot[1] = (slot[1] & 0x00ffffff) | (device->hub_ports << 24);
    slot[2] = (slot[2] & ~(3U << 16)) | (device->hub_tt_think << 16);
    return command(h, 13, physical(d->input), device->slot << 24);
}
static int transfer(struct usb_device *device, struct usb_endpoint *ep, void *data, size_t length,
                    bool wait) {
    struct xhci *h = device->host->private;
    struct xhci_pipe *p = ep->private;
    if (!device->online || !p || length > 16384) {
        return -19;
    }
    bool input = ep->address & 0x80;
    if (!p->active) {
        if (!input) {
            memcpy(p->buffer, data, length);
        }
        p->requested = length;
        p->control_transfer = false;
        p->complete = false;
        p->active = true;
        ring_push(&p->ring, physical(p->buffer), length, (1U << 10) | (1U << 5) | (1U << 2));
        __atomic_thread_fence(__ATOMIC_RELEASE);
        h->doorbells[device->slot] = p->dci;
    }
    int result = finish(device, p, wait);
    if (result >= 0 && input) {
        memcpy(data, p->buffer, result);
    }
    return result;
}
static int reset_endpoint(struct usb_device *device, struct usb_endpoint *ep) {
    struct xhci *h = device->host->private;
    struct xhci_pipe *p = ep->private;
    struct xhci_device *d = device->private;
    if (!p || !d) {
        return -EINVAL;
    }
    uint32_t *out = (void *)(d->output + p->dci * h->context_size);
    int error;
    if ((out[0] & 7) == 2) {
        error = command(h, 14, 0, (device->slot << 24) | (p->dci << 16));
    } else {
        error = command(h, 15, 0, (device->slot << 24) | (p->dci << 16));
    }
    if (error) {
        return error;
    }
    p->active = false;
    p->complete = false;
    uint64_t pointer = physical(&p->ring.trbs[p->ring.enqueue]) | p->ring.cycle;
    return command(h, 16, pointer, (device->slot << 24) | (p->dci << 16));
}
static void stop(struct usb_device *device) {
    struct xhci *h = device->host->private;
    if (device->slot) {
        if (command(h, 10, 0, device->slot << 24)) {
            kprintf("xHCI slot disable failed; DMA resources retained\n");
            return;
        }
        h->dcbaa[device->slot] = 0;
        h->devices[device->slot] = NULL;
        struct xhci_device *d = device->private;
        if (d) {
            for (unsigned i = 0; i < 32; i++) {
                struct xhci_pipe *p = d->pipes[i];
                if (!p) {
                    continue;
                }
                pmm_free(physical(p->ring.trbs));
                for (unsigned page = 0; page < 4; page++) {
                    pmm_free(physical(p->buffer) + page * PAGE);
                }
                if (p != &d->ep0) {
                    kfree(p);
                }
            }
            pmm_free(physical(d->input));
            pmm_free(physical(d->output));
            kfree(d);
            device->private = NULL;
            for (unsigned i = 0; i < device->endpoint_count; i++) {
                device->endpoints[i].private = NULL;
            }
        }
    }
}
static void release_host(struct xhci *h) {
    h->operational[0] = 0;
    if (hw_wait32(&h->operational[1], 1, 1, 1000000)) {
        kprintf("xHCI halt failed; DMA resources retained\n");
        return;
    }
    if (h->scratchpads) {
        for (unsigned i = 0; i < h->scratchpad_count; i++) {
            if (h->scratchpads[i]) {
                pmm_free(h->scratchpads[i]);
            }
        }
        pmm_free(physical(h->scratchpads));
    }
    if (h->dcbaa) {
        pmm_free(physical(h->dcbaa));
    }
    if (h->events) {
        pmm_free(physical(h->events));
    }
    if (h->event_segments) {
        pmm_free(physical(h->event_segments));
    }
    if (h->command.trbs) {
        pmm_free(physical(h->command.trbs));
    }
    kfree(h);
}
void xhci_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    volatile uint8_t *cap = pci_mmio(bus, dev, fn, 0, 0x10000);
    if (!cap || cap[0] < 0x20) {
        return;
    }
    uint32_t hcs1 = *(volatile uint32_t *)(cap + 4);
    uint32_t hcs2 = *(volatile uint32_t *)(cap + 8);
    uint32_t hcc = *(volatile uint32_t *)(cap + 16);
    unsigned extended = (hcc >> 16) * 4;
    for (unsigned count = 0; extended && extended + 8 < 0x10000 && count < 64; count++) {
        volatile uint32_t *r = (void *)(cap + extended);
        uint32_t value = *r;
        if ((value & 255) == 1) {
            *r = value | (1U << 24);
            if (hw_wait32(r, 1U << 16, 0, 1000000)) {
                return;
            }
            r[1] = 0;
        }
        unsigned next = (value >> 8) & 255;
        extended = next ? extended + next * 4 : 0;
    }
    volatile uint32_t *op = (void *)(cap + cap[0]);
    op[0] &= ~1U;
    if (hw_wait32(&op[1], 1, 1, 1000000)) {
        return;
    }
    op[0] |= 2;
    if (hw_wait32(&op[0], 2, 0, 1000000) || hw_wait32(&op[1], 1U << 11, 0, 1000000)) {
        return;
    }
    if (!(op[2] & 1)) {
        return;
    }
    struct xhci *h = kalloc(sizeof(*h));
    if (!h) {
        return;
    }
    h->capability = cap;
    h->operational = op;
    h->context_size = hcc & 4 ? 64 : 32;
    h->max_slots = MIN(hcs1 & 255, 64);
    unsigned doorbell_offset = *(volatile uint32_t *)(cap + 20) & ~3U;
    unsigned runtime_offset = *(volatile uint32_t *)(cap + 24) & ~31U;
    if (doorbell_offset > 0xfe00 || runtime_offset > 0xff00) {
        kfree(h);
        return;
    }
    h->doorbells = (void *)(cap + doorbell_offset);
    h->interrupter = (void *)(cap + runtime_offset + 0x20);
    uint64_t dcbaa = pmm_alloc(), events = pmm_alloc(), segments = pmm_alloc();
    if (!dcbaa || !events || !segments || ring_init(&h->command)) {
        if (dcbaa) {
            pmm_free(dcbaa);
        }
        if (events) {
            pmm_free(events);
        }
        if (segments) {
            pmm_free(segments);
        }
        kfree(h);
        return;
    }
    h->dcbaa = phys(dcbaa);
    h->events = phys(events);
    h->event_segments = phys(segments);
    unsigned scratchpad_count = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (scratchpad_count > PAGE / 8) {
        release_host(h);
        return;
    }
    h->scratchpad_count = scratchpad_count;
    if (scratchpad_count) {
        uint64_t scratchpads = pmm_alloc();
        if (!scratchpads) {
            release_host(h);
            return;
        }
        h->scratchpads = phys(scratchpads);
        for (unsigned i = 0; i < scratchpad_count; i++) {
            h->scratchpads[i] = pmm_alloc();
            if (!h->scratchpads[i]) {
                release_host(h);
                return;
            }
        }
        h->dcbaa[0] = scratchpads;
    }
    h->event_segments[0] = events;
    h->event_segments[1] = XHCI_RING_ENTRIES;
    h->event_cycle = 1;
    *(volatile uint64_t *)(op + 6) = physical(h->command.trbs) | 1;
    *(volatile uint64_t *)(op + 12) = dcbaa;
    op[14] = h->max_slots;
    h->interrupter[0] = 0;
    h->interrupter[1] = 0;
    h->interrupter[2] = 1;
    *(volatile uint64_t *)(h->interrupter + 4) = segments;
    *(volatile uint64_t *)(h->interrupter + 6) = events;
    op[0] = 1;
    if (hw_wait32(&op[1], 1, 0, 1000000)) {
        release_host(h);
        return;
    }
    h->host.private = h;
    h->host.ports = hcs1 >> 24;
    h->host.reset_port = reset_port;
    h->host.connected = connected;
    h->host.address_device = address_device;
    h->host.configure = configure;
    h->host.configure_hub = configure_hub;
    h->host.control = control;
    h->host.transfer = transfer;
    h->host.stop = stop;
    h->host.reset_endpoint = reset_endpoint;
    usb_register_host(&h->host);
    kprintf("xHCI %u ports ready\n", (uint64_t)h->host.ports);
}
