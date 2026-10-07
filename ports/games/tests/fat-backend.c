#include <stdint.h>
int read_sector(uint32_t, uint8_t *);
int write_sector(uint32_t, uint8_t *);
#define FAT_READ_SYS if (read_sector(sector, buf)) break;
#define FAT_WRITE_SYS if (write_sector(sector, buf)) break;
#include "microfat.h"
