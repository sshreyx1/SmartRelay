################################################################################
# sources.mk - SmartRelay's own build glue for third_party/mtk3smp_rp2040.
#
# Copied (never committed) into third_party/mtk3smp_rp2040/build_make/ by
# build_smp.sh, which is what makefile:25's documented `-include sources.mk`
# hook is for (docs/curveball_findings.md Step 2 item 8). OBJS/C_DEPS use
# `+=`, and INCPATH/CFLAGS are only ever read (never reassigned) inside our
# own recipes below, so load order relative to pico_rp2040.mk's later
# `CFLAGS := ...` reset does not matter.
#
# What this builds into the kernel image: the same node firmware the CMake/FreeRTOS build links
# (firmware/CMakeLists.txt) -- ALFP, the uplink, the sensing + sound classifier code, the raw
# sensor drivers, Mridula's LR1121 LoRa mesh, Goutham's ESP-01 WiFi mesh, firmware/app's radios/
# storage/board glue, the carlk3 FatFs + SD-over-SPI driver, and the Pico SDK hardware drivers
# those call -- plus firmware_smp/buildglue/pico_runtime_mtk3.c, which supplies the Pico SDK
# runtime pieces (timer tick, clock bookkeeping, mutexes, sleeps, RNG, malloc lock, printf) that
# Pico SDK's crt0/runtime_init would normally provide and this kernel does not.
#
# Everything except pico_runtime_mtk3.o goes into one static archive (libsmartrelay.a) linked after
# the kernel objects, so -- exactly like the CMake build's static libraries -- only the members
# something actually references are pulled in (the LR1121 driver tree alone has ~40 files, most of
# them modem/GNSS code the mesh never calls). pico_runtime_mtk3.o stays a plain object on purpose:
# it overrides newlib symbols (_sbrk, __malloc_lock, printf, ...) that are only referenced from
# inside libc, so it must already be linked when libc is searched.
################################################################################

SMARTRELAY_ROOT ?= ../../..
PICO_SDK_PATH ?= $(error PICO_SDK_PATH is not set)
FATFS_PICO_PATH ?= $(error FATFS_PICO_PATH is not set (clone of carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico, tools/fetch_deps.sh))

SR := $(SMARTRELAY_ROOT)
SDK := $(PICO_SDK_PATH)
FATFS := $(FATFS_PICO_PATH)/src
LORA_MESH_DIR := $(SR)/third_party/lora_mesh
WIFI_MESH_DIR := $(SR)/third_party/wifi_mesh

APP_INCLUDES := -I"$(SR)/alfp/include" \
                -I"$(SR)/mesh" \
                -I"$(SR)/common" \
                -I"$(SR)/firmware/sensing" \
                -I"$(SR)/firmware/app" \
                -I"$(SR)/third_party/sensors" \
                -I"$(SR)/firmware_smp" \
                -I"$(SR)/firmware_smp/buildglue" \
                -I"$(LORA_MESH_DIR)/lora" \
                -I"$(LORA_MESH_DIR)/src/gpio" \
                -I"$(LORA_MESH_DIR)/src/spi" \
                -I"$(LORA_MESH_DIR)/src/lr1121" \
                -I"$(LORA_MESH_DIR)/src/lr1121/lr1121_printers" \
                -I"$(LORA_MESH_DIR)/src/lr1121/lr1121_modem" \
                -I"$(LORA_MESH_DIR)/src/lr1121/lr1121_common" \
                -I"$(LORA_MESH_DIR)/src/lr1121/lr11xx_driver" \
                -I"$(WIFI_MESH_DIR)" \
                -I"$(FATFS)/include" \
                -I"$(FATFS)/ff15/source" \
                -I"$(FATFS)/sd_driver"

# main_smp.c/log.c/rtos_port_mtk3.c/usermain_smp.c are overlaid into app_program/ by build_smp.sh
# and compiled by the kernel's GENERIC app_program/subdir.mk rule, which only passes $(INCPATH).
# INCPATH is defined with plain `=` at makefile:23 and never reassigned, so appending here reaches
# every compile rule, generic or ours.
INCPATH += $(APP_INCLUDES)

