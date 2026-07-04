#include <avr/pgmspace.h>
#include <i2c/i2c_slave_defs.h>
#include "patterns.h"

extern volatile uint8_t i2c_reg[];

/*
 * Buffer-free pattern engine.
 *
 * There is no per-LED frame buffer: compute_led() produces each pixel's colour
 * on demand as the WS2812 stream is clocked out. Patterns that would otherwise
 * need a multiply or divide per pixel (rainbow hue = i * step, matrix row/col =
 * i / cols) instead advance a running accumulator, which works because LEDs are
 * always streamed in ascending index order. The ATtiny85 has no hardware
 * multiplier, so this keeps the inner loop short enough to avoid a WS2812 latch.
 */

/*
 * Predefined palettes stored in flash: {pri_G, pri_R, pri_B, sec_G, sec_R, sec_B}
 * WS2812 byte order is G, R, B.
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

/* Per-frame constants, recomputed once by pat_prep() then read by compute_led() */
static uint16_t s_head;        /* chase comet head */
static uint16_t s_eye;         /* larson eye position */
static uint16_t s_wpos;        /* wipe fill/clear position */
static uint8_t  s_hue;         /* rainbow running hue (advances per LED) */
static uint8_t  s_col;         /* matrix column accumulator */
static uint8_t  s_row;         /* matrix row accumulator */
static uint8_t  s_phase;       /* blink/alternate parity */

/*
 * Retro Blink and Twinkle need genuine per-LED memory to look randomly
 * time-varying rather than a fixed function of index — a small, deliberate
 * exception to the buffer-free design above.
 *
 * Retro Blink: one persistent on/off bit per LED (like an old blinkenlights
 * panel), capped to BLINK_MAX_LEDS so it stays cheap; PARAM1 bits flip per
 * frame advance. LEDs beyond the cap stay off.
 *
 * Twinkle: a small set of independently-timed "sparkle" slots, each holding
 * a target LED index and a fade level; when a slot's fade reaches zero it is
 * reseeded to a new random LED anywhere in the full strip.
 */
#define BLINK_MAX_LEDS   512
#define BLINK_MAP_BYTES  (BLINK_MAX_LEDS / 8)
#define MAX_SPARKLES     8

static uint8_t  blink_map[BLINK_MAP_BYTES];
static uint16_t sparkle_led[MAX_SPARKLES];
static uint8_t  sparkle_fade[MAX_SPARKLES];   /* 0 = slot inactive/available */

/* ------------------------------------------------------------------ */
/* Utilities                                                            */
/* ------------------------------------------------------------------ */

/*
 * Convert hue (0-255) to RGB using a 6-segment piecewise approximation.
 * No division, no floats. Outputs are in WS2812 wire order: *g, *r, *b.
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
        case 0: *r = 255; *g = t;   *b = 0;   break; /* red -> yellow   */
        case 1: *r = q;   *g = 255; *b = 0;   break; /* yellow -> green */
        case 2: *r = 0;   *g = 255; *b = t;   break; /* green -> cyan   */
        case 3: *r = 0;   *g = q;   *b = 255; break; /* cyan -> blue    */
        case 4: *r = t;   *g = 0;   *b = 255; break; /* blue -> magenta */
        default:*r = 255; *g = 0;   *b = q;   break; /* magenta -> red  */
    }
}

/* 16-bit Galois LFSR, taps 16/14/13/11 (mask 0xB400), period 65535. Seed must
 * never be 0 — an all-zero state is a fixed point and never advances. */
