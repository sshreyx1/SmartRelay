################################################################################
# sources.mk - SmartRelay's own build glue for third_party/mtk3smp_rp2040.
#
# Copied (never committed) into third_party/mtk3smp_rp2040/build_make/ by
# build_smp.sh, which is what makefile:25's documented `-include sources.mk`
# hook is for (docs/curveball_findings.md Step 2 item 8). OBJS/C_DEPS use
# `+=`, and INCPATH/CFLAGS are only ever read (never reassigned) inside our
# own recipes below, so load order relative to pico_rp2040.mk's later
# `CFLAGS := ...` reset does not matter - see the findings doc for why a
# naive `CFLAGS += -I...` here would have been silently discarded.
#
# SCOPE (see firmware_smp/buildglue/README.md "What is NOT wired in yet"):
# this compiles the kernel + the ported app + alfp + the sensing glue + the
# raw sensor drivers. It does NOT yet compile third_party/lora_mesh,
# third_party/wifi_mesh, or the SD/FatFs storage layer - radios.c and
# storage_fatfs.c will fail to link until those are added the same way.
################################################################################

SMARTRELAY_ROOT ?= ../../..

APP_INCLUDES := -I"$(SMARTRELAY_ROOT)/alfp/include" \
                -I"$(SMARTRELAY_ROOT)/mesh" \
                -I"$(SMARTRELAY_ROOT)/common" \
                -I"$(SMARTRELAY_ROOT)/firmware/sensing" \
                -I"$(SMARTRELAY_ROOT)/firmware/app" \
                -I"$(SMARTRELAY_ROOT)/third_party/sensors" \
                -I"$(SMARTRELAY_ROOT)/firmware_smp"

# Round 3 (CURVEBALL), confirmed with a real toolchain: main_smp.c/log.c/rtos_port_mtk3.c/
# usermain_smp.c are overlaid directly into app_program/ by build_smp.sh, so they are compiled by
# the kernel's own GENERIC app_program/subdir.mk rule (TEMP_SRCS = $(wildcard ../app_program/*.c)),
# not by APP_COMPILE_RULE below -- that generic rule only ever passes $(INCPATH), never
# $(APP_INCLUDES), so main_smp.c's #include "alfp.h" etc. could not be found ("fatal error: alfp.h:
# No such file or directory"). INCPATH is defined with plain `=` in makefile:23 and never
# reassigned afterward (confirmed by inspection), so appending to it here is visible, lazily, to
# every rule that references it, including the generic one -- the same reasoning this file's own
# header comment already gives for why CFLAGS could NOT be handled this way (it IS reassigned with
# `:=` later, by pico_rp2040.mk).
INCPATH += $(APP_INCLUDES)

# Task C3 (CURVEBALL round 3): the Pico SDK hardware driver library this port never compiled
# before (docs/curveball_findings.md / firmware_smp/buildglue/README.md "What is NOT wired in
# yet"). Discovered incrementally against a real toolchain -- each header/source pair here is one
# that an actual link error named, not guessed up front. GPIO/SPI/UART/ADC/DMA are what
# radios.c/storage_fatfs.c/the sensor drivers actually call; clocks/pll/xosc/watchdog came in only
# because gpio.c/spi.c/etc. themselves call into the clock API to read/set peripheral clock
# sources (confirmed by the exact undefined-reference names at each step, see
# docs/curveball_results.md's C3 section for the real sequence).
PICO_SDK_PATH ?= $(error PICO_SDK_PATH is not set)
PICO_DRIVER_INCLUDES := -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_gpio/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_spi/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_adc/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_dma/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_uart/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_i2c/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_clocks/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_pll/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_xosc/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_watchdog/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_gpio_coproc/include" \
                        -I"$(PICO_SDK_PATH)/src/common/hardware_claim/include" \
                        -I"$(PICO_SDK_PATH)/src/common/pico_time/include" \
                        -I"$(PICO_SDK_PATH)/src/common/pico_sync/include" \
                        -I"$(PICO_SDK_PATH)/src/common/pico_util/include" \
                        -I"$(PICO_SDK_PATH)/src/common/pico_stdlib_headers/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_stdio/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/hardware_ticks/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_bootrom/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_runtime_init/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_runtime/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_time_adapter/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/pico_platform_compiler/include" \
                        -I"$(PICO_SDK_PATH)/src/rp2_common/boot_bootrom_headers/include"
INCPATH += $(PICO_DRIVER_INCLUDES)

PICO_DRIVER_SRCS := $(SMARTRELAY_ROOT)/firmware_smp/buildglue/pico_sdk_glue_stubs.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_gpio/gpio.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_spi/spi.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_adc/adc.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_dma/dma.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_uart/uart.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_i2c/i2c.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_clocks/clocks.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_pll/pll.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_xosc/xosc.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_timer/timer.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_irq/irq.c \
                    $(PICO_SDK_PATH)/src/rp2_common/hardware_sync/sync.c \
                    $(PICO_SDK_PATH)/src/common/pico_time/time.c \
                    $(PICO_SDK_PATH)/src/common/pico_time/timeout_helper.c \
                    $(PICO_SDK_PATH)/src/common/pico_sync/critical_section.c \
                    $(PICO_SDK_PATH)/src/common/pico_sync/lock_core.c \
                    $(PICO_SDK_PATH)/src/common/pico_sync/sem.c \
                    $(PICO_SDK_PATH)/src/common/hardware_claim/claim.c