PICO_DRIVER_INCLUDES := -I"$(SDK)/src/rp2_common/hardware_gpio/include" \
                        -I"$(SDK)/src/rp2_common/hardware_spi/include" \
                        -I"$(SDK)/src/rp2_common/hardware_adc/include" \
                        -I"$(SDK)/src/rp2_common/hardware_dma/include" \
                        -I"$(SDK)/src/rp2_common/hardware_uart/include" \
                        -I"$(SDK)/src/rp2_common/hardware_clocks/include" \
                        -I"$(SDK)/src/rp2_common/hardware_pll/include" \
                        -I"$(SDK)/src/rp2_common/hardware_xosc/include" \
                        -I"$(SDK)/src/rp2_common/hardware_watchdog/include" \
                        -I"$(SDK)/src/rp2_common/hardware_gpio_coproc/include" \
                        -I"$(SDK)/src/rp2_common/hardware_pio/include" \
                        -I"$(SDK)/src/rp2_common/hardware_rtc/include" \
                        -I"$(SDK)/src/rp2_common/hardware_ticks/include" \
                        -I"$(SDK)/src/rp2_common/hardware_exception/include" \
                        -I"$(SDK)/src/rp2_common/hardware_boot_lock/include" \
                        -I"$(SDK)/src/common/hardware_claim/include" \
                        -I"$(SDK)/src/common/pico_time/include" \
                        -I"$(SDK)/src/common/pico_sync/include" \
                        -I"$(SDK)/src/common/pico_util/include" \
                        -I"$(SDK)/src/common/pico_stdlib_headers/include" \
                        -I"$(SDK)/src/rp2_common/pico_stdio/include" \
                        -I"$(SDK)/src/rp2_common/pico_printf/include" \
                        -I"$(SDK)/src/rp2_common/pico_rand/include" \
                        -I"$(SDK)/src/rp2_common/pico_aon_timer/include" \
                        -I"$(SDK)/src/rp2_common/pico_bootrom/include" \
                        -I"$(SDK)/src/rp2_common/pico_runtime_init/include" \
                        -I"$(SDK)/src/rp2_common/pico_runtime/include" \
                        -I"$(SDK)/src/rp2_common/pico_time_adapter/include" \
                        -I"$(SDK)/src/rp2_common/pico_platform_compiler/include" \
                        -I"$(SDK)/src/rp2_common/boot_bootrom_headers/include" \
                        -I"$(SDK)/src/rp2_common/cmsis/stub/CMSIS/Core/Include" \
                        -I"$(SDK)/src/rp2_common/cmsis/stub/CMSIS/Device/RP2040/Include"
INCPATH += $(PICO_DRIVER_INCLUDES)

# Same compile definitions firmware/CMakeLists.txt gives these sources, so behaviour matches the
# FreeRTOS build. Appended to INCPATH too, for the same reach-every-rule reason as above
# (main_smp.c needs NODE_ID/AUDIO_SECONDS/HAVE_*).
APP_DEFINES := -DNODE_ID=$(or $(NODE_ID),2) \
               -DAUDIO_SECONDS=5u \
               -DSOUND_LEGACY_MODE=0 \
               -DALFP_LORA_GAP_MS=500u \
               -DHAVE_LORA_MESH=1 -DHAVE_WIFI_MESH=1 \
               -DRADIO_BUSY=6 -DRADIO_IRQ=7 \
               -DSD_SPI_INST=1 -DSD_SCK=10 -DSD_MOSI=11 -DSD_MISO=12 -DSD_CS=15 -DSPI1_BAUD_HZ=$(or $(SPI1_BAUD_HZ),4000000)u \
               -DPICO_PRINTF_ALWAYS_INCLUDED=1 \
               -DPICO_MAX_SHARED_IRQ_HANDLERS=8u
INCPATH += $(APP_DEFINES)

