#include "usb.h"
static struct usb_host *hosts;
static struct usb_device *devices[USB_MAX_DEVICES];
static struct lock usb_lock;
static unsigned device_count;
void usb_register_host(struct usb_host *host) {
    host->next = hosts;
    hosts = host;
}
bool usb_device_connected(struct usb_device *device) {
    if (!device->online || !device->host->connected(device->host, device->port)) {
        return false;
    }
    return !device->parent || usb_device_connected(device->parent);
}
int usb_control(struct usb_device *device, uint8_t type, uint8_t request, uint16_t value,
                uint16_t index, void *data, uint16_t length) {
    if (!usb_device_connected(device)) {
        return -19;
    }
    struct usb_request setup = {type, request, value, index, length};
    int result = device->host->control(device, &setup, data);
    if (result >= 0 && type == 2 && request == 1 && value == 0) {
        for (unsigned i = 0; i < device->endpoint_count; i++) {
            struct usb_endpoint *ep = &device->endpoints[i];
            if (ep->address == index) {
                ep->toggle = 0;
                if (device->host->reset_endpoint) {
                    result = device->host->reset_endpoint(device, ep);
                }
                break;
            }
        }
    }
    return result;
}
int usb_bulk(struct usb_device *device, struct usb_endpoint *ep, void *data, size_t length) {
    if (!usb_device_connected(device)) {
        return -19;
    }
    return device->host->transfer(device, ep, data, length, true);
}
struct usb_interface {
    unsigned number, class, subclass, protocol, report_length;
    struct usb_endpoint *input, *output;
};
static int hub_port_status(struct usb_device *hub, unsigned port, uint32_t *status) {
    uint8_t data[4];
    int result = usb_control(hub, 0xa3, 0, 0, port, data, 4);
    if (result != 4) {
        return result < 0 ? result : -EIO;
    }
    memcpy(status, data, 4);
    return 0;
}
static int hub_reset_port(struct usb_device *hub, unsigned port, unsigned *speed) {
    int result = usb_control(hub, 0x23, 3, 4, port, NULL, 0);
    if (result < 0) {
        return result;
    }
    uint64_t end = hw_time_us() + 1000000;
    uint32_t status;
    do {
        hw_delay(10000);
        result = hub_port_status(hub, port, &status);
        if (result) {
            return result;
        }
        if (!(status & 1)) {
            return -19;
        }
        if (!(status & 16) && (status & 2)) {
            usb_control(hub, 0x23, 1, 20, port, NULL, 0);
            *speed = hub->speed == 4 ? 4 : status & (1U << 9) ? 2 : status & (1U << 10) ? 3 : 1;
            return 0;
        }
    } while (hw_time_us() < end);
    return -110;
}
static int hub_attach(struct usb_device *device, unsigned protocol) {
    uint8_t descriptor[64] = {0};
    unsigned type = device->speed == 4 ? 0x2a : 0x29;
    int result = usb_control(device, 0xa0, 6, type << 8, 0, descriptor, 8);
    if (result < 7 || descriptor[1] != type || !descriptor[2] || descriptor[2] > 31) {
        return result < 0 ? result : -EINVAL;
    }
    device->hub_ports = descriptor[2];
    device->hub_protocol = protocol;
    device->multi_tt = protocol == 2;
    device->hub_tt_think = (descriptor[3] >> 5) & 3;
    if (device->host->configure_hub) {
        result = device->host->configure_hub(device);
        if (result) {
            return result;
        }
    }
    if (device->speed == 4) {
        result = usb_control(device, 0x20, 12, device->depth, 0, NULL, 0);
        if (result < 0) {
            return result;
        }
    }
    for (unsigned port = 1; port <= device->hub_ports; port++) {
        result = usb_control(device, 0x23, 3, 8, port, NULL, 0);
        if (result < 0) {
            return result;
        }
    }
    hw_delay(MAX(descriptor[5] * 2000U, 20000U));
    device->hub = true;
    kprintf("USB hub %u ports ready\n", (uint64_t)device->hub_ports);
    return 0;
}
static int enumerate(struct usb_host *host, unsigned port, struct usb_device *parent) {
    if (device_count == USB_MAX_DEVICES) {
        return -ENOSPC;
    }
    struct usb_device *device = kalloc(sizeof(*device));
    if (!device) {
        return -ENOMEM;
    }
    device->host = host;
    device->parent = parent;
    device->port = parent ? parent->port : port;
    if (parent) {
        if (parent->depth >= 5) {
            kfree(device);
            return -ELOOP;
        }
        device->depth = parent->depth + 1;
        device->hub_port = port;
        device->route = parent->route | (MIN(port, 15U) << (parent->depth * 4));
    }
    int error = parent ? hub_reset_port(parent, port, &device->speed)
                       : host->reset_port(host, port, &device->speed);
    if (error) {
        kfree(device);
        return error;
    }
    device->ep0_size = device->speed == 4 ? 512 : device->speed == 3 ? 64 : 8;
    device->online = true;
    devices[device_count++] = device;
    error = host->address_device(device);
    if (error) {
        goto fail;
    }
    uint8_t descriptor[18];
    error = usb_control(device, 0x80, 6, 0x100, 0, descriptor, 8);
    if (error != 8 || descriptor[0] != 18 || descriptor[1] != 1) {
        goto fail;
    }
    if (device->speed == 4 && descriptor[7] != 9) {
        goto fail;
    }
    device->ep0_size = device->speed == 4 ? 512 : descriptor[7];
    if (device->ep0_size != 8 && device->ep0_size != 16 && device->ep0_size != 32 &&
        device->ep0_size != 64 && device->ep0_size != 512) {
        goto fail;
    }
    error = usb_control(device, 0x80, 6, 0x100, 0, descriptor, sizeof(descriptor));
    if (error != sizeof(descriptor) || !descriptor[17]) {
        goto fail;
    }
    uint8_t configuration_header[9];
    error = usb_control(device, 0x80, 6, 0x200, 0, configuration_header, 9);
    if (error != 9 || configuration_header[0] != 9 || configuration_header[1] != 2) {
        goto fail;
    }
    unsigned length = *(uint16_t *)(configuration_header + 2);
    if (length < 9 || length > 4096) {
        goto fail;
    }
    uint8_t *configuration = kalloc(length);
    if (!configuration) {
        error = -ENOMEM;
        goto fail;
    }
    error = usb_control(device, 0x80, 6, 0x200, 0, configuration, length);
    if (error != (int)length) {
        kfree(configuration);
        goto fail;
    }
    device->configuration = configuration[5];
    struct usb_interface interfaces[16] = {0};
    unsigned interface_count = 0;
    struct usb_interface *interface = NULL;
    for (unsigned offset = 9; offset + 2 <= length;) {
        uint8_t *d = configuration + offset;
        if (d[0] < 2 || d[0] > length - offset) {
            error = -EINVAL;
            break;
        }
        if (d[1] == 4 && d[0] >= 9) {
            interface = NULL;
            if (!d[3] && interface_count < 16) {
                interface = &interfaces[interface_count++];
                interface->number = d[2];
                interface->class = d[5];
                interface->subclass = d[6];
                interface->protocol = d[7];
            }
        } else if (d[1] == 5 && d[0] >= 7 && interface) {
            if (device->endpoint_count == USB_MAX_ENDPOINTS || !(d[2] & 15)) {
                error = -EINVAL;
                break;
            }
            struct usb_endpoint *ep = &device->endpoints[device->endpoint_count++];
            ep->address = d[2];
            ep->type = d[3] & 3;
            ep->packet_size = *(uint16_t *)(d + 4) & 0x7ff;
            ep->interval = d[6];
            if (!ep->packet_size || ep->packet_size > 1024 || (ep->type != 2 && ep->type != 3)) {
                error = -95;
                break;
            }
            if (ep->address & 0x80) {
                interface->input = ep;
            } else {
                interface->output = ep;
            }
        } else if (d[1] == 0x21 && d[0] >= 9 && interface) {
            if (d[6] == 0x22) {
                interface->report_length = *(uint16_t *)(d + 7);
            }
        }
        offset += d[0];
    }
    kfree(configuration);
    if (error < 0) {
        goto fail;
    }
    error = usb_control(device, 0, 9, device->configuration, 0, NULL, 0);
    if (error < 0) {
        goto fail;
    }
    if (host->configure) {
        error = host->configure(device);
        if (error) {
            goto fail;
        }
    }
    for (unsigned i = 0; i < interface_count; i++) {
        struct usb_interface *it = &interfaces[i];
        if (it->class == 9) {
            error = hub_attach(device, it->protocol);
            if (error) {
                goto fail;
            }
        } else if (it->class == 8 && it->subclass == 6 && it->protocol == 0x50 && it->input &&
                   it->output) {
            usb_msd_attach(device, it->number, it->input, it->output);
        } else if (it->class == 3 && it->input && it->input->type == 3) {
            usb_hid_attach(device, it->number, it->subclass == 1 ? it->protocol : 0, it->input,
                           it->report_length);
        }
    }
    kprintf("USB device %x:%x address %u speed %u ready\n", (uint64_t)*(uint16_t *)(descriptor + 8),
            (uint64_t)*(uint16_t *)(descriptor + 10), (uint64_t)device->address,
            (uint64_t)device->speed);
    return 0;
fail:
    device->online = false;
    host->stop(device);
    kprintf("USB enumeration port %u error %u\n", (uint64_t)port + 1,
            (uint64_t)(error < 0 ? -error : EIO));
    return error < 0 ? error : -EIO;
}
void usb_init(void) {
    lock(&usb_lock);
    for (unsigned pass = 0; pass < 2; pass++) {
        for (struct usb_host *host = hosts; host; host = host->next) {
            for (unsigned port = 0; port < host->ports; port++) {
                if (!host->connected(host, port)) {
                    continue;
                }
                bool present = false;
                for (unsigned i = 0; i < device_count; i++) {
                    if (!devices[i]->parent && devices[i]->host == host &&
                        devices[i]->port == port && devices[i]->online) {
                        present = true;
                    }
                }
                if (!present) {
                    enumerate(host, port, NULL);
                }
            }
        }
    }
    for (unsigned i = 0; i < device_count; i++) {
        struct usb_device *hub = devices[i];
        if (!hub->hub || !usb_device_connected(hub)) {
            continue;
        }
        for (unsigned port = 1; port <= hub->hub_ports; port++) {
            uint32_t status;
            int error = hub_port_status(hub, port, &status);
            if (error) {
                kprintf("USB hub port status error %u\n", (uint64_t)-error);
                continue;
            }
            bool present = false;
            for (unsigned child = 0; child < device_count; child++) {
                struct usb_device *device = devices[child];
                if (device->parent != hub || device->hub_port != port || !device->online) {
                    continue;
                }
                if (!(status & 1)) {
                    device->online = false;
                    device->host->stop(device);
                } else {
                    present = true;
                }
            }
            if ((status & 1) && !present) {
                enumerate(hub->host, port, hub);
            }
            if (status & (1U << 16)) {
                usb_control(hub, 0x23, 1, 16, port, NULL, 0);
            }
        }
    }
    unlock(&usb_lock);
}
void usb_poll(void) {
    if (__atomic_exchange_n(&usb_lock.value, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    for (unsigned i = 0; i < device_count; i++) {
        struct usb_device *device = devices[i];
        if (!device->online) {
            continue;
        }
        if (!usb_device_connected(device)) {
            device->online = false;
            device->host->stop(device);
        } else if (device->hid) {
            usb_hid_poll(device);
        }
    }
    unlock(&usb_lock);
}
void usb_acquire(void) {
    lock(&usb_lock);
}
void usb_release(void) {
    unlock(&usb_lock);
}
