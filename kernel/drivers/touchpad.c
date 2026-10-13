#include "kaly.h"
struct input_event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};
static struct input_event events[256];
static unsigned event_head, event_tail, buttons;
static int pointer_x = 400, pointer_y = 300, last_x, last_y;
static struct lock event_lock;
static bool absolute, touching, mouse_enabled;
static uint8_t packet[6];
static unsigned packet_count;
static void event(uint16_t type, uint16_t code, int value) {
    unsigned next = (event_head + 1) % 256;
    if (next == event_tail) {
        event_tail = (event_tail + 1) % 256;
    }
    uint64_t time = hw_time_us();
    events[event_head] = (struct input_event){time / 1000000, time % 1000000, type, code, value};
    event_head = next;
}
void input_pointer(int x, int y, unsigned keys, bool abs) {
    lock(&event_lock);
    if (abs) {
        event(3, 0, x);
        event(3, 1, y);
        if (touching) {
            pointer_x += (x - last_x) / 8;
            pointer_y -= (y - last_y) / 8;
        }
        last_x = x;
        last_y = y;
        touching = true;
    } else {
        event(2, 0, x);
        event(2, 1, y);
        pointer_x += x;
        pointer_y += y;
    }
    for (unsigned i = 0; i < 3; i++) {
        if ((buttons ^ keys) & (1U << i)) {
            event(1, 0x110 + i, (keys >> i) & 1);
        }
    }
    buttons = keys;
    event(0, 0, 0);
    pointer_x = MAX(0, MIN(pointer_x, (int)framebuffer_width() - 1));
    pointer_y = MAX(0, MIN(pointer_y, (int)framebuffer_height() - 1));
    framebuffer_pointer(pointer_x, pointer_y);
    unlock(&event_lock);
}
void input_pointer_absolute(int x, int y, unsigned keys) {
    lock(&event_lock);
    pointer_x = MAX(0, MIN(x, (int)framebuffer_width() - 1));
    pointer_y = MAX(0, MIN(y, (int)framebuffer_height() - 1));
    event(3, 0, pointer_x);
    event(3, 1, pointer_y);
    for (unsigned i = 0; i < 3; i++) {
        if ((buttons ^ keys) & (1U << i)) {
            event(1, 0x110 + i, (keys >> i) & 1);
        }
    }
    buttons = keys;
    event(0, 0, 0);
    framebuffer_pointer(pointer_x, pointer_y);
    unlock(&event_lock);
}
void input_wheel(int value) {
    if (!value) {
        return;
    }
    lock(&event_lock);
    event(2, 8, value);
    event(0, 0, 0);
    unlock(&event_lock);
}
long input_read(void *p, size_t n) {
    if (n < sizeof(struct input_event)) {
        return -EINVAL;
    }
    keyboard_poll();
    lock(&event_lock);
    size_t done = 0;
    while (event_tail != event_head && n - done >= sizeof(struct input_event)) {
        memcpy((uint8_t *)p + done, &events[event_tail], sizeof(struct input_event));
        done += sizeof(struct input_event);
        event_tail = (event_tail + 1) % 256;
    }
    unlock(&event_lock);
    return done ? (long)done : -EAGAIN;
}
static int write_ready(void) {
    uint64_t end = hw_time_us() + 100000;
    while (inb(0x64) & 2) {
        if (hw_time_us() >= end) {
            return -110;
        }
    }
    return 0;
}
static int aux_read(void) {
    uint64_t end = hw_time_us() + 100000;
    do {
        uint8_t status = inb(0x64);
        if ((status & 0x21) == 0x21) {
            return inb(0x60);
        }
        if ((status & 1) && !(status & 32)) {
            (void)inb(0x60);
        }
    } while (hw_time_us() < end);
    return -110;
}
static int aux_write(uint8_t value) {
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (write_ready()) {
            return -110;
        }
        outb(0x64, 0xd4);
        if (write_ready()) {
            return -110;
        }
        outb(0x60, value);
        int ack = aux_read();
        if (ack == 0xfa) {
            return 0;
        }
        if (ack != 0xfe) {
            return -EIO;
        }
    }
    return -EIO;
}
static int sliced(uint8_t value) {
    for (int i = 6; i >= 0; i -= 2) {
        if (aux_write(0xe8) || aux_write((value >> i) & 3)) {
            return -EIO;
        }
    }
    return 0;
}
void touchpad_init(void) {
    if (write_ready()) {
        return;
    }
    outb(0x64, 0xa8);
    if (aux_write(0xf5) || aux_write(0xf6)) {
        return;
    }
    if (!sliced(0) && !aux_write(0xe9)) {
        int a = aux_read(), b = aux_read(), c = aux_read();
        if (a >= 0 && b == 0x47 && c >= 0) {
            if (!sliced(0x80) && !aux_write(0xf3) && !aux_write(20)) {
                absolute = true;
            }
        }
    }
    if (!absolute && aux_write(0xf6)) {
        return;
    }
    if (aux_write(0xf4)) {
        return;
    }
    mouse_enabled = true;
    kprintf(absolute ? "Synaptics absolute touchpad ready\n" : "PS/2 relative pointer ready\n");
}
void touchpad_byte(uint8_t byte) {
    if (!mouse_enabled) {
        return;
    }
    if (!packet_count && (absolute ? (byte & 0xc8) != 0x80 : !(byte & 8))) {
        return;
    }
    packet[packet_count++] = byte;
    if (packet_count < (absolute ? 6U : 3U)) {
        return;
    }
    packet_count = 0;
    if (absolute) {
        if ((packet[3] & 0xc8) != 0xc0) {
            return;
        }
        if (packet[2] < 30) {
            touching = false;
            input_pointer(0, 0, packet[0] & 3, false);
            return;
        }
        int x = ((packet[3] & 16) << 8) | ((packet[1] & 15) << 8) | packet[4];
        int y = ((packet[3] & 32) << 7) | ((packet[1] & 0xf0) << 4) | packet[5];
        input_pointer(x, y, packet[0] & 3, true);
    } else if (!(packet[0] & 0xc0)) {
        int x = packet[1] - ((packet[0] & 16) ? 256 : 0);
        int y = packet[2] - ((packet[0] & 32) ? 256 : 0);
        input_pointer(x, -y, packet[0] & 7, false);
    }
}
