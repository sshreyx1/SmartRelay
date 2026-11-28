/* freertos_hooks.c - fail loudly during integration instead of silently
 * corrupting memory. */
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

void vApplicationStackOverflowHook (TaskHandle_t p_task, char* p_name)
{
    (void)p_task;
    printf("FATAL: stack overflow in task '%s'\n", p_name);
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook (void)
{
    printf("FATAL: FreeRTOS heap exhausted\n");
    for (;;)
    {
    }
}

/*** end of file ***/

