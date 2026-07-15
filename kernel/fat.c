#include <stdint.h>
#include "fat.h"
#include "ata.h"
#include "xhci.h"
#include "util.h"
#include "rtc.h"

#define SEC 512u
#define MAX_SPC 128u

static int      g_mounted;
static int      g_use_ata;
static int      g_use_usb;
static uint32_t g_part_lba;
static const uint8_t *g_mem;
static uint32_t g_mem_size;

static uint32_t g_spc, g_reserved, g_num_fats, g_root_entries, g_fat_size;
static uint32_t g_root_dir_sectors, g_data_start, g_total_sectors;
static uint32_t g_is_fat32;
static uint32_t g_root_cluster;
static uint32_t g_alloc_hint = 2u;

static uint8_t  g_fatsec[SEC];
static uint32_t g_fatsec_num;
static int      g_fatsec_dirty;
static uint8_t  g_dirbuf[128u * 1024u];
static uint32_t g_dirbuf_len;

static int      g_cwd_is_root;
static uint32_t g_cwd_cluster;
static char     g_cwd_str[256];

static int fat_cache_flush(void);
static int fat_cache_load(uint32_t sec);

static uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rdu32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int ieq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

/* Retry transient block-device failures without hiding permanent errors. */
/* xhci_msc_read/write already perform their own bounded recovery sequence.
   Repeating that whole sequence five more times made a failed large write
   look like a permanent system lock. */
#define IO_RETRIES 2

static int rd(uint32_t sec, uint32_t count, void *buf) {
    if (g_use_usb) {
        for (int attempt = 0; attempt < IO_RETRIES; attempt++)
            if (xhci_msc_read(g_part_lba + sec, count, buf) == 0) return 0;
        return -1;
    }
    if (g_use_ata) {
        for (int attempt = 0; attempt < IO_RETRIES; attempt++)
            if (ata_read(g_part_lba + sec, count, buf) == 0) return 0;
        return -1;
    }
    uint32_t off = sec * SEC, bytes = count * SEC;
    if (off >= g_mem_size) { memset(buf, 0, bytes); return 0; }
    uint32_t avail = g_mem_size - off;
    if (avail >= bytes) {
        memcpy(buf, g_mem + off, bytes);
    } else {
        memcpy(buf, g_mem + off, avail);
        memset((uint8_t *)buf + avail, 0, bytes - avail);
    }
    return 0;
}

static int wr(uint32_t sec, uint32_t count, const void *buf) {
    if (g_use_usb) {
        for (int attempt = 0; attempt < IO_RETRIES; attempt++)
            if (xhci_msc_write(g_part_lba + sec, count, buf) == 0) return 0;
        return -1;
    }
    if (g_use_ata) {
        for (int attempt = 0; attempt < IO_RETRIES; attempt++)
            if (ata_write(g_part_lba + sec, count, buf) == 0) return 0;
        return -1;
    }
    if (!g_mem) return -1;
    uint32_t off = sec * SEC, bytes = count * SEC;
    if (off + bytes > g_mem_size) return -1;
    memcpy((uint8_t *)g_mem + off, buf, bytes);
    return 0;
}

static int cl_is_eoc(uint32_t cl) {
    return g_is_fat32 ? (cl >= 0x0FFFFFF8u) : (cl >= 0xFFF8u);
}

static uint32_t fat_next(uint32_t cl) {
    uint32_t ent      = g_is_fat32 ? 4u : 2u;
    uint32_t byte_off = cl * ent;
    uint32_t sec      = g_reserved + byte_off / SEC;
    uint32_t within   = byte_off % SEC;
    if (sec != g_fatsec_num) {
        if (fat_cache_load(sec) != 0) return g_is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    }
    if (g_is_fat32) return rdu32(g_fatsec + within) & 0x0FFFFFFFu;
    return rdu16(g_fatsec + within);
}

static int parse_bpb(void) {
    uint8_t bs[SEC];
    if (rd(0, 1, bs) != 0) return 0;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    if (rdu16(bs + 11) != 512) return 0;
    g_spc          = bs[13];
    g_reserved     = rdu16(bs + 14);
    g_num_fats     = bs[16];
    g_root_entries = rdu16(bs + 17);
    uint32_t t16   = rdu16(bs + 19);
    uint32_t fsz16 = rdu16(bs + 22);
    uint32_t t32   = rdu32(bs + 32);
    uint32_t fsz32 = rdu32(bs + 36);
    g_total_sectors = t16 ? t16 : t32;
    g_fat_size      = fsz16 ? fsz16 : fsz32;
    if (g_spc == 0 || g_spc > MAX_SPC) return 0;
    if (g_fat_size == 0 || g_num_fats == 0) return 0;
    g_root_dir_sectors = (g_root_entries * 32u + SEC - 1u) / SEC;
    g_data_start = g_reserved + g_num_fats * g_fat_size + g_root_dir_sectors;
    if (g_total_sectors <= g_data_start) return 0;
    uint32_t clusters = (g_total_sectors - g_data_start) / g_spc;

    if (clusters < 4085u) return 0;
    g_is_fat32     = (clusters >= 65525u) ? 1u : 0u;
    g_root_cluster = g_is_fat32 ? (rdu32(bs + 44) & 0x0FFFFFFFu) : 0u;
    g_fatsec_num   = 0xFFFFFFFFu;
    g_fatsec_dirty = 0;
    g_alloc_hint   = 2u;
    return 1;
}

