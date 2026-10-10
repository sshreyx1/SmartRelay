#!/usr/bin/env bash
# fetch_deps.sh - clone the three firmware dependencies at the exact versions this code was built with.
#   ./tools/fetch_deps.sh [dir]        (default: ./deps)
# Then copy the three export lines it prints into your terminal and build (see README.md).
set -euo pipefail
DIR="${1:-deps}"
mkdir -p "$DIR" && cd "$DIR"

clone() { # url dir commit
  if [ ! -d "$2" ]; then git clone -q "$1" "$2"; fi
  git -C "$2" fetch -q --depth 1 origin "$3" 2>/dev/null || true
  git -C "$2" checkout -q "$3"
}

clone https://github.com/raspberrypi/pico-sdk.git pico-sdk 2.1.1
git -C pico-sdk submodule update -q --init --depth 1 lib/tinyusb
clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git FreeRTOS-Kernel 8be86d4a24fd4091f8f4192018423ab590f408db
clone https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git no-OS-FatFS-SD-SDIO-SPI-RPi-Pico d5e453404cdbfaa55ab30d285b6ab0b730e84a05

P="$(pwd)"
echo
echo "Done. Now run:"
echo "  export PICO_SDK_PATH=$P/pico-sdk"
echo "  export FREERTOS_KERNEL_PATH=$P/FreeRTOS-Kernel"
echo "  export FATFS_PICO_PATH=$P/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico"
