#ifndef STORAGE_FATFS_H
#define STORAGE_FATFS_H

#include <stdbool.h>

#include "alfp_port.h"

/* Mount the SD card (call once at boot, from the comms task). */
bool storage_fatfs_mount(void);
const alfp_storage_t* storage_fatfs(void);

#endif