static void load_cwd(void) {
    if (g_cwd_is_root && !g_is_fat32) {

        uint32_t start = g_reserved + g_num_fats * g_fat_size;
        uint32_t bytes = g_root_dir_sectors * SEC;
        if (bytes > sizeof(g_dirbuf)) bytes = sizeof(g_dirbuf);
        rd(start, bytes / SEC, g_dirbuf);
        g_dirbuf_len = bytes;
        return;
    }

    uint32_t pos = 0;
    uint32_t cl = g_cwd_is_root ? g_root_cluster : g_cwd_cluster;
    while (cl >= 2 && !cl_is_eoc(cl)) {
        uint32_t sec = g_data_start + (cl - 2) * g_spc;
        uint32_t cbytes = g_spc * SEC;
        if (pos + cbytes > sizeof(g_dirbuf)) break;
        rd(sec, g_spc, g_dirbuf + pos);
        pos += cbytes;
        cl = fat_next(cl);
    }
    g_dirbuf_len = pos;
}

static const int LFN_POS[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static void build_name_83(const uint8_t *e, char *name) {
    int n = 0;
    uint8_t nt = e[12];
    for (int k = 0; k < 8 && e[k] != ' '; k++) {
        char c = (char)e[k];
        if ((nt & 0x08) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        name[n++] = c;
    }
    char ext[3]; int el = 0;
    for (int k = 0; k < 3 && e[8 + k] != ' '; k++) {
        char c = (char)e[8 + k];
        if ((nt & 0x10) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        ext[el++] = c;
    }
    if (el) { name[n++] = '.'; for (int k = 0; k < el; k++) name[n++] = ext[k]; }
    name[n] = 0;
}

static uint32_t dir_iterate(uint32_t want, fat_dirent *out, int *found) {
    char slots[264];
    int have_lfn = 0;
    uint32_t visible = 0;
    if (found) *found = 0;

    for (uint32_t p = 0; p + 32 <= g_dirbuf_len; p += 32) {
        const uint8_t *e = g_dirbuf + p;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { have_lfn = 0; continue; }
        uint8_t attr = e[11];
        if (attr == 0x0F) {
            int seq = e[0] & 0x3F;
            if (seq < 1 || seq > 20) continue;
            if (!have_lfn) { for (int k = 0; k < 264; k++) slots[k] = 0; have_lfn = 1; }
            int base = (seq - 1) * 13;
            for (int j = 0; j < 13; j++) {
                uint16_t ch = (uint16_t)(e[LFN_POS[j]] | (e[LFN_POS[j] + 1] << 8));
                char c;
                if (ch == 0x0000 || ch == 0xFFFF) c = 0;
                else if (ch < 128) c = (char)ch;
                else c = '?';
                if (base + j < 263) slots[base + j] = c;
            }
            continue;
        }
        if (attr & 0x08) { have_lfn = 0; continue; }

        char name[128];
        if (have_lfn && slots[0]) {
            int i = 0;
            for (; i < 127 && slots[i]; i++) name[i] = slots[i];
            name[i] = 0;
        } else {
            build_name_83(e, name);
        }
        have_lfn = 0;

        if (name[0] == '.' && (name[1] == 0 ||
            (name[1] == '.' && name[2] == 0))) continue;
        if (name[0] == 0) continue;

        if (want != 0xFFFFFFFFu && visible == want && out) {
            int q = 0; for (; name[q] && q < 127; q++) out->name[q] = name[q];
            out->name[q] = 0;
            out->is_dir = (attr & 0x10) ? 1 : 0;
            out->size = rdu32(e + 28);
            out->first_cluster = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);
            if (found) *found = 1;
            return visible;
        }
        visible++;
    }
    return visible;
}

void fat_cwd_reset(void) {
    g_cwd_is_root = 1;
    g_cwd_cluster = 0;
    g_cwd_str[0] = '/';
    g_cwd_str[1] = 0;
}

void fat_cwd_path(char *buf, uint32_t bufsize) {
    uint32_t i = 0;
    for (; g_cwd_str[i] && i + 1 < bufsize; i++) buf[i] = g_cwd_str[i];
    buf[i] = 0;
}

static void path_push(const char *name) {
    uint32_t l = 0; while (g_cwd_str[l]) l++;
    for (uint32_t k = 0; name[k] && l < 254; k++) g_cwd_str[l++] = name[k];
    if (l < 254) g_cwd_str[l++] = '/';
    g_cwd_str[l] = 0;
}

static void path_pop(void) {
    uint32_t l = 0; while (g_cwd_str[l]) l++;
    if (l <= 1) return;
    if (g_cwd_str[l - 1] == '/') l--;
    while (l > 0 && g_cwd_str[l - 1] != '/') l--;
    g_cwd_str[l] = 0;
}

uint32_t fat_dir_count(void) {
    if (!g_mounted) return 0;
    load_cwd();
    return dir_iterate(0xFFFFFFFFu, 0, 0);
}

int fat_dir_get(uint32_t index, fat_dirent *out) {
    if (!g_mounted) return 0;
    load_cwd();
    int found = 0;
    dir_iterate(index, out, &found);
    return found;
}

int fat_chdir(const char *name) {
    if (!g_mounted) return 0;
    if (name[0] == '/' && name[1] == 0) { fat_cwd_reset(); return 1; }
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
        if (g_cwd_is_root) return 1;
        load_cwd();
        uint32_t parent = 0; int found = 0;
        for (uint32_t p = 0; p + 32 <= g_dirbuf_len; p += 32) {
            const uint8_t *e = g_dirbuf + p;
            if (e[0] == 0x00) break;
            if (e[11] == 0x0F) continue;
            if (e[0] == '.' && e[1] == '.' &&
                (e[2] == ' ' || e[2] == 0)) {
                parent = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);
                found = 1; break;
            }
        }
        if (!found) return 0;
        if (parent == 0) { fat_cwd_reset(); return 1; }
        g_cwd_is_root = 0; g_cwd_cluster = parent; path_pop();
        return 1;
    }
    load_cwd();
    uint32_t cnt = dir_iterate(0xFFFFFFFFu, 0, 0);
    for (uint32_t i = 0; i < cnt; i++) {
        fat_dirent d; int f = 0;
        load_cwd();
        dir_iterate(i, &d, &f);
        if (f && d.is_dir && ieq(d.name, name)) {
            g_cwd_is_root = 0; g_cwd_cluster = d.first_cluster;
            path_push(d.name);
            return 1;
        }
    }
    return 0;
}

