"""
Space Unicorn — ATtiny85 I2C Register Test Tool
MicroPython CLI for verifying the NeoPixel pattern firmware.

Target : ATtiny85 I2C slave at address 0x40
Tested on Pi Pico / Pico W running MicroPython.

Wiring (adjust PIN_SCL / PIN_SDA at top of file):
  ATtiny85 pin 5 (PB0/SDA) --> Pico GP4
  ATtiny85 pin 7 (PB2/SCL) --> Pico GP5
  2.2 kOhm pull-ups to 3.3 V on both lines
  Shared GND
"""

from machine import I2C, Pin
import time

# ── Configuration — adjust to match your wiring ───────────────────────────────
I2C_BUS  = 0        # Pi Pico I2C bus ID (0 or 1)
PIN_SDA  = 4        # GP4  (I2C0 default)
PIN_SCL  = 5        # GP5  (I2C0 default)
I2C_FREQ = 100_000  # 100 kHz standard mode

# ── Register addresses ────────────────────────────────────────────────────────
ADDR        = 0x40

REG_CTRL    = 0x00
REG_GLB_G   = 0x01
REG_GLB_R   = 0x02
REG_GLB_B   = 0x03
REG_PATTERN = 0x04
REG_SPEED   = 0x05
REG_N_LEDS  = 0x06
REG_SEC_G   = 0x07
REG_SEC_R   = 0x08
REG_SEC_B   = 0x09
REG_PARAM1  = 0x0A
REG_PAL_SEL = 0x0B
REG_LED_BASE= 0x0C

# CTRL bit masks
CTRL_RST    = 0x01
CTRL_GLB    = 0x02
CTRL_PAT_EN = 0x04

# Pattern names (REG_PATTERN values)
PATTERNS = {
    0: "OFF",
    1: "SOLID",
    2: "CHASE",
    3: "BLINK",
    4: "ALTERNATE",
    5: "WIPE",
    6: "TWINKLE",
    7: "RAINBOW",
    8: "RAINBOW_MTX",
    9: "RETRO_BLINK",
}

# Palette names (REG_PAL_SEL values)
PALETTES = {
    0: "USER",
    1: "FIRE",
    2: "OCEAN",
    3: "FOREST",
    4: "PARTY",
    5: "MONO_W",
    6: "RAINBOW",
    7: "HEAT",
}

# Pretty names for register dump
REG_NAMES = {
    REG_CTRL:    "CTRL",
    REG_GLB_G:   "GLB_G",
    REG_GLB_R:   "GLB_R",
    REG_GLB_B:   "GLB_B",
    REG_PATTERN: "PATTERN",
    REG_SPEED:   "SPEED",
    REG_N_LEDS:  "N_LEDS",
    REG_SEC_G:   "SEC_G",
    REG_SEC_R:   "SEC_R",
    REG_SEC_B:   "SEC_B",
    REG_PARAM1:  "PARAM1",
    REG_PAL_SEL: "PAL_SEL",
}

# Expected register values after do_reset() (from main.c)
#   init_color = {0x00, 0xFF, 0x00}  => G=0x00 R=0xFF B=0x00 (pure red on WS2812)
RESET_DEFAULTS = {
    REG_CTRL:    0x00,
    REG_GLB_G:   0x00,
    REG_GLB_R:   0xFF,
    REG_GLB_B:   0x00,
    REG_PATTERN: 0x01,  # PAT_SOLID
    REG_SPEED:   0x0A,  # 10
    REG_N_LEDS:  0x40,  # 64
    REG_SEC_G:   0x00,
    REG_SEC_R:   0x00,
    REG_SEC_B:   0x00,
    REG_PARAM1:  0x01,
    REG_PAL_SEL: 0x00,
}

# Predefined palette PROGMEM values from patterns.c
# {GLB_G, GLB_R, GLB_B, SEC_G, SEC_R, SEC_B}
PALETTE_COLOURS = {
    1: (100, 255,   0,   0, 128,   0),  # FIRE
    2: (255,   0, 255,   0,   0,  64),  # OCEAN
    3: (200,  50,   0,  48,  10,   0),  # FOREST
    4: (  0, 255,   0,   0,   0, 255),  # PARTY
    5: (255, 255, 255,  32,  32,  32),  # MONO_W
    6: (  0, 255,   0,   0,   0, 255),  # RAINBOW
    7: (  0, 255,   0, 200, 255,   0),  # HEAT
}

# ── I2C helpers ───────────────────────────────────────────────────────────────

_i2c = None

