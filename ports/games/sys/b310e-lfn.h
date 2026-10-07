/* ASCII FAT32 long-name creation for the fpdoom filesystem.
 * SPDX-License-Identifier: GPL-2.0-or-later */
struct b310e_alias { uint8_t name[11]; };
static int b310e_alias_used(void *opaque, fat_entry_t *entry)
{
    return !memcmp(entry->raw, ((struct b310e_alias*)opaque)->name, 11);
}

fat_entry_t *fat_create_name(fatdata_t *fat, unsigned dir, const char *name)
{
    unsigned len = strlen(name), slots, i, count = 0, index, ended = 0;
    uint32_t sectors[22], sector, cluster = dir;
    uint8_t offsets[22], dos[11], checksum = 0;
    struct b310e_alias alias;
    if (!len || len > 255 || name[len-1] == ' ' || name[len-1] == '.') return NULL;
    for (i = 0; i < len; i++)
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] >= 127 ||
            strchr("\"*/:<>?\\|", name[i])) return NULL;
    if (make_dos_name(name, dos)) return fat_create_name_sfn(fat, dir, name);
    /* Keep the original name in LFN entries; give its alias a collision-free tail. */
    const char *dot = strrchr(name, '.');
    memset(alias.name, ' ', 11);
    if (dot) for (i = 0; i < 3 && dot[i+1]; i++) {
        unsigned c = (unsigned char)dot[i+1];
        alias.name[8+i] = c >= 'a' && c <= 'z' ? c - 32 :
            ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_');
    }
    for (unsigned n = 1; n < 1000000; n++) {
        char tail[9]; unsigned digits, stem;
        unsigned value = n;
        digits = 0;
        do { tail[7-digits++] = '0' + value % 10; value /= 10; } while (value);
        tail[7-digits] = '~';
        stem = 7-digits;
        memset(alias.name, '_', stem);
        for (i = 0; i < stem && name[i] && (!dot || name+i < dot); i++) {
            unsigned c = (unsigned char)name[i];
            alias.name[i] = c >= 'a' && c <= 'z' ? c - 32 :
                            ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_');
        }
        memcpy(alias.name + stem, tail + stem, 8-stem);
        int found = fat_enum_entry(fat, dir, b310e_alias_used, &alias);
        if (found == 1) { if (n == 999999) return NULL; continue; }
        if (fat->flags & FAT_IO_ERROR) return NULL;
        break;
    }
    for (i = 0; i < 11; i++) checksum = ((checksum << 7 | checksum >> 1) + alias.name[i]) & 255;
    slots = (len + 12) / 13;
    /* Find one consecutive run, including sector/cluster boundaries. */
    for (unsigned visited = 0; visited < fat->cnum; visited++) {
        if (cluster - 2 >= fat->cnum) return NULL;
        sector = ((cluster-2) << fat->csh) + fat->data_seg;
        for (unsigned s = 0; s < fat->csize; s++, sector++) {
            uint8_t *buf = fat_read_sec(fat, fat->buf, sector);
            if (!buf) return NULL;
            for (index = 0; index < 16; index++) {
                unsigned first = buf[index*32];
                if (!first) ended = 1;
                if (ended || first == 0xe5) {
                    sectors[count] = sector; offsets[count++] = index;
                    if (count == slots + 1 + ended) goto found;
                } else count = 0;
            }
        }
        unsigned start = 0;
        uint8_t *chain = fat_read_chain(fat, cluster);
        if (!chain) return NULL;
        unsigned added = ((uint32_t*)chain)[cluster & 127] >= 0xffffff8;
        unsigned next = fat_alloc_clust(fat, cluster, &start);
        if (next - 2 >= fat->cnum) return NULL;
        if (added && !fat_dir_expand(fat, next)) return NULL;
        cluster = next;
    }
    return NULL;
found:
    /* Preserve the end marker even when the last entry crosses a sector. */
    if (ended) {
        uint8_t *buf = fat_read_sec(fat, fat->buf, sectors[slots+1]);
        if (!buf) return NULL;
        buf[offsets[slots+1]*32] = 0;
        fat->flags |= FAT_FLUSH_BUF1;
    }
    for (i = 0; i <= slots; i++) {
        uint8_t *buf = fat_read_sec(fat, fat->buf, sectors[i]);
        if (!buf) return NULL;
        fat_entry_t *p = (fat_entry_t*)buf + offsets[i];
        memset(p, 0, 32);
        if (i == slots) {
            memcpy(p->raw, alias.name, 11);
            p->entry.attr = FAT_ATTR_ARC;
            fat->flags |= FAT_FLUSH_BUF1;
            return p;
        }
        unsigned ordinal = slots-i;
        p->lfn.seq = ordinal | (i ? 0 : 0x40);
        p->lfn.attr = FAT_ATTR_LFN;
        p->lfn.chk = checksum;
        for (unsigned j = 0; j < 13; j++) {
            unsigned pos = (ordinal-1)*13+j;
            unsigned c = pos < len ? (unsigned char)name[pos] : pos == len ? 0 : 0xffff;
            unsigned off = fat_lfn_idx[j];
            p->raw[off] = c; p->raw[off+1] = c >> 8;
        }
        fat->flags |= FAT_FLUSH_BUF1;
    }
    return NULL;
}
