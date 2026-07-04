"""
Space Unicorn — ATtiny85 I2C Register Test Tool

Runs on MicroPython (Pi Pico / Pico W) and CPython on Linux (Raspberry Pi).
Platform is detected automatically at import time.

MicroPython wiring (adjust MP_PIN_SDA / MP_PIN_SCL):
  ATtiny85 PB0 (pin 5, SDA) --> Pico GP4
  ATtiny85 PB2 (pin 7, SCL) --> Pico GP5
  2.2 kOhm pull-ups to 3.3 V on both lines, shared GND.

Raspberry Pi wiring (I2C1, /dev/i2c-1):
  ATtiny85 PB0 (pin 5, SDA) --> RPi GPIO 2 (pin 3)
  ATtiny85 PB2 (pin 7, SCL) --> RPi GPIO 3 (pin 5)
  2.2 kOhm pull-ups to 3.3 V on both lines, shared GND.
  Enable I2C: sudo raspi-config > Interface Options > I2C
  Install:    sudo apt install python3-smbus2
"""

import time

# ── Platform detection ────────────────────────────────────────────────────────

try:
    from machine import I2C as _MachineI2C, Pin as _Pin
    _PLATFORM = 'micropython'
except ImportError:
    _PLATFORM = 'linux'

# ── Configuration ─────────────────────────────────────────────────────────────

# MicroPython (Pi Pico) — adjust pins to match your wiring
MP_I2C_BUS = 0
MP_PIN_SDA  = 4       # GP4
MP_PIN_SCL  = 5       # GP5
MP_I2C_FREQ = 100_000 # 100 kHz

# Linux (Raspberry Pi) — /dev/i2c-<LINUX_I2C_BUS>
LINUX_I2C_BUS = 1     # i2c-1 = GPIO 2/3 on all Pi models

# Shared
ADDR = 0x40

# ── sleep_ms abstraction ──────────────────────────────────────────────────────

if _PLATFORM == 'micropython':
    def sleep_ms(ms):
        time.sleep_ms(ms)
else:
    def sleep_ms(ms):
        time.sleep(ms / 1000.0)

# ── I2C adapters ──────────────────────────────────────────────────────────────

