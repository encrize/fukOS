#ifndef ATA_H
#define ATA_H
#include <stdint.h>

/* Primary-master ATA PIO access. */
int ata_read(uint32_t lba, uint32_t count, void *buffer);

int ata_write(uint32_t lba, uint32_t count, const void *buffer);

int ata_present(void);

#endif
