#include <avr/pgmspace.h>
#include <i2c/i2c_slave_defs.h>
#include "patterns.h"

extern volatile uint8_t i2c_reg[];

/*
 * Predefined palettes stored in flash: {pri_G, pri_R, pri_B, sec_G, sec_R, sec_B}
 * WS2812 byte order is G, R, B.
 *
 * PAL_FIRE(1): orange / dark-red
 * PAL_OCEAN(2): cyan / deep-blue
 * PAL_FOREST(3): lime / dark-green
 * PAL_PARTY(4): red / blue
 * PAL_MONO_W(5): white / dim-white
 * PAL_RAINBOW(6): red / blue  (pair used by non-rainbow patterns; rainbow pattern ignores it)
 * PAL_HEAT(7): red / yellow
 */
static const uint8_t predefined_palettes[7][6] PROGMEM = {
    /* 1 FIRE    */ { 100, 255,   0,   0, 128,   0 },
    /* 2 OCEAN   */ { 255,   0, 255,   0,   0,  64 },
    /* 3 FOREST  */ { 200,  50,   0,  48,  10,   0 },
    /* 4 PARTY   */ {   0, 255,   0,   0,   0, 255 },
    /* 5 MONO_W  */ { 255, 255, 255,  32,  32,  32 },
    /* 6 RAINBOW */ {   0, 255,   0,   0,   0, 255 },
    /* 7 HEAT    */ {   0, 255,   0, 200, 255,   0 },
};

/* One bit per LED; sized for the compile-time maximum */
static uint8_t blink_map[(N_LEDS + 7) / 8];

/* ------------------------------------------------------------------ */
/* Utility functions                                                    */
/* ------------------------------------------------------------------ */

/* 8-bit Galois LFSR, polynomial 0xB8, period 255. State must never be 0. */
static uint8_t lfsr_tick(uint8_t v)
{
    uint8_t lsb = v & 1;
    v >>= 1;
    if (lsb) v ^= 0xB8;
    return v;
}

/*
 * Convert hue (0-255) to RGB using a 6-segment piecewise approximation.
 * No division, no floats: frac*6 computed as (frac<<2)+(frac<<1).
 * Outputs are in WS2812 wire order: *g, *r, *b.
 */
static void hue_to_rgb(uint8_t hue, uint8_t *g, uint8_t *r, uint8_t *b)
{
    uint8_t seg, frac, t, q;

    if      (hue <  43) { seg = 0; frac = hue; }
    else if (hue <  86) { seg = 1; frac = hue -  43; }
    else if (hue < 129) { seg = 2; frac = hue -  86; }
    else if (hue < 172) { seg = 3; frac = hue - 129; }
    else if (hue < 215) { seg = 4; frac = hue - 172; }
    else                { seg = 5; frac = hue - 215; }

    t = (frac << 2) + (frac << 1); /* frac * 6, max 252 */
    q = 255 - t;

    switch (seg) {
        case 0: *r = 255; *g = t;   *b = 0;   break; /* red → yellow  */
        case 1: *r = q;   *g = 255; *b = 0;   break; /* yellow → green */
        case 2: *r = 0;   *g = 255; *b = t;   break; /* green → cyan   */
        case 3: *r = 0;   *g = q;   *b = 255; break; /* cyan → blue    */
        case 4: *r = t;   *g = 0;   *b = 255; break; /* blue → magenta */
        default:*r = 255; *g = 0;   *b = q;   break; /* magenta → red  */
    }
}

/* Write one LED into the i2c_reg LED buffer. i*3 computed without multiply. */
static inline void write_led(uint8_t i, uint8_t g, uint8_t r, uint8_t b)
{
    volatile uint8_t *p = i2c_reg + I2C_N_GLB_REG + i + (i << 1); /* i*3 */
    *p++ = g;
    *p++ = r;
    *p   = b;
}

/* ------------------------------------------------------------------ */
/* Pattern functions                                                    */
/* ------------------------------------------------------------------ */

static void pat_off(uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++)
        write_led(i, 0, 0, 0);
}

static void pat_solid(uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; i++)
        write_led(i, REG_GLB_G, REG_GLB_R, REG_GLB_B);
}

/* Scrolling comet: dim entire buffer by >>1 each advance, place bright head. */
static void pat_chase(struct pat_state *s, uint8_t n, uint8_t advance)
{
    volatile uint8_t *base;
    uint8_t i, head;

    if (!advance) return;

    base = i2c_reg + I2C_N_GLB_REG;
    for (i = 0; i < n; i++) {
        base[i + (i << 1)]     >>= 1;
        base[i + (i << 1) + 1] >>= 1;
        base[i + (i << 1) + 2] >>= 1;
    }

    head = s->pos % n;
    write_led(head, REG_GLB_G, REG_GLB_R, REG_GLB_B);
}

/* All LEDs toggle between primary color and off. */
static void pat_blink(struct pat_state *s, uint8_t n, uint8_t advance)
{
    if (!advance) return;
    if (s->pos & 1)
        pat_solid(n);
    else
        pat_off(n);
}

/* Even LEDs = primary, odd = secondary; groups swap each advance. */
static void pat_alternate(struct pat_state *s, uint8_t n)
{
    uint8_t phase = s->pos & 1;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if ((i & 1) == phase)
            write_led(i, REG_GLB_G, REG_GLB_R, REG_GLB_B);
        else
            write_led(i, REG_SEC_G, REG_SEC_R, REG_SEC_B);
    }
}

