#!/usr/bin/env bash
# Build the Space Unicorn firmware without GNU make, using the Arduino-bundled
# AVR toolchain. Produces main.hex (flash) and main.eep (EEPROM, usually empty).
#
#   ./build.sh            # compile + link + hex, print size
#   ./build.sh flash      # ... then flash via the AVRISP mkII
#   ./build.sh fuses      # write 8 MHz fuses (virgin chips only)
set -e

TOOLS="$HOME/AppData/Local/Arduino15/packages/arduino/tools"
AVRGCC="$(ls -d "$TOOLS"/avr-gcc/*/bin | head -1)"
AVRDUDE_BIN="$(ls -d "$TOOLS"/avrdude/*/bin | head -1)"
AVRDUDE_ETC="$(ls -d "$TOOLS"/avrdude/*/etc | head -1)"
export PATH="$AVRGCC:$AVRDUDE_BIN:$PATH"

MCU=attiny85
TARGET=main
SRC="main.c patterns.c i2c/i2c_machine.c ws2812/light_ws2812_AVR/Light_WS2812/light_ws2812.c"

CFLAGS="-mmcu=$MCU -I. -Iws2812/light_ws2812_AVR -DF_CPU=8000000 -Os \
-funsigned-char -funsigned-bitfields -fpack-struct -fshort-enums \
-Wall -Wstrict-prototypes -std=gnu99 -ffunction-sections -fdata-sections"

echo "== compiling =="
avr-gcc $CFLAGS $SRC -o $TARGET.elf -Wl,-Map=$TARGET.map,--cref -Wl,--gc-sections -lm

echo "== hex =="
avr-objcopy -O ihex -R .eeprom $TARGET.elf $TARGET.hex
avr-objcopy -j .eeprom --set-section-flags=.eeprom="alloc,load" \
    --change-section-lma .eeprom=0 -O ihex $TARGET.elf $TARGET.eep || true

echo "== size =="
avr-size -C --mcu=$MCU $TARGET.elf

# AVRISP mkII: avrdude programmer id "avrispmkII", USB
AVRDUDE_FLAGS="-C $AVRDUDE_ETC/avrdude.conf -p $MCU -c avrispmkII -P usb"

case "$1" in
  flash)
    echo "== flashing =="
    avrdude $AVRDUDE_FLAGS -U flash:w:$TARGET.hex
    ;;
  fuses)
    echo "== writing fuses (8 MHz internal) =="
    avrdude $AVRDUDE_FLAGS -U lfuse:w:0xe2:m -U hfuse:w:0xdf:m -U efuse:w:0xff:m
    ;;
esac
