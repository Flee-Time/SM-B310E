/* Exercise the game's real FAT code against a generated image. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fatfile.h"

int b310e_mkdir(const char *, int);
int b310e_remove(const char *);
int b310e_rename(const char *, const char *);
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint8_t *disk;
static size_t disk_size;
static int fail_write;
static uint8_t cache[1024];

int read_sector(uint32_t sector, uint8_t *buf) {
    if ((uint64_t)sector*512+512 > disk_size) return -1;
    memcpy(buf, disk+(size_t)sector*512, 512); return 0;
}
int write_sector(uint32_t sector, uint8_t *buf) {
    if (fail_write || (uint64_t)sector*512+512 > disk_size) return -1;
    memcpy(disk+(size_t)sector*512, buf, 512); return 0;
}
static void mount(void) {
    memset(&fatdata_glob, 0, sizeof(fatdata_glob));
    fatdata_glob.buf = cache;
    CHECK(fat_init(&fatdata_glob, 0) == 0);
}
static void save(const char *name, unsigned n, const char *mode) {
    uint8_t data[8192];
    for (unsigned i=0; i<sizeof(data); i++) data[i] = (uint8_t)(i*37);
    fatfile_t *f = fat_fopen(name, mode);
    CHECK(f != NULL);
    CHECK(fat_fwrite(data, 1, n, f) == n);
    CHECK(fat_fwrite(data, 0, n, f) == 0);
    CHECK(fat_fclose(f) == 0);
}
static void verify(const char *name, unsigned n) {
    uint8_t data[8192]; fatfile_t *f = fat_fopen(name, "rb");
    CHECK(f != NULL);
    CHECK(fat_fread(data, 1, n, f) == n);
    for (unsigned i=0; i<n; i++) CHECK(data[i] == (uint8_t)(i*37));
    CHECK(fat_fread(data, 1, 1, f) == 0);
    CHECK(fat_fread(data, 0, 1, f) == 0);
    CHECK(fat_fclose(f) == 0);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    FILE *image = fopen(argv[1], "rb"); CHECK(image != NULL);
    fseek(image, 0, SEEK_END); disk_size = ftell(image); rewind(image);
    disk = malloc(disk_size); CHECK(disk != NULL);
    CHECK(fread(disk, 1, disk_size, image) == disk_size); fclose(image);
    mount();
    /* A reused card's free clusters contain deleted files, not zero-filled RAM. */
    for (unsigned c=2; c<fatdata_glob.cnum+2; c++) {
        unsigned off=fatdata_glob.fat1*512+c*4;
        if (!(disk[off]|disk[off+1]|disk[off+2]|disk[off+3]))
            memset(disk+((size_t)(c-2)*fatdata_glob.csize+fatdata_glob.data_seg)*512,
                   0xa5, fatdata_glob.csize*512);
    }
    CHECK(b310e_mkdir("/saves", 0777) == 0);
    CHECK(b310e_mkdir("/saves/Long directory name", 0777) == 0);
    save("/saves/Super Metroid.srm", 8192, "wb");
    mount(); verify("/saves/Super Metroid.srm", 8192);
    save("/saves/Super Metroid.srm", 123, "wb");
    mount(); verify("/saves/Super Metroid.srm", 123);
    save("/saves/Long directory name/Game Boy state.sav", 4096, "wb");
    mount(); verify("/saves/Long directory name/Game Boy state.sav", 4096);
    for (unsigned i=0; i<45; i++) {
        char name[100]; snprintf(name, sizeof(name), "/saves/Overlapping save filename number %u.dsg", i);
        save(name, 517, "wb");
    }
    mount();
    for (unsigned i=0; i<45; i++) {
        char name[100]; snprintf(name, sizeof(name), "/saves/Overlapping save filename number %u.dsg", i);
        verify(name, 517);
    }
    save("/saves/temp.dsg", 4000, "wb");
    CHECK(b310e_rename("/saves/temp.dsg", "/saves/Final save name.dsg") == 0);
    mount(); verify("/saves/Final save name.dsg", 4000);
    CHECK(fat_fopen("/saves/temp.dsg", "rb") == NULL);
    save("/saves/temp.dsg", 1000, "wb");
    CHECK(b310e_rename("/saves/temp.dsg", "/saves/Final save name.dsg") == 0);
    CHECK(b310e_rename("/saves/Final save name.dsg", "/saves/Final save name.dsg") == 0);
    mount(); verify("/saves/Final save name.dsg", 1000);
    CHECK(b310e_remove("/saves/Final save name.dsg") == 0);
    mount(); CHECK(fat_fopen("/saves/Final save name.dsg", "rb") == NULL);
    save("/saves/append.log", 1000, "wb");
    save("/saves/append.log", 1000, "ab");
    mount(); fatfile_t *f = fat_fopen("/saves/append.log", "rb");
    CHECK(f && f->size == 2000); CHECK(fat_fclose(f) == 0);
    fatdata_glob.curdir = fat_dir_clust(&fatdata_glob, "/saves");
    CHECK(fatdata_glob.curdir >= 2);
    save("savegam0.wl6", 4096, "wb");
    mount(); verify("/saves/savegam0.wl6", 4096);
    /* Filling all available clusters must return, including a brand-new file. */
    fatdata_t *fat = &fatdata_glob;
    fat->cnum = 32;
    for (unsigned c=2; c<34; c++) {
        unsigned off = (fat->fat1*512) + c*4;
        memset(disk+off, 0xff, 4);
    }
    fat->buf2_pos = ~0u;
    uint32_t start = 0;
    CHECK(fat_alloc_clust(fat, 0, &start) == FAT_CLUST_ERR);
    mount();
    f = fat_fopen("/saves/Failure injection.sav", "wb"); CHECK(f != NULL);
    uint8_t data[512] = {1};
    CHECK(fat_fwrite(data, 1, sizeof(data), f) == sizeof(data));
    fail_write = 1;
    CHECK(fat_fclose(f) == EOF);
    CHECK(fat->flags & FAT_IO_ERROR);
    CHECK(fat->flags & (FAT_FLUSH_BUF1 | FAT_FLUSH_BUF2));
    CHECK(fat_fopen("/saves/another.sav", "wb") == NULL);
    printf("PASS: %u game FAT checks\n", checks);
    free(disk); return 0;
}
