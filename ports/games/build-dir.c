/* Snapshot a FAT directory for Build's portable directory API. */
#include "fatfile.h"
typedef struct b310e_dir_entry {
    struct b310e_dir_entry *next;
    struct Bdirent info;
    char name[1];
} b310e_dir_entry;
typedef struct {b310e_dir_entry *first,*last,*current;unsigned count;int failed;} b310e_dir;
static int directory_entry(void *ctx,fat_entry_t *entry,const char *name) {
    b310e_dir *dir=ctx;unsigned len=strlen(name);
    if(dir->count>=4096){dir->failed=1;return 1;}
    b310e_dir_entry *item=malloc(sizeof(*item)+len);
    if(!item){dir->failed=1;return 1;}
    memset(item,0,sizeof(*item));strcpy(item->name,name);
    item->info.name=item->name;item->info.namlen=len;item->info.size=entry->entry.size;
    item->info.mode=(entry->entry.attr&FAT_ATTR_DIR?S_IFDIR:S_IFREG)|S_IREAD|S_IWRITE;
    if(dir->last)dir->last->next=item;else dir->first=item;
    dir->last=item;dir->count++;return 0;
}
int Bclosedir(BDIR *handle) {
    b310e_dir *dir=(void*)handle;
    if(!dir)return -1;
    b310e_dir_entry *item=dir->first;
    while(item){b310e_dir_entry *next=item->next;free(item);item=next;}
    free(dir);return 0;
}
BDIR *Bopendir(const char *name) {
    unsigned clust=fat_dir_clust(&fatdata_glob,name);
    if(!clust)return NULL;
    b310e_dir *dir=calloc(1,sizeof(*dir));if(!dir)return NULL;
    fat_enum_name(&fatdata_glob,clust,directory_entry,dir);
    if(dir->failed || (fatdata_glob.flags&FAT_IO_ERROR)){Bclosedir((void*)dir);return NULL;}
    dir->current=dir->first;return (void*)dir;
}
struct Bdirent *Breaddir(BDIR *handle) {
    b310e_dir *dir=(void*)handle;
    if(!dir || !dir->current)return NULL;
    b310e_dir_entry *item=dir->current;dir->current=item->next;return &item->info;
}
