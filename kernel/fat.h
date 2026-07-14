#ifndef FAT_H
#define FAT_H
#include <stdint.h>

typedef struct {
    char     name[128];
    int      is_dir;
    uint32_t size;
    uint32_t first_cluster;
} fat_dirent;

/* Mount FAT16/FAT32 through one of the available block backends. */
int fat_mount_ata(uint32_t part_lba);
int fat_mount_mem(const void *base, uint32_t size);

int fat_mount_usb(uint32_t part_lba);
int fat_mount_usb_auto(void);

int fat_mounted(void);

typedef struct {
    int      mounted;
    int      backend;
    uint32_t bytes_per_sec;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_size_sectors;
    uint32_t root_dir_sectors;
    uint32_t data_start_sector;
    uint32_t total_sectors;
    int      is_fat32;
    uint32_t root_cluster;
    uint32_t part_lba;
} fat_debug_t;

void fat_debug_info(fat_debug_t *out);

/* Current-directory operations. */
void fat_cwd_reset(void);

void fat_cwd_path(char *buf, uint32_t bufsize);

uint32_t fat_dir_count(void);

int fat_dir_get(uint32_t index, fat_dirent *out);

int fat_chdir(const char *name);

uint32_t fat_read_file(const char *name, uint8_t *buf, uint32_t bufsize);

typedef struct {
    uint32_t first_cluster;
    uint32_t size;
    uint32_t position;
    uint32_t cluster;
    uint32_t cluster_offset;
} fat_file;

/* Sequential reads for files too large for the shared file buffer. */
int      fat_open(const char *name, fat_file *file);
int      fat_seek(fat_file *file, uint32_t offset);
uint32_t fat_read(fat_file *file, uint8_t *buf, uint32_t size);

/* Mutating operations use the currently mounted writable backend. */
int fat_write_file(const char *name, const uint8_t *data, uint32_t size);

int fat_touch(const char *name);

int fat_mkdir(const char *name);

int fat_delete_file(const char *name);
int fat_exists(const char *name);
int fat_rmdir(const char *name);

/* Free space on the mounted volume, in bytes (0 if nothing is mounted). */
uint32_t fat_free_bytes(void);

int fat_move_file(const char *source_path, const char *destination_path);

#endif