static uint32_t read_chain(uint32_t first, uint32_t size,
                           uint8_t *buf, uint32_t bufsize) {
    uint32_t want = size < bufsize ? size : bufsize;
    uint32_t got = 0, cl = first, cbytes = g_spc * SEC;
    static uint8_t clu[MAX_SPC * SEC];
    while (cl >= 2 && !cl_is_eoc(cl) && got < want) {
        uint32_t sec = g_data_start + (cl - 2) * g_spc;
        rd(sec, g_spc, clu);
        uint32_t n = (want - got < cbytes) ? (want - got) : cbytes;
        memcpy(buf + got, clu, n);
        got += n;
        cl = fat_next(cl);
    }
    return got;
}

uint32_t fat_read_file(const char *name, uint8_t *buf, uint32_t bufsize) {
    if (!g_mounted) return 0;
    load_cwd();
    uint32_t cnt = dir_iterate(0xFFFFFFFFu, 0, 0);
    for (uint32_t i = 0; i < cnt; i++) {
        fat_dirent d; int f = 0;
        load_cwd();
        dir_iterate(i, &d, &f);
        if (f && !d.is_dir && ieq(d.name, name)) {
            if (d.first_cluster < 2 || d.size == 0) return 0;
            return read_chain(d.first_cluster, d.size, buf, bufsize);
        }
    }
    return 0;
}

static uint8_t g_rclu[MAX_SPC * SEC] __attribute__((aligned(64)));

int fat_open(const char *name, fat_file *file) {
    if (!g_mounted || !name || !name[0] || !file) return 0;
    load_cwd();
    uint32_t cnt = dir_iterate(0xFFFFFFFFu, 0, 0);
    for (uint32_t i = 0; i < cnt; i++) {
        fat_dirent d; int found = 0;
        dir_iterate(i, &d, &found);
        if (found && !d.is_dir && ieq(d.name, name)) {
            if (d.first_cluster < 2u || d.size == 0u) return 0;
            file->first_cluster = d.first_cluster;
            file->size = d.size;
            file->position = 0;
            file->cluster = d.first_cluster;
            file->cluster_offset = 0;
            return 1;
        }
    }
    return 0;
}

int fat_seek(fat_file *file, uint32_t offset) {
    if (!file || offset > file->size || file->first_cluster < 2u) return 0;
    uint32_t cbytes = g_spc * SEC;
    uint32_t skip = offset / cbytes;
    uint32_t cl = file->first_cluster;
    while (skip-- && cl >= 2u && !cl_is_eoc(cl)) cl = fat_next(cl);
    if (offset < file->size && (cl < 2u || cl_is_eoc(cl))) return 0;
    file->position = offset;
    file->cluster = cl;
    file->cluster_offset = offset % cbytes;
    return 1;
}

uint32_t fat_read(fat_file *file, uint8_t *buf, uint32_t size) {
    if (!file || !buf || !size || file->position >= file->size) return 0;
    uint32_t left_file = file->size - file->position;
    if (size > left_file) size = left_file;
    uint32_t got = 0, cbytes = g_spc * SEC;
    while (got < size && file->cluster >= 2u && !cl_is_eoc(file->cluster)) {
        uint32_t sec = g_data_start + (file->cluster - 2u) * g_spc;
        if (rd(sec, g_spc, g_rclu) != 0) break;
        uint32_t avail = cbytes - file->cluster_offset;
        uint32_t take = size - got < avail ? size - got : avail;
        memcpy(buf + got, g_rclu + file->cluster_offset, take);
        got += take;
        file->position += take;
        file->cluster_offset += take;
        if (file->cluster_offset == cbytes) {
            file->cluster = fat_next(file->cluster);
            file->cluster_offset = 0;
        }
    }
    return got;
}

int fat_mounted(void) { return g_mounted; }

void fat_debug_info(fat_debug_t *out) {
    if (!out) return;
    out->mounted             = g_mounted;
    out->backend             = !g_mounted ? 0 : (g_use_usb ? 2 : (g_use_ata ? 1 : 3));
    out->bytes_per_sec       = SEC;
    out->sectors_per_cluster = g_spc;
    out->reserved_sectors    = g_reserved;
    out->num_fats            = g_num_fats;
    out->fat_size_sectors    = g_fat_size;
    out->root_dir_sectors    = g_root_dir_sectors;
    out->data_start_sector   = g_data_start;
    out->total_sectors       = g_total_sectors;
    out->is_fat32            = (int)g_is_fat32;
    out->root_cluster        = g_root_cluster;
    out->part_lba            = g_part_lba;
}