# Matches firmware/CMakeLists.txt's own compile definitions for these same
# sources, so the ALFP/audio behaviour is identical on both builds. Only APP_COMPILE_RULE below
# passes $(APP_DEFINES) explicitly; main_smp.c itself (overlaid into app_program/, compiled by the
# kernel's own generic rule, same INCPATH-only situation as above) needs AUDIO_SECONDS too
# ("error: 'AUDIO_SECONDS' undeclared", confirmed on a real toolchain run) -- appended to INCPATH
# for the same reason the comment above it is appended there: it is the one variable proven to
# reach every compile rule, generic or custom.
# PICO_DISABLE_SHARED_IRQ_HANDLERS=1: Pico SDK's hardware_irq/irq.c's shared-IRQ-handler-chain
# path (irq_handler_chain_slots et al) is only compiled when this is 0 AND PICO_NO_RAM_VECTOR_TABLE
# is also 0 (its own #if), but something in this build already makes that combination false
# (confirmed by a real link: "undefined reference to irq_handler_chain_slots" even though the
# chain-walking CODE that reads it still compiles unconditionally elsewhere) -- not yet root-
# caused to a specific header/define (see docs/curveball_results.md's C3 section). Forcing this to
# 1 selects irq.c's simpler direct-vector-write path instead, which gets past the link error, but
# whether that direct path is actually compatible with this kernel's OWN vector table
# (kernel/sysdepend/cpu/rp2040/vector_tbl.c), rather than Pico SDK's relocated RAM vector table, is
# UNVERIFIED -- flagged, not assumed. A real toolchain+board test of an actual UART/I2C IRQ firing
# is needed before trusting irq_set_exclusive_handler() at runtime, not just at link time.
APP_DEFINES := -DAUDIO_SECONDS=5u -DALFP_LORA_GAP_MS=500u -DNODE_ID=$(or $(NODE_ID),2) \
              -DPICO_DISABLE_SHARED_IRQ_HANDLERS=1
INCPATH += $(APP_DEFINES)

ALFP_SRCS := $(wildcard $(SMARTRELAY_ROOT)/alfp/src/*.c) $(SMARTRELAY_ROOT)/common/uplink.c
SENSING_SRCS := $(wildcard $(SMARTRELAY_ROOT)/firmware/sensing/*.c)
SENSOR_DRIVER_SRCS := $(SMARTRELAY_ROOT)/third_party/sensors/pir.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/sound.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/mic.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/DHT22.c

APP_SRCS := $(ALFP_SRCS) $(SENSING_SRCS) $(SENSOR_DRIVER_SRCS) $(PICO_DRIVER_SRCS)
APP_OBJS := $(addprefix ./mtkernel_3/app_libs/,$(notdir $(APP_SRCS:.c=.o)))
APP_DEPS := $(APP_OBJS:.o=.d)

OBJS += $(APP_OBJS)
C_DEPS += $(APP_DEPS)

# audio_features.c/sound_features.c call sqrtf/logf/fabsf/log10f/sqrt/lroundf -- need libm, but
# there is no Make variable sources.mk can append to here that both (a) actually reaches the link
# recipe (`$(LINK) $(LFLAGS) -T ... $(OBJS)` at makefile:74) and (b) is not reassigned with `:=`
# in pico_rp2040.mk after this file is processed (LFLAGS is, for the same reason this file's
# header comment already gives for CFLAGS) or treated as a build target if appended to $(OBJS)
# ("No rule to make target '-lm'", confirmed on a real run). build_smp.sh/build_smoke.sh instead
# `sed`-patch the link recipe itself to add `-lm`, in the submodule's working tree only, the same
# pattern already used there for CNF_TIMER_PERIOD -- see that script.

# DHT22.c's 40 us "release the line" wait must be exact - same reasoning and
# fix as firmware/CMakeLists.txt's set_source_files_properties for the
# FreeRTOS build: under any RTOS, sleep_us() can be made to block the task
# and wake it late. Busy-wait instead, without touching Maryam's source.
./mtkernel_3/app_libs/DHT22.o: APP_DEFINES += -Dsleep_us=busy_wait_us

# Generates one explicit per-file rule, same shape as pico_rp2040.mk's own
# WIFI_LOCAL_OBJS/USB_OBJS rules, just written once and stamped out per file
# instead of repeated by hand for everything under alfp/sensing/sensors.
#
# Round 3 (CURVEBALL), confirmed with a real toolchain: GCC/CFLAGS/TARGET were all empty in the
# generated recipe below, because sources.mk is `-include`d at makefile:25, before
# `include pico_rp2040.mk` (makefile:53) defines any of them (`GCC := arm-none-eabi-gcc`) --
# $(eval $(call APP_COMPILE_RULE,...)) expands every un-escaped $(...) reference immediately, at
# eval time, not when the recipe later runs, so the generated command line was effectively empty
# ("Error 127 (ignored)": the shell found no command to run). This means NO file in APP_SRCS has
# ever actually been compiled by this rule before, in any previous round -- main_smp.c itself was
# also never reachable past its own missing-includes bugs (see above) until now. Fixed by
# $$-escaping GCC/CFLAGS/APP_DEFINES/TARGET/APP_INCLUDES/INCPATH exactly like the automatic
# variables below already were, so all of them defer to recipe-execution time.
define APP_COMPILE_RULE
./mtkernel_3/app_libs/$(notdir $(1:.c=.o)): $(1)
	@echo 'Building SmartRelay source: $$<'
	@mkdir -p "$$(@D)"
	$$(GCC) $$(CFLAGS) $$(APP_DEFINES) -D$$(TARGET) $$(APP_INCLUDES) $$(INCPATH) -MF"$$(@:%.o=%.d)" -MT"$$(@)" -c -o "$$@" "$$<"
endef

$(foreach src,$(APP_SRCS),$(eval $(call APP_COMPILE_RULE,$(src))))
