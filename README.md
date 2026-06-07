# Space Unicorn — NeoPixel Pattern Firmware

ATtiny85 I2C slave firmware for driving WS2812 NeoPixel LEDs with animated
patterns, predefined colour palettes, and a user button for live pattern
cycling. Built for the EMF26 Space Unicorn board.

Based on [neopixel_i2c](https://github.com/usedbytes/neopixel_i2c) by
**Brian Starkey (usedbytes)**, with the WinAVR Makefile originally written by
**Eric B. Weddington, Jörg Wunsch et al.** and modified by **Elliot Williams**.

---

## Hardware

ATtiny85 pin assignments:

| Pin | Port     | Function                                      |
|-----|----------|-----------------------------------------------|
| 1   | PB5/RESET| Reset button                                  |
| 2   | PB3      | NeoPixel data output                          |
| 3   | PB4      | User button (active-low, internal pull-up)    |
| 4   | GND      |                                               |
| 5   | PB0      | I2C SDA                                       |
| 6   | PB1      | Heartbeat LED (1 Hz blink; flashes on I2C data)|
| 7   | PB2      | I2C SCL                                       |
| 8   | VCC      |                                               |

### Circuit

```
                        ^ VCC
                        |
    +--------+----------+----------+-----+-------+
    |        |                     |     |       |
    |       |"| 10k                |     |       |
    |       |_|                   |"| 2k2 |"| 2k2
    |        |   +--ATtiny85--+   |_|   |_|
    |        +---| Reset  Vcc |----+     |
    |            |            |          |
  _____ 0.1u     |        SCL |----------+-------<> SCL
  _____  ,-------| GND    SDA |------------------<> SDA
    |    |       +------------+
    |    v GND
    |
    +---------------------------------------------------> NeoPixel Data (PB3)
    |
    +--[ button ]-- GND                                   User button (PB4)
    |
    +--[ LED ]--[ 330R ]-- GND                            Heartbeat LED (PB1)
```

Pull-ups: 2.2 kΩ on SCL and SDA to VCC. 10 kΩ on RESET to VCC.

---

## Building

Requires `avr-gcc`, `avr-libc`, and `avrdude`.

```bash
sudo apt install gcc-avr avr-libc binutils-avr avrdude

git clone --recurse-submodules https://github.com/your-repo/space-unicorn-firmware
cd space-unicorn-firmware

make fuses  # MUST be run once on any new or replacement chip
make        # build and flash
```

The default programmer is **Atmel-ICE (ISP)**. To use a different programmer
edit the top of `Makefile`:

```makefile
AVRDUDE_PROGRAMMER = usbasp     # or linuxspi, avrisp2, usbtiny, ...
AVRDUDE_PORT       = usb
```

A pre-built `main.hex` is included in the repository if you only need to flash.

### Fuses — required on every fresh chip

> **Important:** ATtiny85 chips ship from the factory with `CKDIV8` programmed,
> running the CPU at 1 MHz. The WS2812 driver requires exactly 8 MHz. With the
> wrong clock the NeoPixels will be **stuck solid white** and the button will be
> unresponsive.
>
> Run `make fuses` once whenever fitting a new or replacement chip. This sets
> `LFUSE=0xE2` (8 MHz internal RC, no divider) and is safe to re-run on a
> chip that is already correctly programmed.

---

## Flash and RAM usage

| Section | Used    | Available |
|---------|---------|-----------|
| Flash   | 2890 B  | 8192 B    |
| RAM     | 225 B   | 512 B     |

---

## I2C Protocol

**Slave address: 0x40** (hardcoded in `i2c/i2c_slave_defs.h`)

Write transaction:

| Start | Addr (0x40) | Register | Data bytes… | Stop |

Read transaction:

| Start | Addr (0x40) | Register | Restart | Addr (0x41) | Data bytes… | Stop |

The register address auto-increments after each byte, so multi-byte reads and
writes are supported in a single transaction.

**LED values are updated only after a STOP condition is received.**

---

## Register Map

| Address    | Name        | Description                              | Reset |
|------------|-------------|------------------------------------------|-------|
| 0x00       | **CTRL**    | Control register                         | 0x00  |
| 0x01       | **GLB_G**   | Primary colour — green                   | 0x00  |
| 0x02       | **GLB_R**   | Primary colour — red                     | 0xFF  |
| 0x03       | **GLB_B**   | Primary colour — blue                    | 0x00  |
| 0x04       | **PATTERN** | Pattern ID (0–9, see table below)        | 0x01  |
| 0x05       | **SPEED**   | Ticks per animation frame (1=fast, 255=slow) | 0x0A |
| 0x06       | **N_LEDS**  | Active LED count (max 64)                | 0x40  |
| 0x07       | **SEC_G**   | Secondary colour — green                 | 0x00  |
| 0x08       | **SEC_R**   | Secondary colour — red                   | 0x00  |
| 0x09       | **SEC_B**   | Secondary colour — blue                  | 0x00  |
| 0x0A       | **PARAM1**  | Pattern-specific parameter               | 0x01  |
| 0x0B       | **PAL_SEL** | Palette selector (0=user, 1–7=predefined)| 0x00  |
| 0x0C       | **GREEN[0]**| LED 0 green                              | 0x00  |
| 0x0D       | **RED[0]**  | LED 0 red                                | 0x00  |
| 0x0E       | **BLUE[0]** | LED 0 blue                               | 0x00  |
| …          | …           | 3 bytes per LED (G, R, B)                | 0x00  |
| 0x0C+n×3   | **GREEN[n]**| LED n green                              | 0x00  |

> WS2812 byte order on the wire is **G, R, B**.

---

## CTRL Register (0x00)

| Bit | Name       | Description |
|-----|------------|-------------|
| 0   | **RST**    | Write 1 to reset all LEDs and registers to defaults. Auto-clears. |
| 1   | **GLB**    | Write 1 to display the primary colour (GLB_G/R/B) on all LEDs. |
| 2   | **PAT_EN** | Write 1 to enable the pattern animation engine. |
| 7–3 | —          | Reserved, read as 0. |

---

## Patterns (REG_PATTERN, 0x04)

| ID | Name            | Description                                                   | Uses PARAM1 |
|----|-----------------|---------------------------------------------------------------|-------------|
| 0  | OFF             | All LEDs off                                                  | —           |
| 1  | SOLID           | All LEDs show primary colour                                  | —           |
| 2  | CHASE           | Scrolling comet; tail dims by ½ each step                    | —           |
| 3  | BLINK           | All LEDs toggle between primary and off                       | —           |
| 4  | ALTERNATE       | Even LEDs = primary, odd = secondary; swaps each frame        | —           |
| 5  | WIPE            | Fill left→right one LED per frame, then clear                 | —           |
| 6  | TWINKLE         | Random LEDs flash; others fade by ½ each frame               | —           |
| 7  | RAINBOW         | Full hue wheel cycling across the strip                       | —           |
| 8  | RAINBOW_MTX     | Diagonal rainbow on 8×8 grid, rotates over time              | bit 0 = serpentine wiring |
| 9  | RETRO_BLINK     | Random LEDs toggle independently (50s computer effect)        | LEDs toggled per frame (1–8) |

Enable a pattern by writing PAT_EN to CTRL and the ID to PATTERN. The user
button (PB4) also cycles through patterns 1–9 with a single press.

---

## Predefined Palettes (REG_PAL_SEL, 0x0B)

Writing a palette ID loads predefined primary and secondary colours from flash
(PROGMEM) into GLB and SEC registers. Writing 0 keeps the current user colours.

| ID | Name    | Primary (G R B)   | Secondary (G R B) |
|----|---------|-------------------|-------------------|
| 0  | USER    | *(unchanged)*     | *(unchanged)*     |
| 1  | FIRE    | 100, 255, 0       | 0, 128, 0         |
| 2  | OCEAN   | 255, 0, 255       | 0, 0, 64          |
| 3  | FOREST  | 200, 50, 0        | 48, 10, 0         |
| 4  | PARTY   | 0, 255, 0         | 0, 0, 255         |
| 5  | MONO_W  | 255, 255, 255     | 32, 32, 32        |
| 6  | RAINBOW | 0, 255, 0         | 0, 0, 255         |
| 7  | HEAT    | 0, 255, 0         | 200, 255, 0       |

---

## Heartbeat LED (PB1)

The LED on PB1 serves two purposes:

- **Heartbeat**: 1 Hz blink while the firmware is running, driven by Timer0 ISR.
- **Data activity**: overrides the heartbeat with a ~200 ms solid-on flash each
  time an I2C STOP condition is received.

---

## Maximum LED Count

With the extended register map (12 global registers) and the pattern engine's
8-byte `blink_map` for RETRO_BLINK, the practical RAM limit is approximately
**125–130 LEDs**. The compile-time default is **64** to support an 8×8 matrix.
To change it, edit `N_LEDS` in `i2c/i2c_slave_defs.h`.

---

## Testing

`test_registers.py` is a MicroPython / Raspberry Pi dual-platform test tool
that verifies all registers over I2C.

**Raspberry Pi (Linux):**
```bash
sudo apt install python3-smbus2
python3 test_registers.py
```

**Pi Pico (MicroPython):**
```
# Copy test_registers.py to the Pico, then run from the REPL or Thonny.
# Edit MP_PIN_SDA / MP_PIN_SCL at the top of the file to match your wiring.
mpremote run test_registers.py
```

The tool provides an interactive menu for running automated register tests and
live control of patterns, palettes, colours, and individual LEDs.

---

## Acknowledgements

This firmware extends
[neopixel_i2c](https://github.com/usedbytes/neopixel_i2c) by
**Brian Starkey (usedbytes)**, which provides the USI I2C slave implementation
and the original WS2812 driving code. The WinAVR Makefile was written by
**Eric B. Weddington, Jörg Wunsch et al.** and modified by **Elliot Williams**.
The [light_ws2812](https://github.com/cpldcpu/light_ws2812) library by
**cpldcpu** is used for WS2812 output.

---

---

# Original README — neopixel_i2c (picopixel)

> The following is the original README from the upstream neopixel_i2c project
> by Brian Starkey (usedbytes), preserved for reference.

This is an AVR-based neopixel driver. It accepts commands over i2c to drive a
a number of ws2812 LED pixels.

The code on the master branch is set up for an Attiny85, with the LEDs on PB3,
using an usbasp programmer. The fuses should be set to use 8 MHz internal
oscillator, **no divider**

## How many LEDs?
The number of LEDs is currently hardcoded in the firmware. The maximum number
depends on the amount of RAM available on your AVR.
An Attiny45 should be able to drive 82 LEDs, and an Attiny85, 167.

## Circuit

Suggested circuit.

```
                            ^ VCC
                            |
    +--------+--------------+-------------+-----+-------+
    |        |                            |     |       |
    |       |"| 10k                       |     |       |
    |       |_|                           |     |       |
    |        |     +Attinyx5--------+     |    |"| 2k2 |"| 2k2
    |        +-----| Reset      Vcc |-----'    |_|     |_|
    |              |                |           |       |
  _____ 0.1u       |                |           |       |
  _____  ,---------|            SCL |-----------+-------|-----<> SCL
    |    |         |                |                   |
    |    |         |                |                   |
    |    |       --|                |--                 |
    |    |         |                |                   |
    |    |         |                |                   |
    +----|---+-----| GND        SDA |-------------------+-----<> SDA
         |   |     +----------------+
         |   v GND
         |
         '-----------------------------------------------------> NeoPixel Data

```

## Getting the code

This project uses git submodules (I kinda wish it didn't...). You can clone
it like so:

```git clone --recursive https://github.com/usedbytes/neopixel_i2c```

If your git version is too old to support that, do this instead:

```
git clone --recursive https://github.com/usedbytes/neopixel_i2c
cd neopixel_i2c
git submodule update --init --recursive
```


## Basic Functionality

There are two basic operating modes:
 * In *normal* mode, each LED is individually driven based on the value in its
   control register.
 * In *global* mode, all LEDs are driven to the same value, based on the values
   in the global value registers.

The operating modes are selected by flipping the *GLB* bit in the **CTRL**
register.

## i2c Protocol

**The slave address is currently hardcoded to 0x40 in the firmware**, see

```
i2c/i2c_slave_defs.h:30
```


This utilises my i2c slave library (https://github.com/usedbytes/usi_i2c_slave)
which means it follows a fairly standard i2c protocol (for more, see here:
http://www.robot-electronics.co.uk/i2c-tutorial).

Writes look like this:

| Start | Slave Address << 1 | Register Address | Data | Stop |
|-------|--------------------|------------------|------|------|

The register address will auto-increment after every byte, so you can write
data in bursts.

Reads look like this:

| Start | Slave Address | Register Address | Restart | (Slave Address << 1) + 1  | Data | Stop |
|-------|---------------|------------------|---------|---------------------------|------|------|

First you do a write transaction to set the register address to read from, then
a read transaction to read the data. When you've read all the data you want,
send a NAK after the last byte to terminate the read.

**The LED values are only updated after a STOP is received**

## Register Map
The register map consists of a number of global control registers - address
0x0-0x3 - followed by an array of registers which hold the individual value
for each LED in normal mode.

| **Address** | **Name**     | **Description**      | **Access** | **Reset** |
|------------:|--------------|:---------------------|--------|------:|
|   0x00      | **CTRL**     | Control Register     | R/W    |    0  |
|   0x01      | **GLB_G**    | Global Green Value   | R/W    |    0  |
|   0x02      | **GLB_R**    | Global Red Value     | R/W    |    0  |
|   0x03      | **GLB_B**    | Global Blue Value    | R/W    |    0  |
|Array follows|--------------|----------------------|--------|-------|
|   0x04      | **GREEN[0]** | Green value, LED0    | R/W    |    0  |
|   0x05      | **RED[0]**   | Red value, LED0      | R/W    |    0  |
|   0x06      | **BLUE[0]**  | Blue value, LED0     | R/W    |    0  |
|   ....      | ....         | ....                 | ....   | ....  |
| (3*n) + 4   | **GREEN[n]** | Green value, LEDn    | R/W    |    0  |
| (3*n) + 5   | **RED[n]**   | Red value, LEDn      | R/W    |    0  |
| (3*n) + 6   | **BLUE[n]**  | Blue value, LEDn     | R/W    |    0  |

## Register Descriptions

### **CTRL**
The control register sets the operating mode.

|   Name: | RSVD | RSVD | RSVD | RSVD | RSVD | RSVD | GLB  | RST  |
|--------:|:----:|:----:|:----:|:----:|:----:|:----:|:----:|:----:|
|    Bit: |    7 |    6 |    5 |    4 |    3 |    2 |    1 |    0 |
| Access: |    r |    r |   r  |   r  |   r  |   r  |  rw  |  rw  |

#### *RST*
Writing a 1 to this bit will reset the LED controler, setting all LEDs to OFF

This bit will be automatically cleared once the reset has completed.

#### *GLB*
Writing a one to this bit causes the global color value to be displayed on all
LEDs at the end of the transaction - Normally you would set the **GLB_R**,
**GLB_G**, and **GLB_B** values in the same transaction as setting the *GLB*
bit so that the new colour is immediately applied.

Writing a zero to this bit will disable the global colour override and return to
normal operation.

### **GLB_R**, **GLB_G**, **GLB_B**
These registers hold the global colour value. When the *GLB* bit in the
**CTRL** register is set, all LEDs will display this colour.

### **LED Value Array**
Everything after the global registers is an array of data for each LED.
When the *GLB* bit is not set, each LED will display whatever value is
programmed in its corresponding register set.
