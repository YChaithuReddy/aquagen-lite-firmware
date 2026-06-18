#!/usr/bin/env bash
# Flash ONE AquaGen board end-to-end: firmware + that box's baked identity (NVS).
#
# Run from the project root in an ESP-IDF-activated terminal (run `espidf` first so idf.py is on PATH).
# Usage:  ./tools/flash_board.sh <serial-port> <device-number>
# Example: ./tools/flash_board.sh /dev/cu.usbserial-0001 07
#          → flashes firmware, then bakes Gravity_water_07's identity at NVS 0x9000.
set -e

PORT="$1"; NUM="$2"
if [ -z "$PORT" ] || [ -z "$NUM" ]; then
  echo "Usage: $0 <serial-port> <NN>   e.g. $0 /dev/cu.usbserial-XXXX 07"
  echo "Tip: find the port with  ls /dev/cu.*  (board plugged in)"
  exit 1
fi

DEV="Gravity_water_${NUM}"
NVS="batch_gravity_50/nvs/${DEV}.bin"
if [ ! -f "$NVS" ]; then
  echo "❌ No identity file for ${DEV} at ${NVS}"
  echo "   (Did you run the batch generator? Check batch_gravity_50/nvs/)"
  exit 1
fi
if ! command -v idf.py >/dev/null 2>&1; then
  echo "❌ idf.py not found — run 'espidf' first to activate ESP-IDF, then re-run."
  exit 1
fi

echo "==> [1/2] Flashing firmware to ${PORT} (115200, reliable) ..."
idf.py -p "$PORT" -b 115200 flash

echo "==> [2/2] Baking identity ${DEV} (NVS @ 0x9000) ..."
esptool.py -p "$PORT" -b 115200 write_flash 0x9000 "$NVS"

echo ""
echo "✅ ${DEV} flashed + provisioned."
echo "   Watch the boot serial for the meter self-test (looking for ✅ PASS) ..."
echo "   (Make sure the meter is wired to the box's RS485 + powered.)"
echo "   Press Ctrl+] to exit the monitor when you see the result, then label + pack the box."
echo ""
idf.py -p "$PORT" monitor
