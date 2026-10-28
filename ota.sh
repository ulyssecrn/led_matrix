#!/bin/sh
# Build and upload the firmware over WiFi. Usage: ./ota.sh [host]  (default ledclock.local)
set -e
cd "$(dirname "$0")"
HOST=${1:-ledclock.local}
PASS=$(sed -n 's/^#define OTA_PASSWORD *"\(.*\)"/\1/p' secrets.h)
./build.sh
curl -fsS -m 180 -u "admin:$PASS" -F "firmware=@build/out/led_matrix.ino.bin" "http://$HOST/update"
