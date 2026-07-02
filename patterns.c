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
static uint8_t  s_retro_gen;   /* retro-blink generation seed */

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

/* Cheap 8-bit avalanche hash for deterministic per-LED pseudo-randomness. */
static uint8_t hash8(uint8_t x)
{
    x ^= (uint8_t)(x << 3);
    x ^= (uint8_t)(x >> 5);
    x ^= (uint8_t)(x << 1);
    return x;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pat_init(struct pat_state *s)
{
    s->pos  = 0;
    s->tick = 0;
    s->lfsr = 1;
}

uint8_t pat_is_animated(uint8_t pattern)
{
    return (pattern != PAT_OFF && pattern != PAT_SOLID);
}

uint8_t pat_tick(struct pat_state *s)
{
    uint8_t speed = REG_SPEED ? REG_SPEED : 1;

    if (++s->tick >= speed) {
        s->tick = 0;
        s->pos++;
        return 1;
    }
    return 0;
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
    case PAT_RETRO_BLINK: {
        uint8_t flips = REG_PARAM1 ? REG_PARAM1 : 1;
        s_retro_gen = (uint8_t)((uint8_t)s->pos * flips);
        break;
    }
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
        /* fraction of LEDs (density) sparkle, each fading on its own phase */
        uint8_t h = hash8((uint8_t)i);
        uint8_t density = REG_PARAM1 ? REG_PARAM1 : 1;   /* 1-8 */
        if ((h & 7) < density) {
            uint8_t d = ((uint8_t)s->pos + h) & 7;       /* 0..7 fade ramp */
            out->g = REG_GLB_G >> d;
            out->r = REG_GLB_R >> d;
            out->b = REG_GLB_B >> d;
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
        uint8_t h = hash8((uint8_t)((uint8_t)i ^ s_retro_gen));
        if (h & 1) { out->g = REG_GLB_G; out->r = REG_GLB_R; out->b = REG_GLB_B; }
        else       { out->g = 0; out->r = 0; out->b = 0; }
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