int fat_mount_ata(uint32_t part_lba) {
    g_use_ata = 1; g_use_usb = 0; g_part_lba = part_lba; g_mem = 0; g_mem_size = 0;
    if (!parse_bpb()) { g_mounted = 0; return 0; }
    fat_cwd_reset(); g_mounted = 1; return 1;
}

int fat_mount_mem(const void *base, uint32_t size) {
    g_use_ata = 0; g_use_usb = 0; g_mem = (const uint8_t *)base; g_mem_size = size;
    g_part_lba = 0;
    if (!parse_bpb()) { g_mounted = 0; return 0; }
    fat_cwd_reset(); g_mounted = 1; return 1;
}

int fat_mount_usb(uint32_t part_lba) {
    g_use_ata = 0; g_use_usb = 1; g_mem = 0; g_mem_size = 0; g_part_lba = part_lba;
    if (!parse_bpb()) { g_mounted = 0; g_use_usb = 0; return 0; }
    fat_cwd_reset(); g_mounted = 1; return 1;
}

int fat_mount_usb_auto(void) {
    if (fat_mount_usb(0)) return 1;

    uint8_t mbr[SEC];
    if (xhci_msc_read(0, 1, mbr) != 0) return 0;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *pe = mbr + 446 + i * 16;
        uint8_t type    = pe[4];
        uint32_t start  = rdu32(pe + 8);
        if (type == 0 || start == 0) continue;

        if (type == 0x01 || type == 0x04 || type == 0x06 ||
            type == 0x0E || type == 0x0B || type == 0x0C) {
            if (fat_mount_usb(start)) return 1;
        }
    }
    return 0;
}

static uint8_t g_wclu[MAX_SPC * SEC];

static void wru16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void wru32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static uint32_t cluster_limit(void) {
    return (g_total_sectors - g_data_start) / g_spc + 2u;
}

static uint32_t clu_sector(uint32_t cl) { return g_data_start + (cl - 2) * g_spc; }

/* Write back one cached FAT sector to every FAT copy. Long files normally
   modify many entries in the same sector; writing that sector after every
   individual entry caused thousands of BOT transactions per screenshot. */
static int fat_cache_flush(void) {
    if (!g_fatsec_dirty || g_fatsec_num == 0xFFFFFFFFu) return 0;
    uint32_t sec_in_fat = g_fatsec_num - g_reserved;
    for (uint32_t f = 0; f < g_num_fats; f++) {
        uint32_t sec = g_reserved + f * g_fat_size + sec_in_fat;
        if (wr(sec, 1, g_fatsec) != 0) return -1;
    }
    g_fatsec_dirty = 0;
    return 0;
}

static int fat_cache_load(uint32_t sec) {
    if (sec == g_fatsec_num) return 0;
    if (fat_cache_flush() != 0) return -1;
    if (rd(sec, 1, g_fatsec) != 0) return -1;
    g_fatsec_num = sec;
    return 0;
}

static int fat_set(uint32_t cl, uint32_t val) {
    uint32_t ent = g_is_fat32 ? 4u : 2u;
    uint32_t byte_off = cl * ent;
    uint32_t sec_in_fat = byte_off / SEC;
    uint32_t within = byte_off % SEC;
    uint32_t sec = g_reserved + sec_in_fat;
    if (fat_cache_load(sec) != 0) return -1;
    if (g_is_fat32) {
        uint32_t keep = rdu32(g_fatsec + within) & 0xF0000000u;
        wru32(g_fatsec + within, (val & 0x0FFFFFFFu) | keep);
    } else {
        wru16(g_fatsec + within, (uint16_t)val);
    }
    g_fatsec_dirty = 1;
    return 0;
}

static uint32_t fat_alloc(void) {
    uint32_t limit = cluster_limit();
    if (g_alloc_hint < 2u || g_alloc_hint >= limit) g_alloc_hint = 2u;
    uint32_t start = g_alloc_hint;
    for (uint32_t cl = start; cl < limit; cl++) {
        if (fat_next(cl) == 0) {
            if (fat_set(cl, g_is_fat32 ? 0x0FFFFFFFu : 0xFFFFu) != 0) return 0;
            g_alloc_hint = (cl + 1u < limit) ? cl + 1u : 2u;
            return cl;
        }
    }
    for (uint32_t cl = 2u; cl < start; cl++) {
        if (fat_next(cl) == 0) {
            if (fat_set(cl, g_is_fat32 ? 0x0FFFFFFFu : 0xFFFFu) != 0) return 0;
            g_alloc_hint = (cl + 1u < limit) ? cl + 1u : 2u;
            return cl;
        }
    }
    return 0;
}

uint32_t fat_free_bytes(void) {
    if (!g_mounted) return 0;
    uint32_t limit = cluster_limit();
    uint32_t free_clusters = 0;
    for (uint32_t cl = 2; cl < limit; cl++) {
        if (fat_next(cl) == 0) free_clusters++;
    }
    uint32_t cbytes = g_spc * SEC;
    uint32_t max_clusters = 0xFFFFFFFFu / cbytes;
    if (free_clusters > max_clusters) return 0xFFFFFFFFu;
    return free_clusters * cbytes;
}

static void free_chain(uint32_t first) {
    uint32_t cl = first, guard = 0;
    while (cl >= 2 && !cl_is_eoc(cl) && guard++ < 10000000u) {
        uint32_t next = fat_next(cl);
        fat_set(cl, 0);
        if (cl < g_alloc_hint) g_alloc_hint = cl;
        cl = next;
    }
    fat_cache_flush();
}

