################################################################################
# sources.mk - build glue for the smp_smoke app (Task C2), copied (never committed) into
# third_party/mtk3smp_rp2040/build_make/ by build_smoke.sh.
#
# Deliberately a no-op: unlike firmware_smp/buildglue/sources.mk (which compiles alfp/sensing/
# sensor-driver sources that live OUTSIDE app_program/, so need their own explicit compile
# rules), every one of this app's sources (log.c, rtos_port_mtk3.c, smp_smoke.c,
# usermain_smoke.c) is copied straight into app_program/ by build_smoke.sh -- and
# build_make/mtkernel_3/app_program/subdir.mk (already part of the vendored kernel's own build,
# see its `TEMP_SRCS = $(wildcard ../app_program/*.c)`) already discovers and compiles every .c
# file physically present there, into mtkernel_3/app_program/*.o, linked automatically. Adding a
# second compile rule for the same files here (as an earlier version of this file did) pointed
# the linker at object paths that were never actually built -- confirmed with a real toolchain
# run, round 3 (CURVEBALL): "ld.exe: cannot find ./mtkernel_3/app_libs/log.o". Fixed by removing
# the redundant rule rather than by building the same source twice under two names.
################################################################################
