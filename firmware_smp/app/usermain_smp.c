/*
 * usermain_smp.c - the kernel's entry point, overlaid into
 * third_party/mtk3smp_rp2040/app_program/ by buildglue/build_smp.sh (never
 * committed into the submodule - see third_party/MTK3SMP_PROVENANCE.md).
 * Replaces the port's own app_main.c/demo_tasks.c, which define the same
 * usermain() symbol and would otherwise collide with this one.
 */
#include <tk/tkernel.h>

void app_main_smp(void); /* firmware_smp/app/main_smp.c */
/* firmware_smp/buildglue/pico_runtime_mtk3.c */
void smartrelay_runtime_init(void);

EXPORT INT usermain (void)
{
    /* Pico SDK runtime pieces (timer tick, clock bookkeeping, peripheral
     * resets) before any driver runs; see pico_runtime_mtk3.c. */
    smartrelay_runtime_init();
    app_main_smp();
    /* usermain must not return (brief/app_main.c:522-524: the kernel shuts
     * down on return); the real tasks it created now run forever. */
    tk_slp_tsk(TMO_FEVR);
    return 0;
}

/*** end of file ***/

