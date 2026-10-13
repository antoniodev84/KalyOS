#include "usb.h"
struct msd {
    struct blockdev block;
    struct usb_device *device;
    struct usb_endpoint *input, *output;
    unsigned interface, tag;
};
struct cbw {
    uint32_t signature, tag, length;
    uint8_t flags, lun, command_length, command[16];
} PACKED;
struct csw {
    uint32_t signature, tag, residue;
    uint8_t status;
} PACKED;
static unsigned disk_count;
static void recovery(struct msd *d) {
    usb_control(d->device, 0x21, 0xff, 0, d->interface, NULL, 0);
    usb_control(d->device, 2, 1, 0, d->input->address, NULL, 0);
    usb_control(d->device, 2, 1, 0, d->output->address, NULL, 0);
    d->input->toggle = d->output->toggle = 0;
}
static int scsi(struct msd *d, const uint8_t *command, unsigned command_length, void *data,
                size_t length, bool write) {
    if (command_length > 16 || !command_length) {
        return -EINVAL;
    }
    struct cbw cbw = {.signature = 0x43425355,
                      .tag = ++d->tag,
                      .length = length,
                      .flags = write ? 0 : 0x80,
                      .command_length = command_length};
    memcpy(cbw.command, command, command_length);
    int result = usb_bulk(d->device, d->output, &cbw, sizeof(cbw));
    if (result != sizeof(cbw)) {
        goto error;
    }
    if (length) {
        result = usb_bulk(d->device, write ? d->output : d->input, data, length);
        if (result != (int)length) {
            goto error;
        }
    }
    struct csw csw;
    result = usb_bulk(d->device, d->input, &csw, sizeof(csw));
    if (result != sizeof(csw) || csw.signature != 0x53425355 || csw.tag != cbw.tag ||
        csw.residue > length || csw.status > 1) {
        goto error;
    }
    if (csw.status || csw.residue) {
        return -EIO;
    }
    return 0;
error:
    recovery(d);
    return result < 0 ? result : -EIO;
}
static void put_be32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = value >> ((3 - i) * 8);
    }
}
static void put_be64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) {
        p[i] = value >> ((7 - i) * 8);
    }
}
static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static int disk_transfer(struct blockdev *block, uint64_t lba, uint32_t count, void *buffer,
                         bool write) {
    struct msd *d = block->private;
    usb_acquire();
    int error = 0;
    while (count) {
        unsigned chunk = MIN(count, 32);
        uint8_t command[16] = {0};
        unsigned command_length;
        if (lba + chunk > 0x100000000UL) {
            command[0] = write ? 0x8a : 0x88;
            put_be64(command + 2, lba);
            put_be32(command + 10, chunk);
            command_length = 16;
        } else {
            command[0] = write ? 0x2a : 0x28;
            put_be32(command + 2, lba);
            command[7] = chunk >> 8;
            command[8] = chunk;
            command_length = 10;
        }
        error = scsi(d, command, command_length, buffer, chunk * 512, write);
        if (error) {
            break;
        }
        buffer = (uint8_t *)buffer + chunk * 512;
        lba += chunk;
        count -= chunk;
    }
    if (!error && write) {
        uint8_t command[10] = {0x35};
        error = scsi(d, command, 10, NULL, 0, false);
    }
    usb_release();
    return error;
}
void usb_msd_attach(struct usb_device *device, unsigned interface, struct usb_endpoint *input,
                    struct usb_endpoint *output) {
    if (input->type != 2 || output->type != 2) {
        return;
    }
    struct msd *d = kalloc(sizeof(*d));
    if (!d) {
        return;
    }
    d->device = device;
    d->interface = interface;
    d->input = input;
    d->output = output;
    uint8_t inquiry[36], sense[18], capacity[32];
    uint8_t command[16] = {0x12, 0, 0, 0, 36};
    int error = scsi(d, command, 6, inquiry, sizeof(inquiry), false);
    if (error || (inquiry[0] & 31)) {
        goto fail;
    }
    unsigned attempt;
    for (attempt = 0; attempt < 10; attempt++) {
        memset(command, 0, sizeof(command));
        error = scsi(d, command, 6, NULL, 0, false);
        if (!error) {
            break;
        }
        command[0] = 3;
        command[4] = sizeof(sense);
        scsi(d, command, 6, sense, sizeof(sense), false);
        hw_delay(100000);
    }
    if (attempt == 10) {
        goto fail;
    }
    memset(command, 0, sizeof(command));
    command[0] = 0x25;
    error = scsi(d, command, 10, capacity, 8, false);
    if (error) {
        goto fail;
    }
    uint64_t last = get_be32(capacity);
    uint32_t sector_size = get_be32(capacity + 4);
    if (last == 0xffffffff) {
        memset(command, 0, sizeof(command));
        command[0] = 0x9e;
        command[1] = 0x10;
        command[13] = 32;
        error = scsi(d, command, 16, capacity, 32, false);
        if (error) {
            goto fail;
        }
        last = ((uint64_t)get_be32(capacity) << 32) | get_be32(capacity + 4);
        sector_size = get_be32(capacity + 8);
    }
    if (sector_size != 512 || last == UINT64_MAX || !last) {
        error = -95;
        goto fail;
    }
    strcpy(d->block.name, "usb00");
    d->block.name[3] = '0' + disk_count / 10;
    d->block.name[4] = '0' + disk_count % 10;
    disk_count++;
    d->block.sectors = last + 1;
    d->block.sector_size = 512;
    d->block.private = d;
    d->block.transfer = disk_transfer;
    block_register(&d->block);
    kprintf("USB MSD %s %u sectors\n", d->block.name, d->block.sectors);
    return;
fail:
    kprintf("USB MSD attach error %u\n", (uint64_t)(error < 0 ? -error : EIO));
    kfree(d);
}
