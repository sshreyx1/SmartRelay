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

# Matches firmware/CMakeLists.txt's own compile definitions for these same
# sources, so the ALFP/audio behaviour is identical on both builds.
APP_DEFINES := -DAUDIO_SECONDS=5u -DALFP_LORA_GAP_MS=500u -DNODE_ID=$(or $(NODE_ID),2)

ALFP_SRCS := $(wildcard $(SMARTRELAY_ROOT)/alfp/src/*.c) $(SMARTRELAY_ROOT)/common/uplink.c
SENSING_SRCS := $(wildcard $(SMARTRELAY_ROOT)/firmware/sensing/*.c)
SENSOR_DRIVER_SRCS := $(SMARTRELAY_ROOT)/third_party/sensors/pir.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/sound.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/mic.c \
                       $(SMARTRELAY_ROOT)/third_party/sensors/DHT22.c

APP_SRCS := $(ALFP_SRCS) $(SENSING_SRCS) $(SENSOR_DRIVER_SRCS)
APP_OBJS := $(addprefix ./mtkernel_3/app_libs/,$(notdir $(APP_SRCS:.c=.o)))
APP_DEPS := $(APP_OBJS:.o=.d)

OBJS += $(APP_OBJS)
C_DEPS += $(APP_DEPS)

# DHT22.c's 40 us "release the line" wait must be exact - same reasoning and
# fix as firmware/CMakeLists.txt's set_source_files_properties for the
# FreeRTOS build: under any RTOS, sleep_us() can be made to block the task
# and wake it late. Busy-wait instead, without touching Maryam's source.
./mtkernel_3/app_libs/DHT22.o: APP_DEFINES += -Dsleep_us=busy_wait_us

# Generates one explicit per-file rule, same shape as pico_rp2040.mk's own
# WIFI_LOCAL_OBJS/USB_OBJS rules, just written once and stamped out per file
# instead of repeated by hand for everything under alfp/sensing/sensors.
define APP_COMPILE_RULE
./mtkernel_3/app_libs/$(notdir $(1:.c=.o)): $(1)
	@echo 'Building SmartRelay source: $$<'
	@mkdir -p "$$(@D)"
	$(GCC) $(CFLAGS) $(APP_DEFINES) -D$(TARGET) $(APP_INCLUDES) $(INCPATH) -MF"$$(@:%.o=%.d)" -MT"$$(@)" -c -o "$$@" "$$<"
endef

$(foreach src,$(APP_SRCS),$(eval $(call APP_COMPILE_RULE,$(src))))
