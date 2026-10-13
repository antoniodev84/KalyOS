#ifndef USB_H
#define USB_H
#include "kaly.h"
#define USB_MAX_DEVICES 32
#define USB_MAX_ENDPOINTS 16
struct usb_device;
struct usb_endpoint {
    uint8_t address, type, interval, toggle;
    uint16_t packet_size;
    void *private;
};
struct usb_request {
    uint8_t type, request;
    uint16_t value, index, length;
} PACKED;
struct usb_host {
    void *private;
    unsigned ports;
    int (*reset_port)(struct usb_host *, unsigned, unsigned *);
    bool (*connected)(struct usb_host *, unsigned);
    int (*address_device)(struct usb_device *);
    int (*configure)(struct usb_device *);
    int (*configure_hub)(struct usb_device *);
    int (*control)(struct usb_device *, const struct usb_request *, void *);
    int (*transfer)(struct usb_device *, struct usb_endpoint *, void *, size_t, bool);
    void (*stop)(struct usb_device *);
    int (*reset_endpoint)(struct usb_device *, struct usb_endpoint *);
    struct usb_host *next;
};
struct hid_field {
    unsigned offset, bits;
    int minimum, maximum;
    bool relative, present;
};
struct usb_device {
    struct usb_host *host;
    struct usb_device *parent;
    unsigned port, speed, address, slot, hub_port, route, depth;
    unsigned hub_ports, hub_protocol, hub_tt_think;
    bool hub, multi_tt;
    uint16_t ep0_size;
    uint8_t configuration;
    struct usb_endpoint endpoints[USB_MAX_ENDPOINTS];
    unsigned endpoint_count;
    void *private;
    bool online;
    unsigned hid_interface, hid_protocol, hid_endpoint;
    uint8_t previous_keys[8];
    struct hid_field hid_x, hid_y, hid_buttons, hid_wheel;
    unsigned hid_report_id;
    bool hid, caps_lock;
};
void usb_init(void);
void usb_acquire(void);
void usb_release(void);
void usb_poll(void);
void usb_register_host(struct usb_host *);
void uhci_probe(uint8_t, uint8_t, uint8_t);
void ehci_probe(uint8_t, uint8_t, uint8_t);
void xhci_probe(uint8_t, uint8_t, uint8_t);
int usb_control(struct usb_device *, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t);
bool usb_device_connected(struct usb_device *);
int usb_bulk(struct usb_device *, struct usb_endpoint *, void *, size_t);
void usb_msd_attach(struct usb_device *, unsigned, struct usb_endpoint *, struct usb_endpoint *);
void usb_hid_attach(struct usb_device *, unsigned, unsigned, struct usb_endpoint *, unsigned);
void usb_hid_poll(struct usb_device *);
#endif
