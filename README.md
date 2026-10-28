# led_matrix

A WiFi clock for a 64x64 HUB75 LED panel driven by a **Huidu HD-WF1**, a cheap
LED-sign controller card that happens to be an ESP32-S2. This replaces Huidu's
stock firmware with an Arduino sketch.

- Modes:
  - **Clock**: date, time and seconds. Time comes from NTP and is kept in UTC in the on-board BM8563 RTC.
  - **Game of Life**: random soup, or famous patterns (Gosper glider gun with eater, oscillators, spaceships, methuselahs), with adjustable speed
  - **Plasma**
- A web page at `http://ledclock.local` to pick the mode, the Life scene and speed, a scrolling message with its color, and the brightness
- Night dimming between configurable hours
- On-board button: short press for the next mode, hold 2 s for deep sleep, press again to wake
- Firmware updates over WiFi

Pinout from [mrfaptastic/HD-WF1-LED-MatrixPanel-DMA](https://github.com/mrfaptastic/HD-WF1-LED-MatrixPanel-DMA),
drawing with [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA).

## Things learned

- **Only half the rows lit:** 64x64 panels are 1/32 scan and need the E
  address line, which is GPIO 12 on the HD-WF1.
- **Image shifted by one column:** set `clkphase = false`.
- **WiFi "connected" but unreachable:** the S2 has no PSRAM, and an 8-bit
  64x64 framebuffer plus USB CDC left about 5 KB of heap, so WiFi couldn't
  receive anything. `build.sh` uses 4-bit color and no USB CDC, which leaves
  about 70 KB.

## Setup

```sh
arduino-cli core install esp32:esp32   # tested with 3.3.12
arduino-cli lib install "ESP32 HUB75 LED MATRIX PANEL DMA Display" "Adafruit GFX Library"
cp secrets.h.example secrets.h         # WiFi + OTA password
./build.sh                             # output in build/out
```

## First flash (USB, DFU)

1. Connect the board's **USB-A port** (not the micro-USB) to a PC USB-A port
   with an A-to-A cable carrying only GND and D+/D−. USB-C to A cables don't
   work, because the PC sees itself as the device.
2. Short the two pads next to the micro-USB port while powering on, to enter
   download mode. It shows up as `303a:0002`.
3. Package and flash with ESP-IDF's
   [`mkdfu.py`](https://github.com/espressif/esp-idf/blob/master/tools/mkdfu.py)
   and `dfu-util`. DFU was more reliable than esptool over a flaky cable.

```sh
O=build/out
python mkdfu.py write -o dfu.bin --pid 2 --flash-size 4MB \
  0x1000  $O/led_matrix.ino.bootloader.bin \
  0x8000  $O/led_matrix.ino.partitions.bin \
  0xe000  ~/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin \
  0x10000 $O/led_matrix.ino.bin
sudo dfu-util -d 303a:0002 -D dfu.bin
```

Power-cycle without the pads shorted. The IP scrolls across the bottom once it
joins WiFi.

## Updates (WiFi)

```sh
./ota.sh            # or ./ota.sh <ip>
```