# ---- sources, grouped; the group name prefixes each object so equal basenames (lora_mesh's
# ---- src/gpio/gpio.c vs the SDK's hardware_gpio/gpio.c, its spi.c vs hardware_spi/spi.c) never
# ---- collide in app_libs/.
SRC_alfp := $(wildcard $(SR)/alfp/src/*.c) $(SR)/common/uplink.c
SRC_sensing := $(SR)/firmware/sensing/sensing.c \
               $(SR)/firmware/sensing/audio_capture.c \
               $(SR)/firmware/sensing/audio_features.c \
               $(SR)/firmware/sensing/sound_features.c \
               $(SR)/firmware/sensing/sound_fft.c \
               $(SR)/firmware/sensing/sound_tables.c \
               $(SR)/firmware/sensing/sound_classify.c \
               $(SR)/firmware/sensing/sound_image_pick.c
SRC_sensors := $(SR)/third_party/sensors/pir.c \
               $(SR)/third_party/sensors/sound.c \
               $(SR)/third_party/sensors/mic.c \
               $(SR)/third_party/sensors/DHT22.c
SRC_app := $(SR)/firmware/app/radios.c \
           $(SR)/firmware/app/storage_fatfs.c \
           $(SR)/firmware/app/hw_config.c \
           $(SR)/firmware/app/board.c
SRC_lora := $(LORA_MESH_DIR)/lora/lora_radio.c \
            $(LORA_MESH_DIR)/lora/lora_mesh.c \
            $(LORA_MESH_DIR)/src/gpio/gpio.c \
            $(LORA_MESH_DIR)/src/spi/spi.c \
            $(wildcard $(LORA_MESH_DIR)/src/lr1121/*.c) \
            $(wildcard $(LORA_MESH_DIR)/src/lr1121/lr1121_common/*.c) \
            $(wildcard $(LORA_MESH_DIR)/src/lr1121/lr1121_modem/*.c) \
            $(wildcard $(LORA_MESH_DIR)/src/lr1121/lr1121_printers/*.c) \
            $(wildcard $(LORA_MESH_DIR)/src/lr1121/lr11xx_driver/*.c)
SRC_wifi := $(WIFI_MESH_DIR)/wifi_mesh.c $(WIFI_MESH_DIR)/wst_link.c
# carlk3 FatFs + SD card over SPI. The SDIO half (PIO program, its DMA IRQ) is not built: these
# boards use the SPI slot wiring (hw_config.c), and driver_glue_mtk3.c stubs the two SDIO entry
# points sd_card.c references.
SRC_fatfs := $(FATFS)/ff15/source/ff.c \
             $(FATFS)/ff15/source/ffsystem.c \
             $(FATFS)/ff15/source/ffunicode.c \
             $(FATFS)/sd_driver/sd_card.c \
             $(FATFS)/sd_driver/sd_timeouts.c \
             $(FATFS)/sd_driver/SPI/my_spi.c \
             $(FATFS)/sd_driver/SPI/sd_card_spi.c \
             $(FATFS)/sd_driver/SPI/sd_spi.c \
             $(FATFS)/src/crc.c \
             $(FATFS)/src/glue.c \
             $(FATFS)/src/my_debug.c \
             $(FATFS)/src/util.c \
             $(FATFS)/src/f_util.c \
             $(SR)/firmware_smp/buildglue/driver_glue_mtk3.c
# Pico SDK drivers. hardware_irq/irq.c and pico_time/time.c are NOT here on purpose:
# usb_sdk_compat_overlay.c and pico_runtime_mtk3.c provide those APIs on the kernel instead.
SRC_sdk := $(SDK)/src/rp2_common/hardware_gpio/gpio.c \
           $(SDK)/src/rp2_common/hardware_spi/spi.c \
           $(SDK)/src/rp2_common/hardware_adc/adc.c \
           $(SDK)/src/rp2_common/hardware_dma/dma.c \
           $(SDK)/src/rp2_common/hardware_uart/uart.c \
           $(SDK)/src/rp2_common/hardware_clocks/clocks.c \
           $(SDK)/src/rp2_common/hardware_pll/pll.c \
           $(SDK)/src/rp2_common/hardware_xosc/xosc.c \
           $(SDK)/src/rp2_common/hardware_timer/timer.c \
           $(SDK)/src/rp2_common/hardware_sync/sync.c \
           $(SDK)/src/rp2_common/hardware_sync_spin_lock/sync_spin_lock.c \
           $(SDK)/src/rp2_common/pico_printf/printf.c \
           $(SDK)/src/common/pico_time/timeout_helper.c \
           $(SDK)/src/common/hardware_claim/claim.c
LIB_GROUPS := alfp sensing sensors app lora wifi fatfs sdk

GLUE_SRC := $(SR)/firmware_smp/buildglue/pico_runtime_mtk3.c

APP_LIB_DIR := ./mtkernel_3/app_libs
obj_of = $(APP_LIB_DIR)/$(1)__$(notdir $(2:.c=.o))

LIB_OBJS := $(foreach g,$(LIB_GROUPS),$(foreach s,$(SRC_$(g)),$(call obj_of,$(g),$(s))))
GLUE_OBJ := $(call obj_of,glue,$(GLUE_SRC))
SMARTRELAY_LIB := $(APP_LIB_DIR)/libsmartrelay.a
SMARTRELAY_AR ?= arm-none-eabi-ar

OBJS += $(GLUE_OBJ)
C_DEPS += $(LIB_OBJS:.o=.d) $(GLUE_OBJ:.o=.d)

# Referenced from the link recipe, which build_smp.sh patches (working tree only) to read
#   $(EXE_FILE).elf: $(OBJS) $(E2U) $(SMARTRELAY_LINK_DEPS)
#   ... $(OBJS) $(SMARTRELAY_LINK_EXTRA) -lm --specs=nosys.specs
# because no variable this file could append to survives to that recipe otherwise (LFLAGS is
# reassigned with := by pico_rp2040.mk; anything appended to OBJS is also treated as a target).
SMARTRELAY_LINK_DEPS := $(SMARTRELAY_LIB) $(SR)/firmware_smp/buildglue/smartrelay_sections.ld
SMARTRELAY_LINK_EXTRA := $(SMARTRELAY_LIB) -Wl,-T,"$(SR)/firmware_smp/buildglue/smartrelay_sections.ld"

$(SMARTRELAY_LIB): $(LIB_OBJS)
	@echo 'Archiving: $@'
	@rm -f "$@"
	$(SMARTRELAY_AR) rcs "$@" $^

# DHT22.c's 40 us "release the line" wait must be exact - same reasoning and fix as
# firmware/CMakeLists.txt's set_source_files_properties for the FreeRTOS build.
$(call obj_of,sensors,DHT22.c): APP_DEFINES += -Dsleep_us=busy_wait_us

# DHT22.c decodes each bit by counting loop iterations. On the dual-core build core 0 (radios, SD
# card) shares the 16 KB flash cache with core 1, and cache misses landing inside a pulse
# corrupted ~40% of readings on real hardware (FreeRTOS build, one core: 0%). Its code is renamed
# into a .data.* section after compiling, so the kernel's own linker script places it in RAM and
# its reset handler copies it there with the rest of .data -- run from RAM, no cache involved,
# and Maryam's source is untouched.
SMARTRELAY_OBJCOPY ?= arm-none-eabi-objcopy
APP_POST_COMPILE := @true
$(call obj_of,sensors,DHT22.c): APP_POST_COMPILE = $(SMARTRELAY_OBJCOPY) \
    --rename-section .text=.data.smartrelay_ramfunc_dht22,alloc,load,contents,code "$@"

# One explicit rule per source. Everything is $$-escaped so it expands when the recipe runs:
# this file is included before pico_rp2040.mk defines GCC/CFLAGS/TARGET.
define APP_COMPILE_RULE
$(call obj_of,$(1),$(2)): $(2)
	@echo 'Building SmartRelay source: $$<'
	@mkdir -p "$$(@D)"
	$$(GCC) $$(CFLAGS) -D$$(TARGET) $$(INCPATH) -MF"$$(@:%.o=%.d)" -MT"$$(@)" -c -o "$$@" "$$<"
	$$(APP_POST_COMPILE)
endef

$(foreach g,$(LIB_GROUPS),$(foreach s,$(SRC_$(g)),$(eval $(call APP_COMPILE_RULE,$(g),$(s)))))
$(eval $(call APP_COMPILE_RULE,glue,$(GLUE_SRC)))
