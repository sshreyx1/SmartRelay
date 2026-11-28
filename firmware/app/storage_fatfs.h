#ifndef STORAGE_FATFS_H
#define STORAGE_FATFS_H

#include <stdbool.h>
#include <stddef.h>

#include "alfp_port.h"

/* Mount the SD card (call once at boot, from the comms task). */
bool                  storage_fatfs_mount(void);
const alfp_storage_t* storage_fatfs(void);

/* One line for 's': SD file handles in use and the last FatFs error (code +
 * operation + path).
 * FR codes: 1 DISK_ERR, 2 INT_ERR, 3 NOT_READY, 4 NO_FILE, 5 NO_PATH, 6
 * INVALID_NAME, 7 DENIED, 8 EXIST, 9 INVALID_OBJECT, 10 WRITE_PROTECTED, 11
 * INVALID_DRIVE, 18 TOO_MANY_OPEN_FILES. */
void storage_fatfs_status(char* p_out, size_t cap);

#endif

/*** end of file ***/

