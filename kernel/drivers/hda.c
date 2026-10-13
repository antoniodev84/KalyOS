#include "kaly.h"
struct hda_descriptor {
    uint64_t address;
    uint32_t length, flags;
} PACKED;
struct hda {
    volatile uint8_t *mmio, *stream;
    uint32_t *corb;
    uint64_t *rirb;
    uint8_t *buffer;
    struct hda_descriptor *bdl;
    unsigned corb_entries, rirb_entries, corb_write, rirb_read;
    unsigned codec, function_group, converter, rate, channels, volume;
    uint16_t format;
    uint8_t route[32], route_indices[32];
    unsigned route_length;
    bool ready, running;
    unsigned read_position, write_position, queued;
    uint64_t drain_deadline;
    struct lock lock;
};
static struct hda audio;
static uint16_t read16(unsigned offset) {
    return *(volatile uint16_t *)(audio.mmio + offset);
}
static uint32_t read32(unsigned offset) {
    return *(volatile uint32_t *)(audio.mmio + offset);
}
static void write16(unsigned offset, uint16_t value) {
    *(volatile uint16_t *)(audio.mmio + offset) = value;
}
static void write32(unsigned offset, uint32_t value) {
    *(volatile uint32_t *)(audio.mmio + offset) = value;
}
static int verb(unsigned node, unsigned operation, unsigned payload, uint32_t *response) {
    uint32_t command = (audio.codec << 28) | (node << 20) |
                       (operation < 16 ? operation << 16 : operation << 8) | payload;
    unsigned next = (audio.corb_write + 1) % audio.corb_entries;
    uint64_t end = hw_time_us() + 100000;
    while ((read16(0x4a) & 255) == next) {
        if (hw_time_us() >= end) {
            return -110;
        }
    }
    audio.corb[next] = command;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    write16(0x48, next);
    audio.corb_write = next;
    do {
        unsigned write = read16(0x58) & 255;
        while (audio.rirb_read != write) {
            audio.rirb_read = (audio.rirb_read + 1) % audio.rirb_entries;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint64_t entry = audio.rirb[audio.rirb_read];
            uint32_t extended = entry >> 32;
            if (!(extended & 16) && (extended & 15) == audio.codec) {
                *response = entry;
                audio.mmio[0x5d] = 5;
                return 0;
            }
        }
        __asm__ volatile("pause");
    } while (hw_time_us() < end);
    return -110;
}
static int parameter(unsigned node, unsigned id, uint32_t *value) {
    return verb(node, 0xf00, id, value);
}
static int set_verb(unsigned node, unsigned operation, unsigned payload) {
    uint32_t response;
    return verb(node, operation, payload, &response);
}
static int connections(unsigned node, uint8_t nodes[32], unsigned *count) {
    uint32_t value;
    int error = parameter(node, 0x0e, &value);
    if (error) {
        return error;
    }
    bool long_form = value & 128;
    unsigned entries = value & 127, output = 0, previous = 0;
    for (unsigned i = 0; i < entries;) {
        error = verb(node, 0xf02, i, &value);
        if (error) {
            return error;
        }
        for (unsigned j = 0; j < (long_form ? 2U : 4U) && i < entries; j++, i++) {
            unsigned raw = long_form ? (value >> (j * 16)) & 65535 : (value >> (j * 8)) & 255;
            unsigned target = raw & (long_form ? 0x7fff : 127);
            bool range = raw & (long_form ? 0x8000 : 128);
            if (!target || target > 255 || (range && target <= previous)) {
                return -EINVAL;
            }
            unsigned first = range ? previous + 1 : target;
            for (unsigned n = first; n <= target; n++) {
                if (output == 32) {
                    return -ENOSPC;
                }
                nodes[output++] = n;
            }
            previous = target;
        }
    }
    *count = output;
    return 0;
}
static bool find_route(unsigned node, unsigned depth, uint8_t visited[256]) {
    if (depth == 32 || visited[node]) {
        return false;
    }
    visited[node] = 1;
    uint32_t caps;
    if (parameter(node, 9, &caps)) {
        return false;
    }
    audio.route[depth] = node;
    if (((caps >> 20) & 15) == 0) {
        uint32_t formats, rates;
        if (parameter(node, 0x0b, &formats) || parameter(node, 0x0a, &rates)) {
            return false;
        }
        if (!(formats & 1)) {
            if (parameter(audio.function_group, 0x0b, &formats)) {
                return false;
            }
        }
        if (!(rates & (1U << 17))) {
            if (parameter(audio.function_group, 0x0a, &rates)) {
                return false;
            }
        }
        if (!(formats & 1) || !(rates & (1U << 17)) || !(rates & (1U << 6))) {
            return false;
        }
        audio.converter = node;
        audio.route_length = depth + 1;
        return true;
    }
    uint8_t nodes[32];
    unsigned count;
    if (connections(node, nodes, &count)) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        audio.route_indices[depth] = i;
        if (find_route(nodes[i], depth + 1, visited)) {
            return true;
        }
    }
    return false;
}
static int amplifier(unsigned node, uint32_t caps, unsigned input_index, unsigned volume) {
    uint32_t gain_caps;
    if (caps & 4) {
        int error = parameter(caps & 8 ? node : audio.function_group, 0x12, &gain_caps);
        if (error) {
            return error;
        }
        unsigned steps = (gain_caps >> 8) & 127;
        unsigned gain = steps * volume / 100;
        error = set_verb(node, 3, 0xb000 | gain | (!volume && (gain_caps >> 31) ? 128 : 0));
        if (error) {
            return error;
        }
    }
    if (caps & 2) {
        int error = parameter(caps & 8 ? node : audio.function_group, 0x0d, &gain_caps);
        if (error) {
            return error;
        }
        unsigned gain = MIN(gain_caps & 127, (gain_caps >> 8) & 127);
        return set_verb(node, 3, 0x7000 | (input_index << 8) | gain);
    }
    return 0;
}
static int route_enable(void) {
    if (set_verb(audio.function_group, 0x705, 0)) {
        return -EIO;
    }
    for (unsigned i = 0; i < audio.route_length; i++) {
        unsigned node = audio.route[i];
        uint32_t caps;
        if (parameter(node, 9, &caps)) {
            return -EIO;
        }
        if ((caps & (1U << 10)) && set_verb(node, 0x705, 0)) {
            return -EIO;
        }
        if (i + 1 < audio.route_length && set_verb(node, 0x701, audio.route_indices[i])) {
            return -EIO;
        }
        if (amplifier(node, caps, audio.route_indices[i], audio.volume)) {
            return -EIO;
        }
    }
    uint32_t pin_caps, configuration;
    unsigned pin = audio.route[0];
    if (parameter(pin, 0x0c, &pin_caps) || verb(pin, 0xf1c, 0, &configuration)) {
        return -EIO;
    }
    unsigned control = 0x40;
    if (((configuration >> 20) & 15) == 2) {
        control |= 0x80;
    }
    if (set_verb(pin, 0x707, control)) {
        return -EIO;
    }
    if ((pin_caps & (1U << 16)) && set_verb(pin, 0x70c, 2)) {
        return -EIO;
    }
    if (set_verb(audio.converter, 0x706, 0x10) || set_verb(audio.converter, 2, audio.format)) {
        return -EIO;
    }
    return 0;
}
static unsigned ring_size(unsigned offset, unsigned *entries) {
    uint8_t capabilities = audio.mmio[offset];
    unsigned selection = capabilities & 0x40 ? 2 : capabilities & 0x20 ? 1 : 0;
    *entries = selection == 2 ? 256 : selection == 1 ? 16 : 2;
    audio.mmio[offset] = (capabilities & ~3U) | selection;
    return selection;
}
void hda_probe(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (audio.mmio) {
        return;
    }
    audio.mmio = pci_mmio(bus, dev, fn, 0, 0x4000);
    if (!audio.mmio) {
        return;
    }
    write32(8, read32(8) & ~1U);
    if (hw_wait32((volatile uint32_t *)(audio.mmio + 8), 1, 0, 100000)) {
        goto fail;
    }
    hw_delay(1000);
    write32(8, read32(8) | 1);
    if (hw_wait32((volatile uint32_t *)(audio.mmio + 8), 1, 1, 100000)) {
        goto fail;
    }
    hw_delay(1000);
    uint16_t codecs = read16(0x0e);
    uint16_t caps = read16(0);
    unsigned inputs = (caps >> 8) & 15, outputs = (caps >> 12) & 15;
    if (!outputs || !codecs) {
        goto fail;
    }
    uint64_t corb = pmm_alloc(), rirb = pmm_alloc(), bdl = pmm_alloc(),
             buffer = pmm_alloc_contiguous(4);
    if (!corb || !rirb || !bdl || !buffer) {
        if (corb) {
            pmm_free(corb);
        }
        if (rirb) {
            pmm_free(rirb);
        }
        if (bdl) {
            pmm_free(bdl);
        }
        for (unsigned i = 0; buffer && i < 4; i++) {
            pmm_free(buffer + i * PAGE);
        }
        goto fail;
    }
    audio.corb = phys(corb);
    audio.rirb = phys(rirb);
    audio.bdl = phys(bdl);
    audio.buffer = phys(buffer);
    audio.mmio[0x4c] = audio.mmio[0x5c] = 0;
    write32(0x40, corb);
    write32(0x44, 0);
    write32(0x50, rirb);
    write32(0x54, 0);
    ring_size(0x4e, &audio.corb_entries);
    ring_size(0x5e, &audio.rirb_entries);
    write16(0x48, 0);
    write16(0x4a, 0x8000);
    uint64_t end = hw_time_us() + 100000;
    while (!(read16(0x4a) & 0x8000)) {
        if (hw_time_us() >= end) {
            goto fail;
        }
    }
    write16(0x4a, 0);
    write16(0x58, 0x8000);
    write16(0x5a, 1);
    write32(0x20, 0);
    audio.mmio[0x5c] = 3;
    audio.mmio[0x4c] = 2;
    audio.rate = 48000;
    audio.channels = 2;
    audio.format = 0x11;
    audio.volume = 75;
    audio.stream = audio.mmio + 0x80 + inputs * 0x20;
    for (unsigned codec = 0; codec < 15; codec++) {
        if (!(codecs & (1U << codec))) {
            continue;
        }
        audio.codec = codec;
        uint32_t vendor, groups;
        if (parameter(0, 0, &vendor) || parameter(0, 4, &groups)) {
            continue;
        }
        unsigned first_group = (groups >> 16) & 255, group_count = groups & 255;
        for (unsigned group = first_group; group < first_group + group_count; group++) {
            uint32_t type, widgets;
            if (parameter(group, 5, &type) || (type & 255) != 1 || parameter(group, 4, &widgets)) {
                continue;
            }
            audio.function_group = group;
            unsigned first = (widgets >> 16) & 255, count = widgets & 255;
            for (unsigned preference = 0; preference < 3; preference++) {
                for (unsigned node = first; node < first + count && node < 256; node++) {
                    uint32_t widget_caps, pin_caps, configuration;
                    if (parameter(node, 9, &widget_caps) || ((widget_caps >> 20) & 15) != 4 ||
                        parameter(node, 0x0c, &pin_caps) || !(pin_caps & 16) ||
                        verb(node, 0xf1c, 0, &configuration) || (configuration >> 30) == 1) {
                        continue;
                    }
                    unsigned pin_type = (configuration >> 20) & 15;
                    if (pin_type != (preference == 0 ? 1U : preference == 1 ? 2U : 0U)) {
                        continue;
                    }
                    uint8_t visited[256] = {0};
                    if (find_route(node, 0, visited) && !route_enable()) {
                        audio.ready = true;
                        kprintf("Intel HDA codec %x DAC %u pin %u ready\n", (uint64_t)vendor,
                                (uint64_t)audio.converter, (uint64_t)node);
                        if (vendor == 0x10ec0269) {
                            kprintf("Realtek ALC269 analog path ready\n");
                        }
                        return;
                    }
                }
            }
        }
    }
fail:
    if (audio.mmio) {
        audio.mmio[0x4c] = audio.mmio[0x5c] = 0;
        write32(0x20, 0);
    }
    if (audio.corb) {
        pmm_free(physical(audio.corb));
    }
    if (audio.rirb) {
        pmm_free(physical(audio.rirb));
    }
    if (audio.bdl) {
        pmm_free(physical(audio.bdl));
    }
    for (unsigned i = 0; audio.buffer && i < 4; i++) {
        pmm_free(physical(audio.buffer) + i * PAGE);
    }
    memset(&audio, 0, sizeof(audio));
    kprintf("HDA no supported playback route\n");
}
static void stream_stop(void) {
    audio.stream[0] &= ~2U;
    audio.stream[3] = 0x1c;
    audio.running = false;
    audio.queued = 0;
    audio.drain_deadline = 0;
}
static int stream_update(void) {
    if (!audio.running) {
        return 0;
    }
    volatile uint8_t *s = audio.stream;
    if (s[3] & 0x18) {
        stream_stop();
        return -EIO;
    }
    unsigned position = *(volatile uint32_t *)(s + 4) % 16384;
    unsigned consumed = (position + 16384 - audio.read_position) % 16384;
    unsigned clear = MIN(consumed, audio.queued);
    unsigned first = MIN(clear, 16384 - audio.read_position);
    memset(audio.buffer + audio.read_position, 0, first);
    memset(audio.buffer, 0, clear - first);
    audio.queued -= clear;
    audio.read_position = position;
    s[3] = 4;
    if (!audio.queued && !audio.drain_deadline) {
        audio.drain_deadline = hw_time_us() + 50000;
    }
    return 0;
}
static int stream_start(const void *data, size_t length) {
    volatile uint8_t *s = audio.stream;
    s[0] &= ~2U;
    s[0] |= 1;
    uint64_t end = hw_time_us() + 100000;
    while (!(s[0] & 1)) {
        if (hw_time_us() >= end) {
            return -110;
        }
    }
    s[0] &= ~1U;
    while (s[0] & 1) {
        if (hw_time_us() >= end) {
            return -110;
        }
    }
    memset(audio.buffer, 0, 16384);
    memcpy(audio.buffer, data, length);
    for (unsigned i = 0; i < 4; i++) {
        audio.bdl[i] = (struct hda_descriptor){physical(audio.buffer) + i * PAGE, PAGE, 1};
    }
    *(volatile uint32_t *)(s + 8) = 16384;
    *(volatile uint16_t *)(s + 12) = 3;
    *(volatile uint16_t *)(s + 18) = audio.format;
    *(volatile uint64_t *)(s + 24) = physical(audio.bdl);
    s[2] = 0x10;
    s[3] = 0x1c;
    audio.read_position = 0;
    audio.write_position = length;
    audio.queued = length;
    audio.drain_deadline = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s[0] |= 2;
    audio.running = true;
    return 0;
}
static int stream_sync(void) {
    uint64_t end = hw_time_us() + 2000000;
    while (audio.running) {
        int error = stream_update();
        if (error) {
            return error;
        }
        uint64_t now = hw_time_us();
        if (!audio.queued && audio.drain_deadline && now >= audio.drain_deadline) {
            stream_stop();
            return 0;
        }
        if (now >= end) {
            stream_stop();
            return -110;
        }
        __asm__ volatile("pause");
    }
    return 0;
}
void hda_poll(void) {
    if (!audio.ready || __atomic_exchange_n(&audio.lock.value, 1, __ATOMIC_ACQUIRE)) {
        return;
    }
    int error = stream_update();
    if (error) {
        audio.ready = false;
    } else if (audio.running && !audio.queued && audio.drain_deadline &&
               hw_time_us() >= audio.drain_deadline) {
        stream_stop();
    }
    unlock(&audio.lock);
}
long hda_write(const void *data, size_t length) {
    if (!audio.ready) {
        return -19;
    }
    lock(&audio.lock);
    unsigned frame_size = audio.channels * 2;
    if (length % frame_size) {
        unlock(&audio.lock);
        return -EINVAL;
    }
    if (!length) {
        unlock(&audio.lock);
        return 0;
    }
    length = MIN(length, 12288);
    if (!audio.running) {
        int error = stream_start(data, length);
        unlock(&audio.lock);
        return error ? error : (long)length;
    }
    uint64_t end = hw_time_us() + 1000000;
    unsigned space;
    do {
        int error = stream_update();
        if (error) {
            unlock(&audio.lock);
            return error;
        }
        if (!audio.queued) {
            unsigned next = ALIGN_UP(audio.read_position + PAGE, PAGE) % 16384;
            audio.queued = (next + 16384 - audio.read_position) % 16384;
            audio.write_position = next;
        }
        space = 12288 - audio.queued;
        space -= space % frame_size;
        if (space) {
            break;
        }
        if (hw_time_us() >= end) {
            stream_stop();
            unlock(&audio.lock);
            return -110;
        }
        __asm__ volatile("pause");
    } while (true);
    length = MIN(length, space);
    size_t first = MIN(length, 16384 - audio.write_position);
    memcpy(audio.buffer + audio.write_position, data, first);
    memcpy(audio.buffer, (const uint8_t *)data + first, length - first);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    audio.write_position = (audio.write_position + length) % 16384;
    audio.queued += length;
    audio.drain_deadline = 0;
    unlock(&audio.lock);
    return length;
}
long hda_ioctl(unsigned long request, uint64_t user) {
    if (!audio.ready) {
        return -19;
    }
    if (request == 0x5000 || request == 0x5001) {
        lock(&audio.lock);
        int error = 0;
        if (request == 0x5000) {
            stream_stop();
        } else {
            error = stream_sync();
        }
        unlock(&audio.lock);
        return error;
    }
    int value;
    int error = copy_from_user(&value, user, sizeof(value));
    if (error) {
        return error;
    }
    lock(&audio.lock);
    if (audio.running &&
        (request == 0xc0045002 || request == 0xc0045003 || request == 0xc0045006)) {
        error = stream_sync();
        if (error) {
            unlock(&audio.lock);
            return error;
        }
    }
    if (request == 0xc0045002) {
        if (value != 44100 && value != 48000) {
            error = -EINVAL;
        } else {
            audio.rate = value;
        }
    } else if (request == 0xc0045003 || request == 0xc0045006) {
        unsigned channels = request == 0xc0045003 ? value + 1 : value;
        if (channels < 1 || channels > 2) {
            error = -EINVAL;
        } else {
            audio.channels = channels;
        }
    } else if (request == 0xc0045005) {
        if (value && value != 16) {
            error = -EINVAL;
        }
        value = 16;
    } else if (request == 0xc0044d00) {
        unsigned left = value & 255, right = (value >> 8) & 255;
        if (left > 100 || right > 100) {
            error = -EINVAL;
        } else {
            audio.volume = (left + right) / 2;
        }
    } else if (request == 0x80044d00) {
        value = audio.volume | (audio.volume << 8);
    } else {
        error = -ENOSYS;
    }
    if (!error) {
        audio.format = (audio.rate == 44100 ? 0x4000 : 0) | 0x10 | (audio.channels - 1);
        error = route_enable();
        if (!error) {
            error = copy_to_user(user, &value, sizeof(value));
        }
    }
    unlock(&audio.lock);
    return error;
}
size_t hda_status(char *data, size_t capacity) {
    const char *text = audio.ready ? "Intel HDA PCM s16le 44100/48000 Hz mono/stereo ready\n"
                                   : "HDA playback unavailable\n";
    size_t length = MIN(strlen(text), capacity);
    memcpy(data, text, length);
    return length;
}