static int clu_zero(uint32_t cl) {
    memset(g_wclu, 0, g_spc * SEC);
    return wr(clu_sector(cl), g_spc, g_wclu);
}

static int write_data_chain(const uint8_t *data, uint32_t size, uint32_t *out_first) {
    *out_first = 0;
    if (size == 0) return 0;
    uint32_t cbytes = g_spc * SEC;
    uint32_t nclu = (size + cbytes - 1) / cbytes;
    uint32_t first = 0, prev = 0, off = 0;
    for (uint32_t i = 0; i < nclu; i++) {
        uint32_t cl = fat_alloc();
        if (cl == 0) { if (first) free_chain(first); return -1; }
        if (first == 0) first = cl;
        else if (fat_set(prev, cl) != 0) { free_chain(first); return -1; }
        uint32_t n = (size - off < cbytes) ? (size - off) : cbytes;
        memcpy(g_wclu, data + off, n);
        if (n < cbytes) memset(g_wclu + n, 0, cbytes - n);
        if (wr(clu_sector(cl), g_spc, g_wclu) != 0) { free_chain(first); return -1; }
        prev = cl; off += n;
    }
    if (fat_cache_flush() != 0) { if (first) free_chain(first); return -1; }
    *out_first = first;
    return 0;
}

#define MAX_DIR_SECS 8192u
static uint32_t g_dirsec[MAX_DIR_SECS];
static uint32_t g_dirsec_cnt;
static int      g_dir_fat16root;

static void collect_dir_secs(void) {
    g_dirsec_cnt = 0;
    if (g_cwd_is_root && !g_is_fat32) {
        g_dir_fat16root = 1;
        uint32_t start = g_reserved + g_num_fats * g_fat_size;
        for (uint32_t i = 0; i < g_root_dir_sectors && g_dirsec_cnt < MAX_DIR_SECS; i++)
            g_dirsec[g_dirsec_cnt++] = start + i;
        return;
    }
    g_dir_fat16root = 0;
    uint32_t cl = g_cwd_is_root ? g_root_cluster : g_cwd_cluster;
    uint32_t guard = 0;
    while (cl >= 2 && !cl_is_eoc(cl) && g_dirsec_cnt < MAX_DIR_SECS && guard++ < 10000000u) {
        uint32_t base = clu_sector(cl);
        for (uint32_t s = 0; s < g_spc && g_dirsec_cnt < MAX_DIR_SECS; s++)
            g_dirsec[g_dirsec_cnt++] = base + s;
        cl = fat_next(cl);
    }
}

static int slot_read(uint32_t slot, uint8_t *e32) {
    uint8_t tmp[SEC];
    if (rd(g_dirsec[slot / 16], 1, tmp) != 0) return -1;
    memcpy(e32, tmp + (slot % 16) * 32, 32);
    return 0;
}
static int slot_write(uint32_t slot, const uint8_t *e32) {
    uint8_t tmp[SEC];
    if (rd(g_dirsec[slot / 16], 1, tmp) != 0) return -1;
    memcpy(tmp + (slot % 16) * 32, e32, 32);
    return wr(g_dirsec[slot / 16], 1, tmp);
}

static int dir_find(const char *name, uint32_t *short_slot, uint32_t *lfn_first) {
    collect_dir_secs();
    uint32_t total = g_dirsec_cnt * 16u;
    char slots[264];
    int have_lfn = 0;
    uint32_t lfn_begin = 0;
    for (uint32_t idx = 0; idx < total; idx++) {
        uint8_t e[32];
        if (slot_read(idx, e) != 0) return 0;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { have_lfn = 0; continue; }
        uint8_t attr = e[11];
        if (attr == 0x0F) {
            int seq = e[0] & 0x3F;
            if (seq < 1 || seq > 20) { have_lfn = 0; continue; }
            if (!have_lfn) { for (int k = 0; k < 264; k++) slots[k] = 0; have_lfn = 1; lfn_begin = idx; }
            int base = (seq - 1) * 13;
            for (int j = 0; j < 13; j++) {
                uint16_t ch = (uint16_t)(e[LFN_POS[j]] | (e[LFN_POS[j] + 1] << 8));
                char c;
                if (ch == 0x0000 || ch == 0xFFFF) c = 0;
                else if (ch < 128) c = (char)ch;
                else c = '?';
                if (base + j < 263) slots[base + j] = c;
            }
            continue;
        }
        if (attr & 0x08) { have_lfn = 0; continue; }
        char nm[128];
        if (have_lfn && slots[0]) {
            int i = 0; for (; i < 127 && slots[i]; i++) nm[i] = slots[i]; nm[i] = 0;
        } else {
            build_name_83(e, nm);
        }
        uint32_t this_lfn = have_lfn ? lfn_begin : idx;
        have_lfn = 0;
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        if (ieq(nm, name)) { *short_slot = idx; *lfn_first = this_lfn; return 1; }
    }
    return 0;
}

static int is_83_char(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    const char *ok = "!#$%&'()-@^_`{}~";
    for (const char *p = ok; *p; p++) if (c == *p) return 1;
    return 0;
}