static uint16_t lfsr_tick(uint16_t v)
{
    uint16_t lsb = v & 1;
    v >>= 1;
    if (lsb)
        v ^= 0xB400;
    return v;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pat_init(struct pat_state *s)
{
    uint8_t i;

    s->pos  = 0;
    s->tick = 0;
    s->lfsr = 1;

    for (i = 0; i < BLINK_MAP_BYTES; i++)
        blink_map[i] = 0;
    for (i = 0; i < MAX_SPARKLES; i++)
        sparkle_fade[i] = 0;
}

uint8_t pat_is_animated(uint8_t pattern)
{
    return (pattern != PAT_OFF && pattern != PAT_SOLID);
}

uint8_t pat_tick(struct pat_state *s, uint16_t n)
{
    uint8_t speed = REG_SPEED ? REG_SPEED : 1;

    if (++s->tick < speed)
        return 0;
    s->tick = 0;
    s->pos++;

    if (n == 0)
        n = 1;

    if (REG_PATTERN == PAT_RETRO_BLINK) {
        uint8_t count = REG_PARAM1 ? REG_PARAM1 : 1;
        uint16_t limit = (n < BLINK_MAX_LEDS) ? n : BLINK_MAX_LEDS;
        uint8_t c;
        if (count > 8) count = 8;
        for (c = 0; c < count; c++) {
            uint16_t idx;
            s->lfsr = lfsr_tick(s->lfsr);
            idx = s->lfsr % limit;
            blink_map[idx >> 3] ^= (uint8_t)(1 << (idx & 7));
        }
    } else if (REG_PATTERN == PAT_TWINKLE) {
        uint8_t density = REG_PARAM1 ? REG_PARAM1 : 1;
        uint8_t k;
        if (density > MAX_SPARKLES) density = MAX_SPARKLES;
        for (k = 0; k < MAX_SPARKLES; k++) {
            if (k >= density) {
                sparkle_fade[k] = 0;
            } else if (sparkle_fade[k] == 0) {
                s->lfsr = lfsr_tick(s->lfsr);
                sparkle_led[k] = s->lfsr % n;
                sparkle_fade[k] = 7;
            } else {
                sparkle_fade[k]--;
            }
        }
    }

    return 1;
}

void pat_prep(struct pat_state *s, uint16_t n)
{
    if (n == 0)
        n = 1;

    s_phase = (uint8_t)(s->pos & 1);

    switch (REG_PATTERN) {
    case PAT_CHASE:
        s_head = s->pos % n;
        break;
    case PAT_WIPE:
        s_wpos = s->pos % (uint16_t)(n + n);
        break;
    case PAT_LARSON: {
        uint16_t span  = (n > 1) ? (n - 1) : 1;
        uint16_t cycle = span + span;
        uint16_t p     = s->pos % cycle;
        s_eye = (p <= span) ? p : (cycle - p);
        break;
    }
    case PAT_RAINBOW:
        s_hue = (uint8_t)s->pos;
        break;
    case PAT_RAINBOW_MTX:
        s_col = 0;
        s_row = 0;
        break;
    default:
        break;
    }
}

void compute_led(struct pat_state *s, uint16_t i, uint16_t n, struct cRGB *out)
{
    switch (REG_PATTERN) {

    case PAT_SOLID:
        out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B;
        break;

    case PAT_CHASE: {
        /* comet: head brightest, exponential fade over a trailing tail */
        uint16_t d = (s_head >= i) ? (s_head - i) : (s_head + n - i);
        uint8_t tail = REG_PARAM1 ? REG_PARAM1 : 1;
        if (d < tail) {
            uint8_t sh = (d < 7) ? (uint8_t)d : 7;
            out->g = REG_GLB_G >> sh;
            out->r = REG_GLB_R >> sh;
            out->b = REG_GLB_B >> sh;
        } else {
            out->g = 0; out->r = 0; out->b = 0;
        }
        break;
    }

    case PAT_BLINK:
        if (s_phase) { out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B; }
        else         { out->g = 0; out->r = 0; out->b = 0; }
        break;

    case PAT_ALTERNATE:
        if ((uint8_t)(i & 1) == s_phase) {
            out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B;
        } else {
            out->g = REG_SEC_G; out->r = REG_SEC_R; out->b = REG_SEC_B;
        }
        break;

    case PAT_WIPE: {
        uint8_t on = (s_wpos < n) ? (i <= s_wpos) : (i > (s_wpos - n));
        if (on) { out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B; }
        else    { out->g = 0; out->r = 0; out->b = 0; }
        break;
    }

    case PAT_TWINKLE: {
        /* Bright when this LED matches an active, freshly-seeded sparkle
         * slot; fades as that slot's counter runs down (see pat_tick()). */
        uint8_t k, shift = 8;
        for (k = 0; k < MAX_SPARKLES; k++) {
            if (sparkle_fade[k] && sparkle_led[k] == i) {
                shift = (uint8_t)(7 - sparkle_fade[k]);
                break;
            }
        }
        if (shift < 8) {
            out->g = REG_GLB_G >> shift;
            out->r = REG_GLB_R >> shift;
            out->b = REG_GLB_B >> shift;
        } else {
            out->g = 0; out->r = 0; out->b = 0;
        }
        break;
    }

    case PAT_RAINBOW:
        hue_to_rgb(s_hue, &out->g, &out->r, &out->b);
        s_hue += REG_PARAM2 ? REG_PARAM2 : 5;
        break;

    case PAT_RAINBOW_MTX: {
        uint8_t ncols = REG_N_COLS ? REG_N_COLS : 8;
        uint8_t c = s_col;
        if ((REG_PARAM1 & 1) && (s_row & 1))
            c = ncols - 1 - s_col;
        hue_to_rgb((uint8_t)(((c + s_row) << 4) + (uint8_t)s->pos),
                   &out->g, &out->r, &out->b);
        if (++s_col >= ncols) { s_col = 0; s_row++; }
        break;
    }

    case PAT_RETRO_BLINK: {
        /* Persistent per-LED bit, randomly toggled in pat_tick(); LEDs past
         * BLINK_MAX_LEDS have no state and stay off. */
        uint8_t on = (i < BLINK_MAX_LEDS) &&
                     (blink_map[i >> 3] & (1 << (i & 7)));
        if (on) { out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B; }
        else    { out->g = 0; out->r = 0; out->b = 0; }
        break;
    }

    case PAT_LARSON: {
        uint16_t d = (s_eye >= i) ? (s_eye - i) : (i - s_eye);
        uint8_t tail = REG_PARAM1 ? REG_PARAM1 : 1;      /* 1-32 */
        if (d < tail) {
            uint8_t sh = (d < 7) ? (uint8_t)d : 7;
            out->g = REG_GLB_G >> sh;
            out->r = REG_GLB_R >> sh;
            out->b = REG_GLB_B >> sh;
        } else {
            out->g = 0; out->r = 0; out->b = 0;
        }
        break;
    }

    case PAT_OFF:
    default:
        out->g = 0; out->r = 0; out->b = 0;
        break;
    }
}

/* Copy a predefined palette from flash into the primary/secondary registers. */
void load_palette(uint8_t pal_id)
{
    const uint8_t *src;

    if (pal_id == 0 || pal_id > 7)
        return;
    src = predefined_palettes[pal_id - 1];

    REG_GLB_G = pgm_read_byte(&src[0]);
    REG_GLB_R = pgm_read_byte(&src[1]);
    REG_GLB_B = pgm_read_byte(&src[2]);
    REG_SEC_G = pgm_read_byte(&src[3]);
    REG_SEC_R = pgm_read_byte(&src[4]);
    REG_SEC_B = pgm_read_byte(&src[5]);
}
