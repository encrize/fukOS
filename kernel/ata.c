#include "ata.h"
#include "io.h"

#define ATA_DATA     0x1F0
#define ATA_SECCOUNT 0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7
#define ATA_COMMAND  0x1F7
#define ATA_CTRL     0x3F6

#define SR_BSY 0x80
#define SR_DRQ 0x08
#define SR_ERR 0x01

#define CMD_READ_SECTORS  0x20
#define CMD_WRITE_SECTORS 0x30
#define CMD_CACHE_FLUSH   0xE7

static void ata_400ns(void) {

    inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL); inb(ATA_CTRL);
}

static int ata_wait_ready(void) {

    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (s & SR_ERR) return -1;
        if (!(s & SR_BSY) && (s & SR_DRQ)) return 0;
    }
    return -1;
}

int ata_present(void) {
    uint8_t s = inb(ATA_STATUS);
    if (s == 0xFF) return 0;
    outb(ATA_DRIVE, 0xE0);
    ata_400ns();
    s = inb(ATA_STATUS);
    return (s != 0xFF && s != 0x00);
}

static int ata_read_chunk(uint32_t lba, uint32_t count, uint16_t *buf) {

    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (!(s & SR_BSY)) break;
    }

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_400ns();
    outb(ATA_SECCOUNT, (uint8_t)count);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, CMD_READ_SECTORS);

    uint32_t sectors = (count == 0) ? 256 : count;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_wait_ready() != 0) return -1;
        for (int i = 0; i < 256; i++) buf[i] = inw(ATA_DATA);
        buf += 256;
        ata_400ns();
    }
    return 0;
}

int ata_read(uint32_t lba, uint32_t count, void *buffer) {
    uint16_t *buf = (uint16_t *)buffer;
    while (count > 0) {
        uint32_t chunk = (count > 256) ? 256 : count;
        if (ata_read_chunk(lba, (chunk == 256) ? 0 : chunk, buf) != 0)
            return -1;
        buf   += chunk * 256;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static int ata_write_chunk(uint32_t lba, uint32_t count, const uint16_t *buf) {
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (!(s & SR_BSY)) break;
    }

    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    ata_400ns();
    outb(ATA_SECCOUNT, (uint8_t)count);
    outb(ATA_LBA_LO,  (uint8_t)(lba));
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI,  (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, CMD_WRITE_SECTORS);

    uint32_t sectors = (count == 0) ? 256 : count;
    for (uint32_t s = 0; s < sectors; s++) {
        if (ata_wait_ready() != 0) return -1;
        for (int i = 0; i < 256; i++) outw(ATA_DATA, buf[i]);
        buf += 256;
        ata_400ns();
    }

    outb(ATA_COMMAND, CMD_CACHE_FLUSH);
    for (uint32_t i = 0; i < 4000000u; i++) {
        uint8_t st = inb(ATA_STATUS);
        if (st == 0xFF) break;
        if (!(st & SR_BSY)) break;
    }
    return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void *buffer) {
    const uint16_t *buf = (const uint16_t *)buffer;
    while (count > 0) {
        uint32_t chunk = (count > 256) ? 256 : count;
        if (ata_write_chunk(lba, (chunk == 256) ? 0 : chunk, buf) != 0)
            return -1;
        buf   += chunk * 256;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}