def _init():
    global _i2c
    _i2c = I2C(I2C_BUS, scl=Pin(PIN_SCL), sda=Pin(PIN_SDA), freq=I2C_FREQ)

def wr(reg, val):
    _i2c.writeto_mem(ADDR, reg, bytes([val & 0xFF]))

def rd(reg):
    return _i2c.readfrom_mem(ADDR, reg, 1)[0]

def wr_block(reg, data):
    _i2c.writeto_mem(ADDR, reg, bytes(data))

def rd_block(reg, n):
    return list(_i2c.readfrom_mem(ADDR, reg, n))

def device_reset():
    wr(REG_CTRL, CTRL_RST)
    time.sleep_ms(50)

# ── Test framework ────────────────────────────────────────────────────────────

_pass = 0
_fail = 0

def _reset_counts():
    global _pass, _fail
    _pass = 0
    _fail = 0

def _ok(msg):
    global _pass
    _pass += 1
    print("  [PASS] " + msg)

def _fail_msg(msg):
    global _fail
    _fail += 1
    print("  [FAIL] " + msg)

def _check(label, got, expected):
    if got == expected:
        _ok("{}: 0x{:02X}".format(label, got))
    else:
        _fail_msg("{}: expected 0x{:02X}, got 0x{:02X}".format(label, expected, got))

# ── Automated tests ───────────────────────────────────────────────────────────

def test_scan():
    print("\n--- I2C Bus Scan ---")
    found = _i2c.scan()
    if not found:
        print("  No devices found on bus.")
        return False
    print("  Devices: " + str([hex(d) for d in found]))
    if ADDR in found:
        _ok("Device present at 0x{:02X}".format(ADDR))
        return True
    _fail_msg("Device NOT found at 0x{:02X}".format(ADDR))
    return False

def test_reset():
    print("\n--- Reset Test ---")
    device_reset()
    for reg, expected in RESET_DEFAULTS.items():
        name = REG_NAMES.get(reg, "0x{:02X}".format(reg))
        _check(name, rd(reg), expected)

def test_rw_registers():
    print("\n--- Config Register Read/Write ---")
    cases = [
        (REG_GLB_G,   0xAA, "GLB_G"),
        (REG_GLB_R,   0xBB, "GLB_R"),
        (REG_GLB_B,   0xCC, "GLB_B"),
        (REG_PATTERN, 0x07, "PATTERN"),
        (REG_SPEED,   0x20, "SPEED"),
        (REG_N_LEDS,  0x10, "N_LEDS"),
        (REG_SEC_G,   0x11, "SEC_G"),
        (REG_SEC_R,   0x22, "SEC_R"),
        (REG_SEC_B,   0x33, "SEC_B"),
        (REG_PARAM1,  0x04, "PARAM1"),
        (REG_PAL_SEL, 0x00, "PAL_SEL"),
    ]
    for reg, val, name in cases:
        wr(reg, val)
        _check(name, rd(reg), val)

def test_led_array():
    print("\n--- LED Array Read/Write ---")
    leds = [
        (0xFF, 0x00, 0x00),  # LED 0 : green
        (0x00, 0xFF, 0x00),  # LED 1 : red
        (0x00, 0x00, 0xFF),  # LED 2 : blue
    ]
    for i, (g, r, b) in enumerate(leds):
        wr_block(REG_LED_BASE + i * 3, [g, r, b])
    for i, (g, r, b) in enumerate(leds):
        d = rd_block(REG_LED_BASE + i * 3, 3)
        _check("LED[{}] G".format(i), d[0], g)
        _check("LED[{}] R".format(i), d[1], r)
        _check("LED[{}] B".format(i), d[2], b)

def test_led_block_write():
    print("\n--- LED Block Write (multi-byte) ---")
    # Write 4 LEDs in a single transaction
    data = [0x10, 0x20, 0x30,
            0x40, 0x50, 0x60,
            0x70, 0x80, 0x90,
            0xA0, 0xB0, 0xC0]
    wr_block(REG_LED_BASE, data)
    back = rd_block(REG_LED_BASE, 12)
    _check("Block write readback", back, data)

def test_ctrl_bits():
    print("\n--- CTRL Bit Fields ---")
    for bit, name in [(CTRL_GLB, "GLB"), (CTRL_PAT_EN, "PAT_EN")]:
        wr(REG_CTRL, bit)
        _check("{} set".format(name),   rd(REG_CTRL) & bit, bit)
        wr(REG_CTRL, 0x00)
        _check("{} clear".format(name), rd(REG_CTRL) & bit, 0x00)

