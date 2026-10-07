/* Exercise the real SDIO writer while stereo DMA interrupts run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "syscode.h"
#include "b310e-mixer.h"

volatile unsigned b310e_save_test_stage;
void keytrn_init(void) {
    uint8_t keymap[64];
    sys_getkeymap(keymap);
    memset(sys_data.keytrn, 0, sizeof(sys_data.keytrn));
}
void lcd_appinit(void) {
    sys_data.display.w2 = sys_data.display.w1;
    sys_data.display.h2 = sys_data.display.h1;
}
int mkdir(const char *, int);
static const char path[] = "/saves/Super Metroid long filename.srm";
static unsigned char data[1024], tone[22050];
static void fail(unsigned line) {
    b310e_save_test_stage = line;
    for (;;) sys_wait_ms(10);
}
#define CHECK(x) do { if (!(x)) fail(__LINE__); } while (0)
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    b310e_save_test_stage = 1;
    char filename[16] = "savegam0.";
    CHECK(strncat(filename, "wl6", 3) == filename);
    CHECK(!strcmp(filename, "savegam0.wl6"));
    unsigned boot = 0;
    FILE *file = fopen("/boot.txt", "rb");
    if (file) { CHECK(fread(&boot, sizeof(boot), 1, file) == 1); CHECK(!fclose(file)); }
    CHECK(!mkdir("/saves", 0777));
    if (boot) {
        file = fopen(path, "rb"); CHECK(file);
        for (unsigned block=0; block<72; block++) {
            CHECK(fread(data, 1, sizeof(data), file) == sizeof(data));
            for (unsigned i=0; i<sizeof(data); i++) CHECK(data[i] == (unsigned char)(block+i));
        }
        CHECK(fgetc(file) == EOF); CHECK(!fclose(file));
    }
    b310e_mixer_init();
    for (unsigned i=0; i<sizeof(tone); i++) tone[i] = i%50 < 25 ? 170 : 86;
    file = fopen("/saves/Temporary save.tmp", "wb"); CHECK(file);
    for (unsigned block=0; block<72; block++) {
        if (!(block%12)) CHECK(b310e_sample_play(0,tone,sizeof(tone),22050,255,128,128) == 0);
        for (unsigned i=0; i<sizeof(data); i++) data[i] = block+i;
        CHECK(fwrite(data, 1, sizeof(data), file) == sizeof(data));
        sys_wait_ms(25);
    }
    CHECK(!fflush(file)); CHECK(!fclose(file));
    CHECK(!rename("/saves/Temporary save.tmp", path));
    CHECK(!rename(path, path));
    file = fopen("/append.txt", "ab"); CHECK(file);
    CHECK(fwrite("test", 1, 4, file) == 4); CHECK(!fclose(file));
    file = fopen("/append.txt", "rb"); CHECK(file);
    CHECK(!fseek(file, 0, SEEK_END)); CHECK(ftell(file) == (long)((boot+1)*4)); CHECK(!fclose(file));
    /* Low memory is recoverable; completed saves survive heap exhaustion. */
    void *chunks[64]; unsigned used=0;
    while (used<64 && (chunks[used]=malloc(65536))) used++;
    CHECK(used>0 && used<64);
    while (used) free(chunks[--used]);
    boot++;
    file = fopen("/boot.txt", "wb"); CHECK(file);
    CHECK(fwrite(&boot, sizeof(boot), 1, file) == 1); CHECK(!fclose(file));
    file = fopen("/result.txt", "wb"); CHECK(file);
    fprintf(file,"PASS boot=%u bytes=73728 append=%u audio=stereo\n",boot,boot*4);
    CHECK(!fclose(file));
    b310e_save_test_stage = 0x600d;
    for (;;) { int key; sys_event(&key); sys_wait_ms(10); }
}