static int name_to_83(const char *name, uint8_t sfn[11], uint8_t *nt, uint32_t *base_len) {
    for (int i = 0; i < 11; i++) sfn[i] = ' ';
    *nt = 0;
    int len = 0; while (name[len]) len++;
    int dot = -1;
    for (int i = len - 1; i > 0; i--) { if (name[i] == '.') { dot = i; break; } }
    int base_end = (dot >= 0) ? dot : len;

    int lossy = 0, dots = 0;
    for (int i = 0; i < len; i++) if (name[i] == '.') dots++;
    if (dots > 1) lossy = 1;
    if (dot == 0) lossy = 1;

    int bl = 0, blower = 0, bupper = 0;
    for (int i = 0; i < base_end; i++) {
        char c = name[i];
        if (c == ' ') { lossy = 1; continue; }
        if (c >= 'a' && c <= 'z') { blower = 1; c = (char)(c - 'a' + 'A'); }
        else if (c >= 'A' && c <= 'Z') { bupper = 1; }
        else if (!is_83_char(c)) { c = '_'; lossy = 1; }
        if (bl < 8) sfn[bl++] = (uint8_t)c; else lossy = 1;
    }
    if (bl == 0) { sfn[0] = '_'; bl = 1; }

    int el = 0, elower = 0, eupper = 0;
    if (dot >= 0) {
        for (int i = dot + 1; i < len; i++) {
            char c = name[i];
            if (c == ' ') { lossy = 1; continue; }
            if (c >= 'a' && c <= 'z') { elower = 1; c = (char)(c - 'a' + 'A'); }
            else if (c >= 'A' && c <= 'Z') { eupper = 1; }
            else if (!is_83_char(c)) { c = '_'; lossy = 1; }
            if (el < 3) sfn[8 + el++] = (uint8_t)c; else lossy = 1;
        }
    }
    if (blower && bupper) lossy = 1;
    if (elower && eupper) lossy = 1;
    if (!lossy) {
        if (blower && !bupper) *nt |= 0x08;
        if (elower && !eupper) *nt |= 0x10;
    }
    if (base_len) *base_len = (uint32_t)bl;
    return lossy;
}

static int sfn_exists(const uint8_t sfn[11]) {
    collect_dir_secs();
    uint32_t total = g_dirsec_cnt * 16u;
    for (uint32_t i = 0; i < total; i++) {
        uint8_t e[32];
        if (slot_read(i, e) != 0) return 0;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F) continue;
        if (memcmp(e, sfn, 11) == 0) return 1;
    }
    return 0;
}

static void make_unique_sfn(uint8_t sfn[11], uint32_t base_len) {
    if (base_len > 6) base_len = 6;
    for (uint32_t n = 1; n <= 9999; n++) {
        char dec[8]; kutoa(n, dec);
        uint32_t dlen = 0; while (dec[dlen]) dlen++;
        uint32_t taillen = 1 + dlen;
        uint32_t pos = base_len;
        if (pos + taillen > 8) pos = 8 - taillen;
        sfn[pos] = '~';
        for (uint32_t k = 0; k < dlen; k++) sfn[pos + 1 + k] = (uint8_t)dec[k];
        for (uint32_t k = pos + taillen; k < 8; k++) sfn[k] = ' ';
        if (!sfn_exists(sfn)) return;
    }
}

