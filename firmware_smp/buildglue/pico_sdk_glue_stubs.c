/*
 * pico_sdk_glue_stubs.c - Task C3 (CURVEBALL round 3): a handful of symbols Pico SDK driver code
 * (hardware_irq/irq.c's unconditional default-vector-table initialization, newlib's _sbrk) expects
 * to come from Pico SDK's own startup code (src/rp2_common/pico_crt0/crt0.S) or its own linker
 * script -- neither of which this build uses, since this kernel has its own startup
 * (kernel/sysdepend/cpu/rp2040/boot_2nd.c, vector_tbl.c) and its own linker script
 * (etc/linker/pico_rp2040/tkernel_map.ld). Confirmed needed by a real linker run; not previously
 * reachable because nothing pulled Pico SDK driver code in before this round.
 *
 * __unhandled_user_irq(): crt0.S's real implementation reads the active IRQ number out of IPSR
 * and reports it to a panic handler. Our kernel's own vector table (vector_tbl.c) is the one
 * actually installed in hardware -- this symbol only exists to satisfy irq.c's unconditional
 * default-vector-table initialization code, which (with PICO_DISABLE_SHARED_IRQ_HANDLERS=1, see
 * sources.mk) never actually installs that table anywhere the CPU reads from. Whether it can ever
 * really be invoked on real hardware in this configuration is UNVERIFIED; this stub exists to
 * link, not because its behavior has been exercised. If it is ever genuinely called, looping here
 * is at least safer than undefined behavior from a missing symbol.
 *
 * `end`: a linker-script symbol (heap/static-data boundary) that newlib's real _sbrk()
 * implementation (pulled in once anything needs libm's errno path) expects. This kernel manages
 * its own memory (lib/libtk/kmalloc.c) and was never going to call _sbrk() for real; this is a
 * placeholder so the symbol merely resolves, not a real heap. Calling malloc()/_sbrk() from
 * anything linked into this kernel would be a bug independent of this stub.
 */
#include <stdint.h>

void __unhandled_user_irq(void)
{
    for (;;)
    {
    }
}

/* A single zero-length placeholder "heap": real heap use should go through kmalloc(), never
 * through newlib's _sbrk()/malloc(). */
uint8_t end[1];
