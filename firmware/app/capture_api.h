/*
 * capture_api.h - how the sensor code asks for a capture.   Used by Members 3 and 4.
 *
 * Call app_request_capture() from ANY task when the trigger fusion decides to capture. Fill in
 * the fields you own (see capture_meta.h) and leave the rest as capture_meta_clear() set them.
 * The comms task then picks the next sample image, stamps node id / capture id / time, queues the
 * capture on the SD card and sends it when a radio can reach the bridge.
 *
 * Thread-safe (FreeRTOS queue). Never blocks: returns false if 4 requests are already waiting.
 */
#ifndef CAPTURE_API_H
#define CAPTURE_API_H

#include <stdbool.h>

#include "capture_meta.h"

bool app_request_capture(const capture_meta_t* sensors);

#endif
