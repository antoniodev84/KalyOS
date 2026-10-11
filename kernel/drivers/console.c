#include "kaly.h"
static struct lock output_lock, input_lock;
static unsigned row, col;
static uint8_t input[1024];
static unsigned head, tail;
static volatile uint16_t *vga = (void *)0xb8000;
void console_init(void) {
    outb(0x3f9, 0);
    outb(0x3fb, 0x80);
    outb(0x3f8, 1);
    outb(0x3f9, 0);
    outb(0x3fb, 3);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
    for (unsigned i = 0; i < 2000; i++) {
        vga[i] = 0x0720;
    }
}
static void emit(char c) {
    if (c == '\n') {
        while (!(inb(0x3fd) & 32)) {
            __asm__ volatile("pause");
        }
        outb(0x3f8, '\r');
    }
    while (!(inb(0x3fd) & 32)) {
        __asm__ volatile("pause");
    }
    outb(0x3f8, c);
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        col = 0;
    } else if (c == '\b') {
        if (col) {
            col--;
            vga[row * 80 + col] = 0x0720;
        }
    } else if ((uint8_t)c >= 32) {
        vga[row * 80 + col] = 0x0700 | (uint8_t)c;
        if (++col == 80) {
            col = 0;
            row++;
        }
    }
    if (row >= 25) {
        for (unsigned i = 0; i < 1920; i++) {
            vga[i] = vga[i + 80];
        }
        for (unsigned i = 1920; i < 2000; i++) {
            vga[i] = 0x0720;
        }
        row = 24;
    }
    unsigned pos = row * 80 + col;
    outb(0x3d4, 14);
    outb(0x3d5, pos >> 8);
    outb(0x3d4, 15);
    outb(0x3d5, pos);
}
void console_putc(char c) {
    lock(&output_lock);
    emit(c);
    unlock(&output_lock);
}
void console_write(const char *s, size_t n) {
    lock(&output_lock);
    while (n--) {
        emit(*s++);
    }
    unlock(&output_lock);
}
void input_enqueue(uint8_t c) {
    unsigned next = (head + 1) % sizeof(input);
    if (next == tail) {
        return;
    }
    input[head] = c;
    head = next;
}
int console_read(char *s, size_t n) {
    lock(&input_lock);
    keyboard_poll();
    while (inb(0x3fd) & 1) {
        uint8_t c = inb(0x3f8);
        input_enqueue(c == '\r' ? '\n' : c);
    }
    size_t done = 0;
    while (done < n && head != tail) {
        s[done++] = input[tail];
        tail = (tail + 1) % sizeof(input);
    }
    unlock(&input_lock);
    return done;
}