def test_palette_load():
    print("\n--- Predefined Palette Load ---")
    for pal_id, (eg, er, eb, sg, sr, sb) in PALETTE_COLOURS.items():
        name = PALETTES[pal_id]
        wr(REG_PAL_SEL, pal_id)
        time.sleep_ms(10)   # allow main loop to process stop condition
        _check("{} GLB_G".format(name), rd(REG_GLB_G), eg)
        _check("{} GLB_R".format(name), rd(REG_GLB_R), er)
        _check("{} GLB_B".format(name), rd(REG_GLB_B), eb)
        _check("{} SEC_G".format(name), rd(REG_SEC_G), sg)
        _check("{} SEC_R".format(name), rd(REG_SEC_R), sr)
        _check("{} SEC_B".format(name), rd(REG_SEC_B), sb)

def test_pattern_select():
    print("\n--- Pattern Register ---")
    for pat_id in PATTERNS:
        wr(REG_PATTERN, pat_id)
        _check(PATTERNS[pat_id], rd(REG_PATTERN), pat_id)

def test_speed_boundary():
    print("\n--- Speed Boundary Values ---")
    for val in [1, 128, 255]:
        wr(REG_SPEED, val)
        _check("SPEED={}".format(val), rd(REG_SPEED), val)

def test_all():
    _reset_counts()
    print("\n" + "=" * 42)
    print(" Running all tests")
    print("=" * 42)

    if not test_scan():
        print("\nDevice not found — aborting test run.")
        return

    test_reset()
    test_rw_registers()
    test_led_array()
    test_led_block_write()
    test_ctrl_bits()
    test_pattern_select()
    test_speed_boundary()
    test_palette_load()
    test_reset()          # leave device in clean state

    print("\n" + "=" * 42)
    print(" PASSED: {}   FAILED: {}".format(_pass, _fail))
    print("=" * 42)

# ── Interactive commands ───────────────────────────────────────────────────────

def dump_registers():
    print("\n  Addr  Register     Value  Detail")
    print("  ----  -----------  -----  ------")
    for reg in range(REG_PAL_SEL + 1):
        name = REG_NAMES.get(reg, "?")
        val  = rd(reg)
        detail = ""
        if reg == REG_CTRL:
            parts = []
            if val & CTRL_RST:    parts.append("RST")
            if val & CTRL_GLB:    parts.append("GLB")
            if val & CTRL_PAT_EN: parts.append("PAT_EN")
            detail = " | ".join(parts) if parts else "-"
        elif reg == REG_PATTERN:
            detail = PATTERNS.get(val, "?")
        elif reg == REG_PAL_SEL:
            detail = PALETTES.get(val, "?")
        print("  0x{:02X}  {:<12} 0x{:02X}   {}".format(reg, name, val, detail))

    print("\n  LEDs (first 4):")
    for i in range(4):
        d = rd_block(REG_LED_BASE + i * 3, 3)
        print("    LED[{}]  G=0x{:02X}  R=0x{:02X}  B=0x{:02X}".format(i, d[0], d[1], d[2]))

def _prompt_int(prompt, lo, hi):
    try:
        v = int(input(prompt).strip())
        if lo <= v <= hi:
            return v
        print("  Out of range ({}-{}).".format(lo, hi))
    except ValueError:
        print("  Enter an integer.")
    return None

def cmd_set_pattern():
    print("  Patterns:")
    for k, v in PATTERNS.items():
        print("    {:2d}  {}".format(k, v))
    v = _prompt_int("  Pattern ID: ", 0, 9)
    if v is not None:
        wr(REG_PATTERN, v)
        wr(REG_CTRL, rd(REG_CTRL) | CTRL_PAT_EN)
        print("  Pattern set to {} (PAT_EN enabled)".format(PATTERNS[v]))

def cmd_set_palette():
    print("  Palettes:")
    for k, v in PALETTES.items():
        print("    {:2d}  {}".format(k, v))
    v = _prompt_int("  Palette ID: ", 0, 7)
    if v is not None:
        wr(REG_PAL_SEL, v)
        time.sleep_ms(10)
        print("  Palette loaded: {}".format(PALETTES[v]))

def cmd_set_colour(label, g_reg, r_reg, b_reg):
    print("  Set {} colour (each 0-255, WS2812 is G R B order)".format(label))
    g = _prompt_int("  G: ", 0, 255)
    r = _prompt_int("  R: ", 0, 255)
    b = _prompt_int("  B: ", 0, 255)
    if None not in (g, r, b):
        wr(g_reg, g); wr(r_reg, r); wr(b_reg, b)
        print("  {} = G:{} R:{} B:{}".format(label, g, r, b))

