#ifndef PATTERNS_H
#define PATTERNS_H

#include <stdint.h>
#include <Light_WS2812/light_ws2812.h>

/* Pattern IDs written to REG_PATTERN */
#define PAT_OFF          0
#define PAT_SOLID        1
#define PAT_CHASE        2
#define PAT_BLINK        3
#define PAT_ALTERNATE    4
#define PAT_WIPE         5
#define PAT_TWINKLE      6
#define PAT_RAINBOW      7
#define PAT_RAINBOW_MTX  8
#define PAT_RETRO_BLINK  9
#define PAT_LARSON       10
#define PAT_MAX          PAT_LARSON

/* PAL_SEL values written to REG_PAL_SEL */
#define PAL_USER     0
#define PAL_FIRE     1
#define PAL_OCEAN    2
#define PAL_FOREST   3
#define PAL_PARTY    4
#define PAL_MONO_W   5
#define PAL_RAINBOW  6
#define PAL_HEAT     7

struct pat_state {
    uint16_t pos;    /* animation frame counter (16-bit: scans across all LEDs) */
    uint8_t  tick;   /* speed prescaler counter */
    uint8_t  lfsr;   /* LFSR seed — must never be 0 */
};

void pat_init(struct pat_state *s);

/*
 * Advance the animation by one prescaler tick.
 * Returns non-zero when the frame counter advanced (i.e. a redraw is due).
 */
uint8_t pat_tick(struct pat_state *s);

/*
 * Recompute per-frame constants (head/eye position, accumulator seeds) for the
 * active pattern. Call once before streaming a frame, then compute_led() per LED.
 */
void pat_prep(struct pat_state *s, uint16_t n);

/*
 * Compute the colour of LED i (0..n-1) for the active pattern, writing it into
 * *out in WS2812 G/R/B order. LEDs must be requested in ascending order because
 * some patterns advance a running accumulator to avoid per-pixel multiply/divide.
 */
void compute_led(struct pat_state *s, uint16_t i, uint16_t n, struct cRGB *out);

/* Non-zero for patterns that change every frame (need re-streaming on advance) */
uint8_t pat_is_animated(uint8_t pattern);

/* Copy a predefined palette (1-7) into the primary/secondary colour registers */
void load_palette(uint8_t pal_id);

#endif /* PATTERNS_H */
