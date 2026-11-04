/*
 * smartrelay_sections.ld - extra output section for Pico SDK code linked into the uT-Kernel 3
 * image. Passed to the linker with -T after the kernel's own script (build_smp.sh patches the
 * link line), so INSERT places it relative to that script's sections without editing it.
 *
 * .mutex_array        auto_init_mutex() objects (carlk3 SD driver). NOLOAD and zeroed by
 *                     smartrelay_runtime_init() before first use, because the kernel's reset
 *                     handler only zeroes __bss_start..__bss_end.
 * .uninitialized_data __uninitialized_ram() objects: deliberately left as they are at reset.
 *
 * Placed BEFORE .bss, not after: the kernel's own memory pool starts at __bss_end
 * (kernel/sysdepend/cpu/core/armv6m/reset_hdl.c), so anything after .bss would overlap it.
 */
SECTIONS
{
    .smartrelay_noinit (NOLOAD) : ALIGN(4)
    {
        __mutex_array_start = .;
        KEEP(*(SORT_BY_NAME(.mutex_array.*)))
        KEEP(*(.mutex_array))
        __mutex_array_end = .;
        . = ALIGN(4);
        *(.uninitialized_data*)
        . = ALIGN(4);
    }
}
INSERT BEFORE .bss;