def cmd_set_speed():
    v = _prompt_int("  Speed (1=fastest, 255=slowest): ", 1, 255)
    if v is not None:
        wr(REG_SPEED, v)
        print("  Speed = {}".format(v))

def cmd_set_nleds():
    v = _prompt_int("  N_LEDS (1-64): ", 1, 64)
    if v is not None:
        wr(REG_N_LEDS, v)
        print("  N_LEDS = {}".format(v))

def cmd_set_param1():
    v = _prompt_int("  PARAM1 (1-255): ", 1, 255)
    if v is not None:
        wr(REG_PARAM1, v)
        print("  PARAM1 = {}".format(v))

def cmd_set_led():
    i = _prompt_int("  LED index (0-63): ", 0, 63)
    if i is None:
        return
    g = _prompt_int("  G (0-255): ", 0, 255)
    r = _prompt_int("  R (0-255): ", 0, 255)
    b = _prompt_int("  B (0-255): ", 0, 255)
    if None not in (g, r, b):
        wr_block(REG_LED_BASE + i * 3, [g, r, b])
        print("  LED[{}] = G:{} R:{} B:{}".format(i, g, r, b))

def cmd_toggle_pat_en():
    ctrl = rd(REG_CTRL)
    if ctrl & CTRL_PAT_EN:
        wr(REG_CTRL, ctrl & ~CTRL_PAT_EN)
        print("  Pattern engine OFF")
    else:
        wr(REG_CTRL, ctrl | CTRL_PAT_EN)
        print("  Pattern engine ON")

def cmd_toggle_glb():
    ctrl = rd(REG_CTRL)
    if ctrl & CTRL_GLB:
        wr(REG_CTRL, ctrl & ~CTRL_GLB)
        print("  Global colour mode OFF")
    else:
        wr(REG_CTRL, ctrl | CTRL_GLB)
        print("  Global colour mode ON")

def cmd_reset():
    device_reset()
    print("  Device reset complete.")

# ── Menu ──────────────────────────────────────────────────────────────────────

MENU = """
+--------------------------------------+
|  Space Unicorn I2C Register Tester  |
+--------------------------------------+
 Tests
   s  Scan I2C bus
   d  Dump all registers
   r  Run all tests

 Live control
   p  Set pattern
   P  Set palette
   c  Set primary colour  (GLB G/R/B)
   C  Set secondary colour (SEC G/R/B)
   l  Set individual LED
   v  Set speed
   n  Set N_LEDS
   a  Set PARAM1
   e  Toggle PAT_EN (pattern engine)
   g  Toggle GLB (global colour mode)

 Device
   x  Reset device (CTRL_RST)
   q  Quit
"""

def main():
    print("\nSpace Unicorn I2C Register Test Tool")
    print("Initialising I2C{} SCL=GP{} SDA=GP{} {}kHz ...".format(
        I2C_BUS, PIN_SCL, PIN_SDA, I2C_FREQ // 1000))
    _init()
    print("Ready. Target 0x{:02X}".format(ADDR))

    while True:
        print(MENU)
        try:
            cmd = input("Command: ").strip()
        except (EOFError, KeyboardInterrupt):
            break

        if not cmd:
            continue

        try:
            if   cmd == 'q': break
            elif cmd == 's': test_scan()
            elif cmd == 'd': dump_registers()
            elif cmd == 'r': test_all()
            elif cmd == 'p': cmd_set_pattern()
            elif cmd == 'P': cmd_set_palette()
            elif cmd == 'c': cmd_set_colour("Primary",   REG_GLB_G, REG_GLB_R, REG_GLB_B)
            elif cmd == 'C': cmd_set_colour("Secondary", REG_SEC_G, REG_SEC_R, REG_SEC_B)
            elif cmd == 'l': cmd_set_led()
            elif cmd == 'v': cmd_set_speed()
            elif cmd == 'n': cmd_set_nleds()
            elif cmd == 'a': cmd_set_param1()
            elif cmd == 'e': cmd_toggle_pat_en()
            elif cmd == 'g': cmd_toggle_glb()
            elif cmd == 'x': cmd_reset()
            else: print("  Unknown command '{}'. Type q to quit.".format(cmd))
        except OSError as exc:
            print("  I2C error: {}".format(exc))
            print("  Check wiring and that device is powered.")

    print("Bye.")

main()
