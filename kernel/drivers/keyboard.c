#include "kaly.h"
extern void input_enqueue(uint8_t);
static bool shift, caps, ctrl, extended;
static struct lock keyboard_lock;
static const char normal[128] = {
    [1] = 27,    [2] = '1',  [3] = '2',  [4] = '3',   [5] = '4',  [6] = '5',   [7] = '6',
    [8] = '7',   [9] = '8',  [10] = '9', [11] = '0',  [12] = '-', [13] = '=',  [14] = '\b',
    [15] = '\t', [16] = 'q', [17] = 'w', [18] = 'e',  [19] = 'r', [20] = 't',  [21] = 'y',
    [22] = 'u',  [23] = 'i', [24] = 'o', [25] = 'p',  [26] = '[', [27] = ']',  [28] = '\n',
    [30] = 'a',  [31] = 's', [32] = 'd', [33] = 'f',  [34] = 'g', [35] = 'h',  [36] = 'j',
    [37] = 'k',  [38] = 'l', [39] = ';', [40] = '\'', [41] = '`', [43] = '\\', [44] = 'z',
    [45] = 'x',  [46] = 'c', [47] = 'v', [48] = 'b',  [49] = 'n', [50] = 'm',  [51] = ',',
    [52] = '.',  [53] = '/', [57] = ' '};
static const char shifted[128] = {
    [2] = '!',  [3] = '@',  [4] = '#',  [5] = '$',  [6] = '%',  [7] = '^',  [8] = '&',
    [9] = '*',  [10] = '(', [11] = ')', [12] = '_', [13] = '+', [26] = '{', [27] = '}',
    [39] = ':', [40] = '"', [41] = '~', [43] = '|', [51] = '<', [52] = '>', [53] = '?'};
void keyboard_poll(void) {
    if (__atomic_exchange_n(&keyboard_lock.value, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    while (inb(0x64) & 1) {
        uint8_t status = inb(0x64), code = inb(0x60);
        if (status & 32) {
            touchpad_byte(code);
            continue;
        }
        if (code == 0xe0) {
            extended = true;
            continue;
        }
        if (code == 29 || code == 157) {
            ctrl = code == 29;
            extended = false;
            continue;
        }
        if (extended) {
            extended = false;
            char arrow = code == 72   ? 'A'
                         : code == 80 ? 'B'
                         : code == 77 ? 'C'
                         : code == 75 ? 'D'
                                      : 0;
            if (code == 83) {
                input_enqueue(127);
            }
            if (arrow) {
                input_enqueue(27);
                input_enqueue('[');
                input_enqueue(arrow);
            }
            continue;
        }
        if (code == 42 || code == 54) {
            shift = true;
            continue;
        }
        if (code == 170 || code == 182) {
            shift = false;
            continue;
        }
        if (code & 128) {
            continue;
        }
        if (code == 58) {
            caps = !caps;
            continue;
        }
        char c = normal[code];
        if (c >= 'a' && c <= 'z') {
            if (shift != caps) {
                c -= 32;
            }
        } else if (shift && shifted[code]) {
            c = shifted[code];
        }
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            c &= 31;
        }
        if (c) {
            input_enqueue(c);
        }
    }
    unlock(&keyboard_lock);
}
