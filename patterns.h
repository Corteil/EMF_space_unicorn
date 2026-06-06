#ifndef PATTERNS_H
#define PATTERNS_H

#include <stdint.h>

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
    uint8_t pos;    /* animation frame counter */
    uint8_t tick;   /* speed prescaler counter */
    uint8_t lfsr;   /* LFSR state — must never be 0 */
};

void pat_init(struct pat_state *s);
void pat_tick(struct pat_state *s);
void load_palette(uint8_t pal_id);

#endif /* PATTERNS_H */
