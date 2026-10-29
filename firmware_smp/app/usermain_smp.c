/*
 * usermain_smp.c - the kernel's entry point, overlaid into
 * third_party/mtk3smp_rp2040/app_program/ by build/build_smp.sh (never
 * committed into the submodule - see third_party/MTK3SMP_PROVENANCE.md).
 * Replaces the port's own app_main.c/demo_tasks.c, which define the same
 * usermain() symbol and would otherwise collide with this one.
 */
#include <tk/tkernel.h>

void app_main_smp(void); /* firmware_smp/app/main_smp.c */

EXPORT INT usermain(void)
{
    app_main_smp();
    /* usermain must not return (brief/app_main.c:522-524: the kernel shuts
     * down on return); the real tasks it created now run forever. */
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
