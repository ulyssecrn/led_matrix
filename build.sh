#!/bin/sh
# Build the firmware into build/out.
# RAM is tight on the ESP32-S2 (no PSRAM), so to leave room for WiFi and OTA:
# - 4-bit color (instead of 8) shrinks the panel's DMA buffers
# - no USB CDC serial (TinyUSB stack); flashing over USB uses the ROM's DFU mode anyway
set -e
cd "$(dirname "$0")"
arduino-cli compile --fqbn esp32:esp32:esp32s2:CDCOnBoot=default \
  --build-property "compiler.cpp.extra_flags=-DPIXEL_COLOR_DEPTH_BITS=4" \
  --output-dir build/out .