/* Fill LEDs one by one left→right, then clear one by one. */
static void pat_wipe(struct pat_state *s, uint8_t n, uint8_t advance)
{
    uint8_t cycle, wpos;

    if (!advance) return;

    cycle = n + n;
    wpos  = s->pos % cycle;
    if (wpos < n)
        write_led(wpos,     REG_GLB_G, REG_GLB_R, REG_GLB_B);
    else
        write_led(wpos - n, 0, 0, 0);
}

/* Random LED lights up; all others fade by >>1 each advance. */
static void pat_twinkle(struct pat_state *s, uint8_t n, uint8_t advance)
{
    volatile uint8_t *base;
    uint8_t i, idx;

    if (!advance) return;

    base = i2c_reg + I2C_N_GLB_REG;
    for (i = 0; i < n; i++) {
        base[i + (i << 1)]     >>= 1;
        base[i + (i << 1) + 1] >>= 1;
        base[i + (i << 1) + 2] >>= 1;
    }

    s->lfsr = lfsr_tick(s->lfsr);
    idx = s->lfsr % n;
    write_led(idx, REG_GLB_G, REG_GLB_R, REG_GLB_B);
}

/* Hue cycles across the strip with rotation driven by pos. */
static void pat_rainbow(struct pat_state *s, uint8_t n)
{
    uint8_t i, g, r, b;
    for (i = 0; i < n; i++) {
        /* i*5 = (i<<2)+i spreads ~51 LEDs across the full hue wheel */
        hue_to_rgb(s->pos + (i << 2) + i, &g, &r, &b);
        write_led(i, g, r, b);
    }
}

/*
 * Diagonal rainbow across an 8x8 grid with rotation.
 * REG_PARAM1 bit 0: 0 = row-major wiring, 1 = serpentine (zigzag) wiring.
 */
static void pat_rainbow_matrix(struct pat_state *s, uint8_t n)
{
    uint8_t i, row, col, g, r, b;
    for (i = 0; i < n; i++) {
        row = i >> 3;
        col = i & 7;
        if ((REG_PARAM1 & 1) && (row & 1))
            col = 7 - col;
        hue_to_rgb(((col + row) << 4) + s->pos, &g, &r, &b);
        write_led(i, g, r, b);
    }
}

/*
 * 50s-computer effect: random LEDs toggle independently.
 * REG_PARAM1 controls how many LEDs flip per advance (1-8).
 */
static void pat_retro_blink(struct pat_state *s, uint8_t n, uint8_t advance)
{
    uint8_t count, c, i, idx;

    if (!advance) return;

    count = REG_PARAM1;
    if (count < 1) count = 1;
    if (count > 8) count = 8;

    for (c = 0; c < count; c++) {
        s->lfsr = lfsr_tick(s->lfsr);
        idx = s->lfsr % n;
        blink_map[idx >> 3] ^= (1 << (idx & 7));
    }

    for (i = 0; i < n; i++) {
        if (blink_map[i >> 3] & (1 << (i & 7)))
            write_led(i, REG_GLB_G, REG_GLB_R, REG_GLB_B);
        else
            write_led(i, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pat_init(struct pat_state *s)
{
    uint8_t i;
    s->pos  = 0;
    s->tick = 0;
    s->lfsr = 1; /* LFSR state must never be 0 */
    for (i = 0; i < (uint8_t)sizeof(blink_map); i++)
        blink_map[i] = 0;
}

void pat_tick(struct pat_state *s)
{
    uint8_t speed   = REG_SPEED  ? REG_SPEED  : 1;
    uint8_t n       = REG_N_LEDS ? REG_N_LEDS : N_LEDS;
    uint8_t advance = 0;

    if (n > N_LEDS) n = N_LEDS; /* clamp to compile-time max */

    if (++s->tick >= speed) {
        s->tick = 0;
        s->pos++;
        advance = 1;
    }

    switch (REG_PATTERN) {
        case PAT_OFF:          pat_off(n);                         break;
        case PAT_SOLID:        pat_solid(n);                       break;
        case PAT_CHASE:        pat_chase(s, n, advance);           break;
        case PAT_BLINK:        pat_blink(s, n, advance);           break;
        case PAT_ALTERNATE:    pat_alternate(s, n);                break;
        case PAT_WIPE:         pat_wipe(s, n, advance);            break;
        case PAT_TWINKLE:      pat_twinkle(s, n, advance);         break;
        case PAT_RAINBOW:      pat_rainbow(s, n);                  break;
        case PAT_RAINBOW_MTX:  pat_rainbow_matrix(s, n);           break;
        case PAT_RETRO_BLINK:  pat_retro_blink(s, n, advance);    break;
        default:               pat_off(n);                         break;
    }
}

/* Copy a predefined palette from flash into REG_GLB and REG_SEC registers. */
void load_palette(uint8_t pal_id)
{
    const uint8_t *src;

    if (pal_id == 0 || pal_id > 7) return;
    src = predefined_palettes[pal_id - 1];

    REG_GLB_G = pgm_read_byte(&src[0]);
    REG_GLB_R = pgm_read_byte(&src[1]);
    REG_GLB_B = pgm_read_byte(&src[2]);
    REG_SEC_G = pgm_read_byte(&src[3]);
    REG_SEC_R = pgm_read_byte(&src[4]);
    REG_SEC_B = pgm_read_byte(&src[5]);
}
