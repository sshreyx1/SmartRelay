#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>

#define SOUND_PIN 14 // Connected to GP14

void sound_init(void);
bool sound_is_triggered(void);

#endif

/*** end of file ***/