static uint8_t lfn_checksum(const uint8_t sfn[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    return sum;
}

static void fat_now(uint16_t *fdate, uint16_t *ftime) {
    rtc_time_t t;
    rtc_read(&t);
    uint32_t yr = (t.year >= 1980) ? (uint32_t)(t.year - 1980) : 0u;
    *fdate = (uint16_t)((yr << 9) | ((t.month & 0xF) << 5) | (t.day & 0x1F));
    *ftime = (uint16_t)(((t.hour & 0x1F) << 11) | ((t.minute & 0x3F) << 5) | ((t.second / 2) & 0x1F));
}

static int dir_find_free_run(uint32_t need, uint32_t *out_start) {
    for (int pass = 0; pass < 2; pass++) {
        collect_dir_secs();
        uint32_t total = g_dirsec_cnt * 16u;
        uint32_t run = 0, run_start = 0;
        for (uint32_t i = 0; i < total; i++) {
            uint8_t e[32];
            if (slot_read(i, e) != 0) return 0;
            if (e[0] == 0x00 || e[0] == 0xE5) {
                if (run == 0) run_start = i;
                if (++run >= need) { *out_start = run_start; return 1; }
            } else run = 0;
        }
        if (g_dir_fat16root || pass == 1) return 0;
        uint32_t cl = g_cwd_is_root ? g_root_cluster : g_cwd_cluster;
        uint32_t last = cl, guard = 0;
        while (cl >= 2 && !cl_is_eoc(cl) && guard++ < 10000000u) { last = cl; cl = fat_next(cl); }
        uint32_t nc = fat_alloc();
        if (nc == 0) return 0;
        if (clu_zero(nc) != 0) return 0;
        if (fat_set(last, nc) != 0) return 0;
        if (fat_cache_flush() != 0) return 0;
    }
    return 0;
}

static int create_dirent(const char *name, uint8_t attr, uint32_t first, uint32_t size) {
    uint8_t sfn[11]; uint8_t nt = 0; uint32_t blen = 0;
    int need_lfn = name_to_83(name, sfn, &nt, &blen);
    if (need_lfn) { make_unique_sfn(sfn, blen); nt = 0; }
    else if (sfn_exists(sfn)) { make_unique_sfn(sfn, blen); need_lfn = 1; nt = 0; }

    uint32_t nchars = 0; while (name[nchars]) nchars++;
    uint32_t lfn_cnt = need_lfn ? ((nchars + 12) / 13) : 0;
    uint32_t need = lfn_cnt + 1;

    uint32_t start;
    if (!dir_find_free_run(need, &start)) return 0;
    collect_dir_secs();

    uint8_t csum = lfn_checksum(sfn);
    for (uint32_t k = 1; k <= lfn_cnt; k++) {
        uint8_t e[32];
        memset(e, 0, 32);
        e[0] = (uint8_t)(k | (k == lfn_cnt ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = csum;
        for (int j = 0; j < 13; j++) {
            uint32_t ci = (k - 1) * 13 + (uint32_t)j;
            uint16_t ch;
            if (ci < nchars) ch = (uint8_t)name[ci];
            else if (ci == nchars) ch = 0x0000;
            else ch = 0xFFFF;
            e[LFN_POS[j]] = ch & 0xFF;
            e[LFN_POS[j] + 1] = (ch >> 8) & 0xFF;
        }
        if (slot_write(start + (lfn_cnt - k), e) != 0) return 0;
    }

    uint8_t e[32];
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    e[12] = nt;
    uint16_t fdate, ftime; fat_now(&fdate, &ftime);
    wru16(e + 14, ftime);
    wru16(e + 16, fdate);
    wru16(e + 18, fdate);
    wru16(e + 22, ftime);
    wru16(e + 24, fdate);
    wru16(e + 20, (uint16_t)(first >> 16));
    wru16(e + 26, (uint16_t)(first & 0xFFFF));
    wru32(e + 28, size);
    return slot_write(start + lfn_cnt, e) == 0 ? 1 : 0;
}

int fat_write_file(const char *name, const uint8_t *data, uint32_t size) {
    if (!g_mounted || !name || !name[0]) return 0;
    uint32_t first = 0;
    if (write_data_chain(data, size, &first) != 0) return 0;

    uint32_t sslot, lfirst;
    if (dir_find(name, &sslot, &lfirst)) {
        collect_dir_secs();
        uint8_t e[32];
        if (slot_read(sslot, e) != 0) { if (first) free_chain(first); return 0; }
        if (e[11] & 0x10) { if (first) free_chain(first); return 0; }
        uint32_t old_first = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);
        uint16_t fdate, ftime; fat_now(&fdate, &ftime);
        wru16(e + 18, fdate); wru16(e + 22, ftime); wru16(e + 24, fdate);
        wru16(e + 20, (uint16_t)(first >> 16));
        wru16(e + 26, (uint16_t)(first & 0xFFFF));
        wru32(e + 28, size);
        if (slot_write(sslot, e) != 0) { if (first) free_chain(first); return 0; }
        if (old_first >= 2) free_chain(old_first);
        return 1;
    }
    if (!create_dirent(name, 0x20, first, size)) { if (first) free_chain(first); return 0; }
    return 1;
}

int fat_touch(const char *name) {
    if (!g_mounted || !name || !name[0]) return 0;
    uint32_t sslot, lfirst;
    if (dir_find(name, &sslot, &lfirst)) return 1;
    return create_dirent(name, 0x20, 0, 0);
}

int fat_exists(const char *name) {
    if (!g_mounted || !name || !name[0]) return 0;
    uint32_t sslot, lfirst;
    return dir_find(name, &sslot, &lfirst);
}

static void set_dot_entry(uint8_t *e, int dotdot, uint32_t clu) {
    memset(e, 0, 32);
    for (int i = 0; i < 11; i++) e[i] = ' ';
    e[0] = '.';
    if (dotdot) e[1] = '.';
    e[11] = 0x10;
    uint16_t fdate, ftime; fat_now(&fdate, &ftime);
    wru16(e + 14, ftime); wru16(e + 16, fdate); wru16(e + 18, fdate);
    wru16(e + 22, ftime); wru16(e + 24, fdate);
    wru16(e + 20, (uint16_t)(clu >> 16));
    wru16(e + 26, (uint16_t)(clu & 0xFFFF));
    wru32(e + 28, 0);
}

int fat_mkdir(const char *name) {
    if (!g_mounted || !name || !name[0]) return 0;
    uint32_t sslot, lfirst;
    if (dir_find(name, &sslot, &lfirst)) return 0;

    uint32_t parent = g_cwd_is_root ? 0 : g_cwd_cluster;
    uint32_t nc = fat_alloc();
    if (nc == 0) return 0;

    memset(g_wclu, 0, g_spc * SEC);
    set_dot_entry(g_wclu,      0, nc);
    set_dot_entry(g_wclu + 32, 1, parent);
    if (wr(clu_sector(nc), g_spc, g_wclu) != 0) { free_chain(nc); return 0; }

    if (fat_cache_flush() != 0) { free_chain(nc); return 0; }

    if (!create_dirent(name, 0x10, nc, 0)) { free_chain(nc); return 0; }
    return 1;
}

int fat_delete_file(const char *name) {
    if (!g_mounted || !name || !name[0]) return 0;
    uint32_t sslot, lfirst;
    if (!dir_find(name, &sslot, &lfirst)) return 0;
    collect_dir_secs();
    uint8_t e[32];
    if (slot_read(sslot, e) != 0) return 0;
    if (e[11] & 0x10) return 0;
    uint32_t first = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);
    for (uint32_t s = lfirst; s <= sslot; s++) {
        uint8_t d[32];
        if (slot_read(s, d) != 0) return 0;
        d[0] = 0xE5;
        if (slot_write(s, d) != 0) return 0;
    }
    if (first >= 2) free_chain(first);
    return 1;
}

typedef struct {
    int is_root;
    uint32_t cluster;
    char path[256];
} fat_cwd_state;

static void cwd_save(fat_cwd_state *state) {
    state->is_root = g_cwd_is_root;
    state->cluster = g_cwd_cluster;
    uint32_t i = 0;
    while (g_cwd_str[i] && i < sizeof state->path - 1) {
        state->path[i] = g_cwd_str[i];
        i++;
    }
    state->path[i] = 0;
}

static void cwd_restore(const fat_cwd_state *state) {
    g_cwd_is_root = state->is_root;
    g_cwd_cluster = state->cluster;
    uint32_t i = 0;
    while (state->path[i] && i < sizeof g_cwd_str - 1) {
        g_cwd_str[i] = state->path[i];
        i++;
    }
    g_cwd_str[i] = 0;
}

static int path_parent(const char *path, char leaf[128]) {
    char copy[256];
    uint32_t n = 0;
    if (!path || !path[0]) return 0;
    while (path[n] && n < sizeof copy - 1) { copy[n] = path[n]; n++; }
    if (path[n] || n == 0) return 0;
    copy[n] = 0;
    while (n > 1 && copy[n - 1] == '/') copy[--n] = 0;

    if (copy[0] == '/') fat_cwd_reset();
    char *last = copy;
    for (char *p = copy; *p; p++) if (*p == '/') last = p + 1;
    if (!*last) return 0;

    uint32_t li = 0;
    while (last[li] && li < 127) { leaf[li] = last[li]; li++; }
    if (last[li]) return 0;
    leaf[li] = 0;

    char *p = copy;
    if (*p == '/') p++;
    while (p < last) {
        while (*p == '/') p++;
        if (p >= last) break;
        char *end = p;
        while (end < last && *end && *end != '/') end++;
        char saved = *end;
        *end = 0;
        if (*p && !fat_chdir(p)) { *end = saved; return 0; }
        *end = saved;
        p = end + 1;
    }
    return 1;
}

int fat_open_path(const char *path, fat_file *file) {
    if (!g_mounted || !path || !file) return 0;
    fat_cwd_state saved;
    char leaf[128];
    cwd_save(&saved);
    if (!path_parent(path, leaf)) { cwd_restore(&saved); return 0; }
    int ok = fat_open(leaf, file);
    cwd_restore(&saved);
    return ok;
}

static int unlink_dirent(const char *name, int release_clusters) {
    uint32_t sslot, lfirst;
    if (!dir_find(name, &sslot, &lfirst)) return 0;
    collect_dir_secs();
    uint8_t e[32];
    if (slot_read(sslot, e) != 0 || (e[11] & 0x10)) return 0;
    uint32_t first = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);
    for (uint32_t slot = lfirst; slot <= sslot; slot++) {
        uint8_t d[32];
        if (slot_read(slot, d) != 0) return 0;
        d[0] = 0xE5;
        if (slot_write(slot, d) != 0) return 0;
    }
    if (release_clusters && first >= 2) free_chain(first);
    return 1;
}

