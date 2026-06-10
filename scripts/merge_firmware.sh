#!/bin/bash

set -e

if [ $# -eq 2 ]; then
    BUILD_DIR=$1
    OUTPUT_NAME=$2
else
    echo "Usage: $0 [build_directory] [output_name]"
    echo "  build_directory: relative path to directory containing bootloader.bin, partitions.bin, firmware.bin"
    echo "  output_name: name for output file"
    echo ""
    echo "Example:"
    echo "  $0 .pio/build/trmnl my-image.bin"
    exit 1
fi

# Check if build files exist
if [ ! -f "$BUILD_DIR/bootloader.bin" ]; then
    echo "Error: bootloader.bin not found. Run 'pio run -e trmnl' first."
    exit 1
fi

if [ ! -f "$BUILD_DIR/partitions.bin" ]; then
    echo "Error: partitions.bin not found. Run 'pio run -e trmnl' first."
    exit 1
fi

if [ ! -f "$BUILD_DIR/firmware.bin" ]; then
    echo "Error: firmware.bin not found. Run 'pio run -e trmnl' first."
    exit 1
fi

ESPTOOL="pio pkg exec -p tool-esptoolpy esptool.py -- "

# Chip + flash params. Defaults target esp32c3 (original TRMNL); override CHIP for S3 boards
# (OG DIY Kit / reTerminal E1001), e.g. CHIP=esp32s3 ./scripts/merge_firmware.sh ...
CHIP="${CHIP:-esp32c3}"
case "$CHIP" in
    esp32s3) FLASH_MODE="${FLASH_MODE:-dio}"; FLASH_FREQ="${FLASH_FREQ:-80m}"; FLASH_SIZE="${FLASH_SIZE:-8MB}" ;;
    *)       FLASH_MODE="${FLASH_MODE:-dio}"; FLASH_FREQ="${FLASH_FREQ:-40m}"; FLASH_SIZE="${FLASH_SIZE:-4MB}" ;;
esac

# Include boot_app0.bin (otadata -> boot slot 0) at 0xe000 when available, for reliable first boot
BOOT_APP0="builds/bin/boot_app0.bin"
BOOT_APP0_SEG=""
[ -f "$BOOT_APP0" ] && BOOT_APP0_SEG="0xe000 $BOOT_APP0"

echo "Using esptool: $ESPTOOL (chip=$CHIP mode=$FLASH_MODE freq=$FLASH_FREQ size=$FLASH_SIZE)"

$ESPTOOL --chip "$CHIP" merge_bin \
    -o "$OUTPUT_NAME" \
    --flash_mode "$FLASH_MODE" \
    --flash_freq "$FLASH_FREQ" \
    --flash_size "$FLASH_SIZE" \
    0x0000 "$BUILD_DIR/bootloader.bin" \
    0x8000 "$BUILD_DIR/partitions.bin" \
    $BOOT_APP0_SEG \
    0x10000 "$BUILD_DIR/firmware.bin"

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Merged firmware created successfully!"
    echo "📁 Location: $OUTPUT_NAME"
    echo ""
    echo "To flash this merged firmware:"
    echo "./scripts/flash_merged.sh $OUTPUT_NAME"
    echo ""
else
    echo "❌ Failed to create merged firmware"
    exit 1
fi
