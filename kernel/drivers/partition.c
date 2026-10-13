#include "kaly.h"
struct partition {
    struct blockdev block;
    struct blockdev *disk;
    uint64_t start;
};
static int transfer(struct blockdev *block, uint64_t lba, uint32_t count, void *data, bool write) {
    struct partition *p = block->private;
    return write ? block_write(p->disk, p->start + lba, count, data)
                 : block_read(p->disk, p->start + lba, count, data);
}
static void register_partition(struct blockdev *disk, unsigned index, uint64_t start,
                               uint64_t count) {
    if (!count || !start || start >= disk->sectors || count > disk->sectors - start) {
        return;
    }
    struct partition *p = kalloc(sizeof(*p));
    if (!p) {
        return;
    }
    size_t length = strlen(disk->name);
    if (length + 4 >= sizeof(p->block.name) || index > 99) {
        kfree(p);
        return;
    }
    strcpy(p->block.name, disk->name);
    p->block.name[length++] = 'p';
    if (index >= 10) {
        p->block.name[length++] = '0' + index / 10;
    }
    p->block.name[length++] = '0' + index % 10;
    p->block.name[length] = 0;
    p->disk = disk;
    p->start = start;
    p->block.sectors = count;
    p->block.sector_size = disk->sector_size;
    p->block.private = p;
    p->block.transfer = transfer;
    p->block.partition = true;
    block_register(&p->block);
}
static uint32_t crc32(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffff;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1));
        }
    }
    return ~crc;
}
static int gpt(struct blockdev *disk) {
    uint8_t header[512];
    int error = block_read(disk, 1, 1, header);
    if (error || memcmp(header, "EFI PART", 8)) {
        return -EINVAL;
    }
    uint32_t size = *(uint32_t *)(header + 12), expected = *(uint32_t *)(header + 16);
    if (size < 92 || size > 512 || *(uint64_t *)(header + 24) != 1) {
        return -EINVAL;
    }
    *(uint32_t *)(header + 16) = 0;
    if (crc32(header, size) != expected) {
        return -EINVAL;
    }
    uint64_t first = *(uint64_t *)(header + 40), last = *(uint64_t *)(header + 48);
    uint64_t lba = *(uint64_t *)(header + 72);
    unsigned count = *(uint32_t *)(header + 80), entry_size = *(uint32_t *)(header + 84);
    uint32_t entry_crc = *(uint32_t *)(header + 88);
    if (first < 2 || last >= disk->sectors || first > last || entry_size < 128 ||
        entry_size > 4096 || entry_size % 8 || !count || count > 4096 ||
        (uint64_t)count * entry_size > 1024 * 1024) {
        return -EINVAL;
    }
    size_t bytes = (size_t)count * entry_size;
    unsigned sectors = ALIGN_UP(bytes, 512) / 512;
    if (lba < 2 || lba >= first || sectors > first - lba) {
        return -EINVAL;
    }
    uint8_t *entries = kalloc(sectors * 512);
    if (!entries) {
        return -ENOMEM;
    }
    error = block_read(disk, lba, sectors, entries);
    if (!error && crc32(entries, bytes) != entry_crc) {
        error = -EINVAL;
    }
    if (!error) {
        for (unsigned i = 0; i < MIN(count, 99); i++) {
            uint8_t *entry = entries + i * entry_size;
            uint8_t empty[16] = {0};
            if (!memcmp(entry, empty, 16)) {
                continue;
            }
            uint64_t start = *(uint64_t *)(entry + 32), end = *(uint64_t *)(entry + 40);
            if (start < first || end > last || start > end) {
                continue;
            }
            register_partition(disk, i + 1, start, end - start + 1);
        }
    }
    kfree(entries);
    return error;
}
void block_scan_partitions(void) {
    for (struct blockdev *disk = block_devices; disk;) {
        struct blockdev *next = disk->next;
        if (disk->partition || disk->scanned || disk->sector_size != 512) {
            disk = next;
            continue;
        }
        uint8_t sector[512];
        int error = block_read(disk, 0, 1, sector);
        if (error) {
            disk = next;
            continue;
        }
        disk->scanned = true;
        if (*(uint16_t *)(sector + 510) == 0xaa55) {
            bool protective = false;
            for (unsigned i = 0; i < 4; i++) {
                if (sector[446 + i * 16 + 4] == 0xee) {
                    protective = true;
                }
            }
            if (protective) {
                error = gpt(disk);
                if (error) {
                    kprintf("Invalid GPT on %s\n", disk->name);
                }
            } else {
                for (unsigned i = 0; i < 4; i++) {
                    uint8_t *entry = sector + 446 + i * 16;
                    if (!entry[4] || entry[4] == 5 || entry[4] == 15 || entry[4] == 0x85) {
                        continue;
                    }
                    register_partition(disk, i + 1, *(uint32_t *)(entry + 8),
                                       *(uint32_t *)(entry + 12));
                }
            }
        }
        disk = next;
    }
}