/* Removes an empty directory. Refuses non-directories, '.', '..', and any
   directory that still contains visible entries. */
int fat_rmdir(const char *name) {
    if (!g_mounted || !name || !name[0]) return 0;
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) return 0;

    uint32_t sslot, lfirst;
    if (!dir_find(name, &sslot, &lfirst)) return 0;
    collect_dir_secs();
    uint8_t e[32];
    if (slot_read(sslot, e) != 0) return 0;
    if (!(e[11] & 0x10)) return 0;
    uint32_t first = rdu16(e + 26) | ((uint32_t)rdu16(e + 20) << 16);

    fat_cwd_state saved;
    cwd_save(&saved);
    if (first >= 2) {
        if (!fat_chdir(name)) { cwd_restore(&saved); return 0; }
        load_cwd();
        uint32_t count = dir_iterate(0xFFFFFFFFu, 0, 0);
        cwd_restore(&saved);
        if (count != 0) return 0;
    }

    if (!dir_find(name, &sslot, &lfirst)) return 0;
    collect_dir_secs();
    for (uint32_t slot = lfirst; slot <= sslot; slot++) {
        uint8_t d[32];
        if (slot_read(slot, d) != 0) return 0;
        d[0] = 0xE5;
        if (slot_write(slot, d) != 0) return 0;
    }
    if (first >= 2) free_chain(first);
    return 1;
}

int fat_move_file(const char *source_path, const char *destination_path) {
    if (!g_mounted) return 0;
    fat_cwd_state original, source_dir, destination_dir;
    char source[128], destination[128];
    cwd_save(&original);

    if (!path_parent(source_path, source)) { cwd_restore(&original); return 0; }
    uint32_t sslot, lfirst;
    if (!dir_find(source, &sslot, &lfirst)) { cwd_restore(&original); return 0; }
    collect_dir_secs();
    uint8_t entry[32];
    if (slot_read(sslot, entry) != 0 || (entry[11] & 0x10)) {
        cwd_restore(&original);
        return 0;
    }
    uint32_t first = rdu16(entry + 26) | ((uint32_t)rdu16(entry + 20) << 16);
    uint32_t size = rdu32(entry + 28);
    uint8_t attr = entry[11];
    cwd_save(&source_dir);

    cwd_restore(&original);
    if (!path_parent(destination_path, destination)) { cwd_restore(&original); return 0; }
    if (dir_find(destination, &sslot, &lfirst)) { cwd_restore(&original); return 0; }
    if (!create_dirent(destination, attr, first, size)) { cwd_restore(&original); return 0; }
    cwd_save(&destination_dir);

    cwd_restore(&source_dir);
    if (!unlink_dirent(source, 0)) {
        cwd_restore(&destination_dir);
        unlink_dirent(destination, 0);
        cwd_restore(&original);
        return 0;
    }

    cwd_restore(&original);
    return 1;
}