class _MicroPythonAdapter:
    def __init__(self):
        self._bus = _MachineI2C(
            MP_I2C_BUS,
            scl=_Pin(MP_PIN_SCL),
            sda=_Pin(MP_PIN_SDA),
            freq=MP_I2C_FREQ,
        )
        self.description = "I2C{} SCL=GP{} SDA=GP{} {}kHz (MicroPython)".format(
            MP_I2C_BUS, MP_PIN_SCL, MP_PIN_SDA, MP_I2C_FREQ // 1000)

    def scan(self):
        return self._bus.scan()

    def write_reg(self, reg, val):
        self._bus.writeto_mem(ADDR, reg, bytes([val & 0xFF]))

    def read_reg(self, reg):
        return self._bus.readfrom_mem(ADDR, reg, 1)[0]

    def write_block(self, reg, data):
        self._bus.writeto_mem(ADDR, reg, bytes(data))

    def read_block(self, reg, n):
        return list(self._bus.readfrom_mem(ADDR, reg, n))


class _LinuxAdapter:
    def __init__(self):
        try:
            import smbus2 as _smbus2
        except ImportError:
            raise ImportError(
                "smbus2 not found. Install with: sudo apt install python3-smbus2")
        self._bus = _smbus2.SMBus(LINUX_I2C_BUS)
        self.description = "/dev/i2c-{} (Linux / smbus2)".format(LINUX_I2C_BUS)

    def scan(self):
        found = []
        for a in range(0x03, 0x78):
            try:
                self._bus.read_byte(a)
                found.append(a)
            except OSError:
                pass
        return found

    def write_reg(self, reg, val):
        self._bus.write_byte_data(ADDR, reg, val & 0xFF)

    def read_reg(self, reg):
        return self._bus.read_byte_data(ADDR, reg)

    def write_block(self, reg, data):
        self._bus.write_i2c_block_data(ADDR, reg, list(data))

    def read_block(self, reg, n):
        return self._bus.read_i2c_block_data(ADDR, reg, n)


_adapter = None

def _init():
    global _adapter
    if _PLATFORM == 'micropython':
        _adapter = _MicroPythonAdapter()
    else:
        _adapter = _LinuxAdapter()

# ── Register addresses (firmware v2 — buffer-free streaming) ─────────────────

REG_CTRL       = 0x00
REG_PATTERN    = 0x01
REG_SPEED      = 0x02
REG_FALLBACK   = 0x03
REG_N_LEDS_LO  = 0x04
REG_N_LEDS_HI  = 0x05
REG_N_COLS     = 0x06
REG_IDLE_TO    = 0x07
REG_PARAM1     = 0x08
REG_PARAM2     = 0x09
REG_PAL_SEL    = 0x0A

REG_GLB_G      = 0x10
REG_GLB_R      = 0x11
REG_GLB_B      = 0x12
REG_SEC_G      = 0x13
REG_SEC_R      = 0x14
REG_SEC_B      = 0x15

PAL_BASE       = 0x20   # 16 entries x (G,R,B), 0x20-0x4F
PAL_ENTRIES    = 16
IDX_BASE       = 0x50   # packed 4-bit indices, 2 LEDs/byte, 0x50-0xFF
IDX_BYTES      = 176

CTRL_RST     = 0x01
CTRL_GLB     = 0x02
CTRL_PAT_EN  = 0x04
CTRL_IDX_EN  = 0x08
CTRL_SAVE    = 0x10

PATTERNS = {
    0: "OFF",         1: "SOLID",     2: "CHASE",
    3: "BLINK",       4: "ALTERNATE", 5: "WIPE",
    6: "TWINKLE",     7: "RAINBOW",   8: "RAINBOW_MTX",
    9: "RETRO_BLINK", 10: "LARSON",
}

PALETTES = {
    0: "USER",   1: "FIRE",    2: "OCEAN",
    3: "FOREST", 4: "PARTY",   5: "MONO_W",
    6: "RAINBOW",7: "HEAT",
}

REG_NAMES = {
    REG_CTRL: "CTRL", REG_PATTERN: "PATTERN", REG_SPEED: "SPEED",
    REG_FALLBACK: "FALLBACK", REG_N_LEDS_LO: "N_LEDS_LO",
    REG_N_LEDS_HI: "N_LEDS_HI", REG_N_COLS: "N_COLS",
    REG_IDLE_TO: "IDLE_TO", REG_PARAM1: "PARAM1", REG_PARAM2: "PARAM2",
    REG_PAL_SEL: "PAL_SEL", REG_GLB_G: "GLB_G", REG_GLB_R: "GLB_R",
    REG_GLB_B: "GLB_B", REG_SEC_G: "SEC_G", REG_SEC_R: "SEC_R",
    REG_SEC_B: "SEC_B",
}

# Expected values after do_reset() — init_color={0x00,0xFF,0x00} => pure red on WS2812
RESET_DEFAULTS = {
    REG_CTRL: 0x00, REG_PATTERN: 0x01, REG_SPEED: 0x0A, REG_FALLBACK: 0x07,
    REG_N_LEDS_LO: 0x40, REG_N_LEDS_HI: 0x00, REG_N_COLS: 0x08,
    REG_IDLE_TO: 0x00, REG_PARAM1: 0x01, REG_PARAM2: 0x05, REG_PAL_SEL: 0x00,
    REG_GLB_G: 0x00, REG_GLB_R: 0xFF, REG_GLB_B: 0x00,
    REG_SEC_G: 0x00, REG_SEC_R: 0x00, REG_SEC_B: 0x00,
}

# Predefined palette colours from patterns.c PROGMEM: {GLB_G,GLB_R,GLB_B,SEC_G,SEC_R,SEC_B}
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

def wr(reg, val):       _adapter.write_reg(reg, val)
def rd(reg):            return _adapter.read_reg(reg)
def wr_block(reg, d):   _adapter.write_block(reg, d)
def rd_block(reg, n):   return _adapter.read_block(reg, n)

def device_reset():
    wr(REG_CTRL, CTRL_RST)
    sleep_ms(50)

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

def _fmt(v):
    if isinstance(v, list):
        return "[" + ", ".join("0x{:02X}".format(x) for x in v) + "]"
    return "0x{:02X}".format(v)

def _check(label, got, expected):
    if got == expected:
        _ok("{}: {}".format(label, _fmt(got)))
    else:
        _fail_msg("{}: expected {}, got {}".format(label, _fmt(expected), _fmt(got)))

# ── Automated tests ───────────────────────────────────────────────────────────

def test_scan():
    print("\n--- I2C Bus Scan ---")
    found = _adapter.scan()
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
        _check(REG_NAMES.get(reg, "0x{:02X}".format(reg)), rd(reg), expected)

def test_rw_registers():
    print("\n--- Config Register Read/Write ---")
    cases = [
        (REG_GLB_G,     0xAA, "GLB_G"),
        (REG_GLB_R,     0xBB, "GLB_R"),
        (REG_GLB_B,     0xCC, "GLB_B"),
        (REG_PATTERN,   0x07, "PATTERN"),
        (REG_SPEED,     0x20, "SPEED"),
        (REG_N_LEDS_LO, 0x10, "N_LEDS_LO"),
        (REG_N_LEDS_HI, 0x00, "N_LEDS_HI"),
        (REG_N_COLS,    0x08, "N_COLS"),
        (REG_IDLE_TO,   0x00, "IDLE_TO"),
        (REG_SEC_G,     0x11, "SEC_G"),
        (REG_SEC_R,     0x22, "SEC_R"),
        (REG_SEC_B,     0x33, "SEC_B"),
        (REG_PARAM1,    0x04, "PARAM1"),
        (REG_PARAM2,    0x05, "PARAM2"),
        (REG_PAL_SEL,   0x00, "PAL_SEL"),
    ]
    for reg, val, name in cases:
        wr(reg, val)
        _check(name, rd(reg), val)

def test_user_palette():
    print("\n--- User Palette Read/Write (0x20-0x4F) ---")
    entries = [(0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0x00, 0x00, 0xFF)]
    for i, (g, r, b) in enumerate(entries):
        wr_block(PAL_BASE + i * 3, [g, r, b])
    for i, (g, r, b) in enumerate(entries):
        d = rd_block(PAL_BASE + i * 3, 3)
        _check("PALETTE[{}] G".format(i), d[0], g)
        _check("PALETTE[{}] R".format(i), d[1], r)
        _check("PALETTE[{}] B".format(i), d[2], b)

def test_palette_block_write():
    print("\n--- Palette Block Write (multi-byte) ---")
    data = [
        0x10, 0x20, 0x30,
        0x40, 0x50, 0x60,
        0x70, 0x80, 0x90,
        0xA0, 0xB0, 0xC0,
    ]
    wr_block(PAL_BASE, data)
    back = rd_block(PAL_BASE, 12)
    _check("Block write readback", back, data)

def test_index_buffer():
    print("\n--- Index Buffer Read/Write (0x50-0xFF, 4-bit packed) ---")
    # LED 0 -> index 3 (low nibble), LED 1 -> index 9 (high nibble)
    wr(IDX_BASE, (9 << 4) | 3)
    _check("INDEX byte 0", rd(IDX_BASE), (9 << 4) | 3)
    # Full-range block write/readback
    data = [i & 0xFF for i in range(16)]
    wr_block(IDX_BASE, data)
    back = rd_block(IDX_BASE, len(data))
    _check("Index block write readback", back, data)

def test_ctrl_bits():
    print("\n--- CTRL Bit Fields ---")
    for bit, name in [(CTRL_GLB, "GLB"), (CTRL_PAT_EN, "PAT_EN"), (CTRL_IDX_EN, "IDX_EN")]:
        wr(REG_CTRL, bit)
        _check("{} set".format(name),   rd(REG_CTRL) & bit, bit)
        wr(REG_CTRL, 0x00)
        _check("{} clear".format(name), rd(REG_CTRL) & bit, 0x00)

def test_save_autoclear():
    print("\n--- CTRL_SAVE Auto-clear ---")
    wr(REG_CTRL, CTRL_SAVE)
    sleep_ms(20)   # allow the ATtiny85 main loop to process the write and EEPROM save
    _check("SAVE auto-cleared", rd(REG_CTRL) & CTRL_SAVE, 0x00)

def test_pattern_select():
    print("\n--- Pattern Register ---")
    for pat_id, name in PATTERNS.items():
        wr(REG_PATTERN, pat_id)
        _check(name, rd(REG_PATTERN), pat_id)

def test_speed_boundary():
    print("\n--- Speed Boundary Values ---")
    for val in [1, 128, 255]:
        wr(REG_SPEED, val)
        _check("SPEED={}".format(val), rd(REG_SPEED), val)

def test_palette_load():
    print("\n--- Predefined Palette Load ---")
    for pal_id, (eg, er, eb, sg, sr, sb) in PALETTE_COLOURS.items():
        name = PALETTES[pal_id]
        wr(REG_PAL_SEL, pal_id)
        sleep_ms(10)    # allow ATtiny85 main loop to process stop condition
        _check("{} GLB_G".format(name), rd(REG_GLB_G), eg)
        _check("{} GLB_R".format(name), rd(REG_GLB_R), er)
        _check("{} GLB_B".format(name), rd(REG_GLB_B), eb)
        _check("{} SEC_G".format(name), rd(REG_SEC_G), sg)
        _check("{} SEC_R".format(name), rd(REG_SEC_R), sr)
        _check("{} SEC_B".format(name), rd(REG_SEC_B), sb)

def test_all():
    _reset_counts()
    print("\n" + "=" * 42)
    print(" Running all tests")
    print("=" * 42)
    if not test_scan():
        print("\nDevice not found — aborting.")
        return
    test_reset()
    test_rw_registers()
    test_user_palette()
    test_palette_block_write()
    test_index_buffer()
    test_ctrl_bits()
    test_pattern_select()
    test_speed_boundary()
    test_palette_load()
    test_save_autoclear()
    test_reset()   # leave device in a clean state
    print("\n" + "=" * 42)
    print(" PASSED: {}   FAILED: {}".format(_pass, _fail))
    print("=" * 42)

# ── Interactive commands ───────────────────────────────────────────────────────

def dump_registers():
    print("\n  Addr  Register     Value  Detail")
    print("  ----  -----------  -----  ------")
    for reg in list(range(REG_PAL_SEL + 1)) + list(range(REG_GLB_G, REG_SEC_B + 1)):
        name   = REG_NAMES.get(reg, "?")
        val    = rd(reg)
        detail = ""
        if reg == REG_CTRL:
            parts = []
            if val & CTRL_RST:    parts.append("RST")
            if val & CTRL_GLB:    parts.append("GLB")
            if val & CTRL_PAT_EN: parts.append("PAT_EN")
            if val & CTRL_IDX_EN: parts.append("IDX_EN")
            if val & CTRL_SAVE:   parts.append("SAVE")
            detail = " | ".join(parts) if parts else "-"
        elif reg == REG_PATTERN:
            detail = PATTERNS.get(val, "?")
        elif reg == REG_FALLBACK:
            detail = PATTERNS.get(val, "?")
        elif reg == REG_PAL_SEL:
            detail = PALETTES.get(val, "?")
        print("  0x{:02X}  {:<12} 0x{:02X}   {}".format(reg, name, val, detail))
    n_leds = rd(REG_N_LEDS_LO) | (rd(REG_N_LEDS_HI) << 8)
    print("\n  N_LEDS (16-bit) = {}".format(n_leds))
    print("\n  User palette (first 4 entries):")
    for i in range(4):
        d = rd_block(PAL_BASE + i * 3, 3)
        print("    PALETTE[{}]  G=0x{:02X}  R=0x{:02X}  B=0x{:02X}".format(
            i, d[0], d[1], d[2]))

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
        sleep_ms(10)
        print("  Loaded {}".format(PALETTES[v]))

def cmd_set_colour(label, g_reg, r_reg, b_reg):
    print("  Set {} colour (WS2812 order: G R B, each 0-255)".format(label))
    g = _prompt_int("  G: ", 0, 255)
    r = _prompt_int("  R: ", 0, 255)
    b = _prompt_int("  B: ", 0, 255)
    if None not in (g, r, b):
        wr(g_reg, g); wr(r_reg, r); wr(b_reg, b)
        print("  {} = G:{} R:{} B:{}".format(label, g, r, b))

def cmd_set_palette_entry():
    i = _prompt_int("  Palette entry (0-15): ", 0, PAL_ENTRIES - 1)
    if i is None: return
    g = _prompt_int("  G (0-255): ", 0, 255)
    r = _prompt_int("  R (0-255): ", 0, 255)
    b = _prompt_int("  B (0-255): ", 0, 255)
    if None not in (g, r, b):
        wr_block(PAL_BASE + i * 3, [g, r, b])
        print("  PALETTE[{}] = G:{} R:{} B:{}".format(i, g, r, b))

def cmd_set_index():
    max_led = IDX_BYTES * 2 - 1
    i = _prompt_int("  LED index (0-{}): ".format(max_led), 0, max_led)
    if i is None: return
    idx = _prompt_int("  Palette index (0=off, 1-15): ", 0, 15)
    if idx is None: return
    byte_addr = IDX_BASE + (i >> 1)
    cur = rd(byte_addr)
    if i & 1:
        cur = (cur & 0x0F) | (idx << 4)
    else:
        cur = (cur & 0xF0) | idx
    wr(byte_addr, cur)
    print("  INDEX[{}] = {} (byte 0x{:02X} = 0x{:02X})".format(i, idx, byte_addr, cur))

def cmd_set_speed():
    v = _prompt_int("  Speed (1=fastest, 255=slowest): ", 1, 255)
    if v is not None:
        wr(REG_SPEED, v)
        print("  Speed = {}".format(v))

def cmd_set_nleds():
    max_n = IDX_BYTES * 2
    v = _prompt_int("  N_LEDS (1-{}): ".format(max_n), 1, max_n)
    if v is not None:
        wr(REG_N_LEDS_LO, v & 0xFF)
        wr(REG_N_LEDS_HI, (v >> 8) & 0xFF)
        print("  N_LEDS = {}".format(v))

def cmd_set_param1():
    v = _prompt_int("  PARAM1 (1-255): ", 1, 255)
    if v is not None:
        wr(REG_PARAM1, v)
        print("  PARAM1 = {}".format(v))

def cmd_set_param2():
    v = _prompt_int("  PARAM2 (1-255): ", 1, 255)
    if v is not None:
        wr(REG_PARAM2, v)
        print("  PARAM2 = {}".format(v))

def cmd_set_fallback():
    print("  Patterns:")
    for k, v in PATTERNS.items():
        print("    {:2d}  {}".format(k, v))
    v = _prompt_int("  Fallback pattern ID: ", 0, 10)
    if v is not None:
        wr(REG_FALLBACK, v)
        print("  FALLBACK set to {}".format(PATTERNS[v]))

def cmd_set_idle_timeout():
    v = _prompt_int("  Idle timeout, seconds (0=off): ", 0, 255)
    if v is not None:
        wr(REG_IDLE_TO, v)
        print("  IDLE_TO = {}".format(v))

def cmd_save():
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_SAVE)
    print("  CTRL_SAVE written — configuration persisted to EEPROM.")

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

