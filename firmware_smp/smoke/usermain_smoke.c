/*
 * usermain_smoke.c - kernel entry point for the smp_smoke app (Task C2),
 * overlaid into third_party/mtk3smp_rp2040/app_program/ by build_smoke.sh.
 * Mirrors firmware_smp/app/usermain_smp.c exactly, pointed at app_main_smoke()
 * instead of app_main_smp() so the two builds never share a usermain()
 * definition by accident.
 */
#include <tk/tkernel.h>

void app_main_smoke(void); /* firmware_smp/smoke/smp_smoke.c */

EXPORT INT usermain (void)
{
    app_main_smoke();
    tk_slp_tsk(TMO_FEVR); /* usermain must not return: real tasks run forever */
    return 0;
}

/*** end of file ***/

