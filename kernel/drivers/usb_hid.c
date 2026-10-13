#include "usb.h"
static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";
static void keyboard_report(struct usb_device *device, const uint8_t *report, unsigned length) {
    if (length < 8 || report[2] == 1 || report[2] == 2 || report[2] == 3) {
        return;
    }
    bool shift = report[0] & 0x22;
    for (unsigned i = 2; i < 8; i++) {
        unsigned key = report[i];
        if (!key) {
            continue;
        }
        bool previous = false;
        for (unsigned j = 2; j < 8; j++) {
            if (device->previous_keys[j] == key) {
                previous = true;
            }
        }
        if (previous) {
            continue;
        }
        if (key == 0x39) {
            device->caps_lock = !device->caps_lock;
            continue;
        }
        if (key >= 4 && key < 4 + sizeof(plain) - 1) {
            unsigned index = key - 4;
            bool upper = key < 30 ? shift != device->caps_lock : shift;
            char character = upper ? shifted[index] : plain[index];
            if ((report[0] & 0x11) && character >= 'a' && character <= 'z') {
                character -= 'a' - 1;
            }
            input_enqueue(character);
        } else if (key >= 0x4f && key <= 0x52) {
            input_enqueue(27);
            input_enqueue('[');
            input_enqueue("CDBA"[key - 0x4f]);
        }
    }
    memcpy(device->previous_keys, report, 8);
}
struct hid_global {
    unsigned usage_page, report_size, report_count, report_id;
    int minimum, maximum;
};
static int signed_value(uint32_t value, unsigned bytes) {
    if (bytes == 1) {
        return (int8_t)value;
    }
    if (bytes == 2) {
        return (int16_t)value;
    }
    return (int32_t)value;
}
static bool parse_report(struct usb_device *device, const uint8_t *data, unsigned length) {
    struct hid_global global = {0}, saved[8];
    unsigned stack = 0, offsets[256] = {0};
    unsigned usages[32], usage_count = 0, usage_minimum = 0, usage_maximum = 0;
    for (unsigned position = 0; position < length;) {
        uint8_t prefix = data[position++];
        if (prefix == 0xfe) {
            if (position + 2 > length) {
                return false;
            }
            unsigned count = data[position];
            position += 2;
            if (count > length - position) {
                return false;
            }
            position += count;
            continue;
        }
        unsigned bytes = (prefix & 3) == 3 ? 4 : prefix & 3;
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (bytes > length - position) {
            return false;
        }
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; i++) {
            value |= (uint32_t)data[position++] << (i * 8);
        }
        if (type == 1) {
            if (tag == 0) {
                global.usage_page = value;
            } else if (tag == 1) {
                global.minimum = signed_value(value, bytes);
            } else if (tag == 2) {
                global.maximum = global.minimum < 0 ? signed_value(value, bytes) : value;
            } else if (tag == 7) {
                global.report_size = value;
            } else if (tag == 8) {
                if (!value || value > 255) {
                    return false;
                }
                global.report_id = value;
            } else if (tag == 9) {
                global.report_count = value;
            } else if (tag == 10) {
                if (stack == 8) {
                    return false;
                }
                saved[stack++] = global;
            } else if (tag == 11) {
                if (!stack) {
                    return false;
                }
                global = saved[--stack];
            }
        } else if (type == 2) {
            if (tag == 0 && usage_count < 32) {
                usages[usage_count++] = value & 65535;
            } else if (tag == 1) {
                usage_minimum = value;
            } else if (tag == 2) {
                usage_maximum = value;
            }
        } else if (type == 0) {
            if (tag == 8) {
                if (global.report_size > 32 || global.report_count > 256 ||
                    global.report_size * global.report_count > 4096 - offsets[global.report_id]) {
                    return false;
                }
                unsigned offset = offsets[global.report_id];
                if (!(value & 1) && (value & 2)) {
                    for (unsigned i = 0; i < global.report_count; i++) {
                        unsigned usage = i < usage_count ? usages[i] : usage_minimum + i;
                        if (i >= usage_count && usage > usage_maximum) {
                            continue;
                        }
                        struct hid_field *field = NULL;
                        if (global.usage_page == 1 && usage == 0x30) {
                            field = &device->hid_x;
                        } else if (global.usage_page == 1 && usage == 0x31) {
                            field = &device->hid_y;
                        } else if (global.usage_page == 1 && usage == 0x38) {
                            field = &device->hid_wheel;
                        } else if (global.usage_page == 9 && usage == 1) {
                            field = &device->hid_buttons;
                        }
                        if (field &&
                            (!device->hid_x.present || device->hid_report_id == global.report_id)) {
                            *field = (struct hid_field){offset + i * global.report_size,
                                                        global.report_size,
                                                        global.minimum,
                                                        global.maximum,
                                                        !!(value & 4),
                                                        true};
                            if (field == &device->hid_buttons) {
                                field->bits = MIN(global.report_count, 3);
                            }
                            if (field == &device->hid_x) {
                                device->hid_report_id = global.report_id;
                            }
                        }
                    }
                }
                offsets[global.report_id] += global.report_size * global.report_count;
            }
            usage_count = usage_minimum = usage_maximum = 0;
        }
    }
    return device->hid_x.present && device->hid_y.present;
}
static int field_value(struct hid_field *field, const uint8_t *report, unsigned length) {
    if (!field->present || !field->bits || field->bits > 32 ||
        field->offset + field->bits > length * 8) {
        return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < field->bits; i++) {
        unsigned bit = field->offset + i;
        value |= ((report[bit / 8] >> (bit % 8)) & 1U) << i;
    }
    if (field->minimum < 0 && field->bits < 32 && (value & (1U << (field->bits - 1)))) {
        value |= ~((1U << field->bits) - 1);
    }
    return value;
}
void usb_hid_attach(struct usb_device *device, unsigned interface, unsigned protocol,
                    struct usb_endpoint *input, unsigned report_length) {
    device->hid_interface = interface;
    device->hid_protocol = protocol;
    device->hid_endpoint = input - device->endpoints;
    if (protocol == 1 || protocol == 2) {
        if (usb_control(device, 0x21, 11, 0, interface, NULL, 0) < 0) {
            return;
        }
    } else {
        if (!report_length || report_length > 4096) {
            return;
        }
        uint8_t *report = kalloc(report_length);
        if (!report) {
            return;
        }
        int result = usb_control(device, 0x81, 6, 0x2200, interface, report, report_length);
        bool supported =
            result == (int)report_length && parse_report(device, report, report_length);
        kfree(report);
        if (!supported) {
            kprintf("USB HID report unsupported\n");
            return;
        }
    }
    usb_control(device, 0x21, 10, 0, interface, NULL, 0);
    device->hid = true;
    kprintf("USB HID %s ready\n", protocol == 1 ? "keyboard" : "pointer");
}
void usb_hid_poll(struct usb_device *device) {
    struct usb_endpoint *ep = &device->endpoints[device->hid_endpoint];
    uint8_t report[64];
    int length =
        device->host->transfer(device, ep, report, MIN(ep->packet_size, sizeof(report)), false);
    if (length <= 0) {
        return;
    }
    if (device->hid_protocol == 1) {
        keyboard_report(device, report, length);
    } else if (device->hid_protocol == 2 && length >= 3) {
        input_pointer((int8_t)report[1], (int8_t)report[2], report[0] & 7, false);
    } else {
        const uint8_t *data = report;
        if (device->hid_report_id) {
            if (report[0] != device->hid_report_id) {
                return;
            }
            data++;
            length--;
        }
        int x = field_value(&device->hid_x, data, length);
        int y = field_value(&device->hid_y, data, length);
        unsigned buttons = field_value(&device->hid_buttons, data, length);
        if (device->hid_x.relative) {
            input_pointer(x, y, buttons, false);
        } else if (device->hid_x.maximum > device->hid_x.minimum &&
                   device->hid_y.maximum > device->hid_y.minimum) {
            x = (int64_t)(x - device->hid_x.minimum) * (framebuffer_width() - 1) /
                (device->hid_x.maximum - device->hid_x.minimum);
            y = (int64_t)(y - device->hid_y.minimum) * (framebuffer_height() - 1) /
                (device->hid_y.maximum - device->hid_y.minimum);
            input_pointer_absolute(x, y, buttons);
        }
        if (device->hid_wheel.present) {
            input_wheel(field_value(&device->hid_wheel, data, length));
        }
    }
}