def cmd_toggle_idx_en():
    ctrl = rd(REG_CTRL)
    if ctrl & CTRL_IDX_EN:
        wr(REG_CTRL, ctrl & ~CTRL_IDX_EN)
        print("  Index buffer mode OFF")
    else:
        wr(REG_CTRL, ctrl | CTRL_IDX_EN)
        print("  Index buffer mode ON")

def cmd_reset():
    device_reset()
    print("  Device reset complete.")

# ── CLI ───────────────────────────────────────────────────────────────────────

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
   v  Set speed
   n  Set N_LEDS (16-bit)
   a  Set PARAM1
   b  Set PARAM2
   e  Toggle PAT_EN (pattern engine)
   g  Toggle GLB (global colour mode)

 Index buffer / matrix mode
   L  Set palette entry (0-15)
   i  Set one LED's index (index buffer)
   I  Toggle IDX_EN (index buffer mode)

 Persistence
   f  Set FALLBACK pattern
   t  Set IDLE_TO (seconds)
   S  Save current config to EEPROM (CTRL_SAVE)

 Device
   x  Reset device (CTRL_RST)
   q  Quit
"""

def main():
    print("\nSpace Unicorn I2C Register Test Tool")
    print("Platform: " + _PLATFORM)
    print("Initialising I2C...")
    _init()
    print(_adapter.description)
    print("Target: 0x{:02X}".format(ADDR))

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
            elif cmd == 'v': cmd_set_speed()
            elif cmd == 'n': cmd_set_nleds()
            elif cmd == 'a': cmd_set_param1()
            elif cmd == 'b': cmd_set_param2()
            elif cmd == 'e': cmd_toggle_pat_en()
            elif cmd == 'g': cmd_toggle_glb()
            elif cmd == 'L': cmd_set_palette_entry()
            elif cmd == 'i': cmd_set_index()
            elif cmd == 'I': cmd_toggle_idx_en()
            elif cmd == 'f': cmd_set_fallback()
            elif cmd == 't': cmd_set_idle_timeout()
            elif cmd == 'S': cmd_save()
            elif cmd == 'x': cmd_reset()
            else: print("  Unknown command '{}'. Type q to quit.".format(cmd))
        except OSError as exc:
            print("  I2C error: {}".format(exc))
            print("  Check wiring and that the device is powered.")

    print("Bye.")

main()
