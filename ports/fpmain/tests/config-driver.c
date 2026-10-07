/* Host harness for the exact parser and loader argv packing. */
#include "jsonconf.h"
#include "launchargs.h"
static int scan(void *opaque, const char *dir,
                int (*emit)(void *, const char *), void *data)
{
    FILE *file = fopen((const char *)opaque, "rb");
    char line[600];
    if (!file) return -1;
    while (fgets(line, sizeof(line), file)) {
        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep++ = 0;
        sep[strcspn(sep, "\r\n")] = 0;
        if (!strcmp(line, dir) && emit(data, sep)) break;
    }
    fclose(file);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 2;
    char boot[4096];
    int nboot = json_boot_args(file, boot, sizeof(boot));
    fclose(file);
    file = fopen(argv[1], "rb");
    if (!file) return 2;
    char *menu = json_parse(file, 65536, scan, argv[2]);
    fclose(file);
    if (!menu) return 1;
    if (nboot < 0) return 2;
    FILE *bootfile = fopen("boot-args.tmp", "wb");
    if (!bootfile) return 2;
    fwrite(&nboot, 1, sizeof(nboot), bootfile);
    char *b = boot;
    for (int i = 0; i < nboot; i++) b += strlen(b) + 1;
    fwrite(boot, 1, b - boot, bootfile);
    fclose(bootfile);
    /* Check the argv that the ARM launcher will send to the next program. */
    char *item = menu;
    while (*(uint32_t *)item) {
        item += *(uint32_t *)item;
        char *name = item + 4, *args = name + strlen(name) + 1;
        if (get_first_arg(args)) {
            char output[4096];
            extract_args_t state = {0, 1, output + sizeof(output)};
            if (!extract_args(&state, args, output) || state.argc < 0) return 2;
        }
    }
    file = fopen(argv[3], "wb");
    if (!file) return 2;
    fwrite(menu, 1, 65536, file);
    fclose(file); free(menu);
    return 0;
}
