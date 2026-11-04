################################################################################
# sources.mk - build glue for the smp_smoke app (Task C2), copied (never committed) into
# third_party/mtk3smp_rp2040/build_make/ by build_smoke.sh. Deliberately much smaller than
# firmware_smp/buildglue/sources.mk: no alfp, no sensing, no sensor drivers, no radios, no SD --
# this app proves the kernel port itself, independent of everything else.
################################################################################

SMARTRELAY_ROOT ?= ../../..

APP_INCLUDES := -I"$(SMARTRELAY_ROOT)/firmware_smp"

APP_SRCS := $(SMARTRELAY_ROOT)/firmware_smp/app/log.c $(SMARTRELAY_ROOT)/firmware_smp/rtos_port_mtk3.c \
           $(SMARTRELAY_ROOT)/firmware_smp/smoke/smp_smoke.c
APP_OBJS := $(addprefix ./mtkernel_3/app_libs/,$(notdir $(APP_SRCS:.c=.o)))
APP_DEPS := $(APP_OBJS:.o=.d)

OBJS += $(APP_OBJS)
C_DEPS += $(APP_DEPS)

define APP_COMPILE_RULE
./mtkernel_3/app_libs/$(notdir $(1:.c=.o)): $(1)
	@echo 'Building smp_smoke source: $$<'
	@mkdir -p "$$(@D)"
	$(GCC) $(CFLAGS) -D$(TARGET) $(APP_INCLUDES) $(INCPATH) -MF"$$(@:%.o=%.d)" -MT"$$(@)" -c -o "$$@" "$$<"
endef

$(foreach src,$(APP_SRCS),$(eval $(call APP_COMPILE_RULE,$(src))))
