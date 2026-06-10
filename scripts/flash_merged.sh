#!/bin/bash

set -e

if [ $# -eq 0 ]; then
    echo "Usage: $0 <merged_firmware.bin>"
    echo "  merged_firmware.bin: path to merged firmware file"
    echo ""
    echo "Example:"
    echo "  $0 my-image.bin"
    exit 1
fi

FIRMWARE_FILE="$1"
CHIP="${CHIP:-esp32c3}"   # override for S3 boards, e.g. CHIP=esp32s3 ./scripts/flash_merged.sh ...
BAUD_RATE=$((115200 * 4))
ESPTOOL="pio pkg exec -p tool-esptoolpy esptool.py -- "
ESPTOOL_CMD="$ESPTOOL --chip $CHIP --baud $BAUD_RATE"

if [ ! -f "$FIRMWARE_FILE" ]; then
    echo "Error: Firmware file not found: $FIRMWARE_FILE"
    exit 1
fi

echo ""
echo "⚡ Flashing firmware..."

$ESPTOOL_CMD write_flash 0x0000 "$FIRMWARE_FILE"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Flash completed successfully!"
    echo "🔄 Device will reboot automatically"
    echo ""
else
    echo ""
    echo "❌ Flash failed!"
    echo ""
    exit 1
fi
