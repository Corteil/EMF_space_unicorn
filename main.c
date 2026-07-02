/*
 * NeoPixel I2C Slave — Space Unicorn firmware v2 (buffer-free streaming)
 *
 * Pin map (ATtiny85):
 *   PB0 (pin 5) : I2C SDA
 *   PB1 (pin 6) : Heartbeat LED  (slow blink = alive; brief flash = I2C data)
 *   PB2 (pin 7) : I2C SCL
 *   PB3 (pin 2) : NeoPixel data
 *   PB4 (pin 3) : User button    (active-low, internal pull-up)
 *   PB5 (pin 1) : RESET
 *
 * There is no per-LED RGB frame buffer. Each pixel's colour is computed on the
 * fly (see patterns.c) as the WS2812 signal is clocked out, so the number of
 * LEDs is limited by power and frame rate, not by the ATtiny85's 512 bytes of
 * RAM. See i2c/i2c_slave_defs.h for the register map.
 *
 * Persistence:
 *   - Writing CTRL_SAVE stores the current configuration to the ATtiny85's
 *     internal EEPROM.
 *   - On power-on the saved configuration is reloaded (or firmware defaults if
 *     none), and the REG_FALLBACK pattern runs so the badge lights up without
 *     needing the control app.
 *   - If no I2C write is seen for REG_IDLE_TO seconds, the firmware reverts to
 *     the REG_FALLBACK pattern.
 */

#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <Light_WS2812/light_ws2812.h>
#include <i2c/i2c_slave_defs.h>
#include <i2c/i2c_machine.h>
#include "patterns.h"

#define PIN_NEOPIXEL  PB3
#define PIN_HB_LED    PB1
#define PIN_BUTTON    PB4

/* Default primary colour in WS2812 G/R/B order: pure red */
static const uint8_t init_color[3] PROGMEM = { 0x00, 0xFF, 0x00 };

/* Default 16-colour user palette, G/R/B per entry. Slots 0/1 are overwritten
 * with the primary/secondary colours by do_reset(). */
static const uint8_t default_palette[PAL_ENTRIES][3] PROGMEM = {
    { 255, 255, 255 }, {   0,   0,   0 }, {   0, 255,   0 }, { 255,   0,   0 },
    {   0,   0, 255 }, { 255, 255,   0 }, { 255,   0, 255 }, {   0, 255, 255 },
    { 128, 255,   0 }, {   0, 128, 255 }, { 255,   0, 128 }, {   0, 255, 128 },
    {  64,  64,  64 }, { 128, 128, 128 }, { 192, 192, 192 }, { 200, 255, 100 },
};

volatile uint8_t i2c_reg[I2C_N_REG];

static struct pat_state pat;
static uint8_t  last_pal_sel;
static uint8_t  last_pattern;
static uint8_t  dirty;              /* a fresh frame needs streaming */

/* Set by Timer0 at ~30 Hz; drives animation timing and the inactivity counter */
static volatile uint8_t  tick_flag;
static volatile uint16_t idle_ticks;
static volatile uint8_t  flash_timer;
static volatile uint8_t  btn_event;

/* ------------------------------------------------------------------ */
/* EEPROM persistence                                                   */
/* ------------------------------------------------------------------ */

#define EE_MAGIC_ADDR 0
#define EE_MAGIC_VAL  0x5E
/* Config block: 0x01..0x11 mirrors the control/colour registers */
#define EE_CFG_BASE   1     /* PATTERN,SPEED,FALLBACK,NLEDS_LO,NLEDS_HI,NCOLS,
                             * IDLE_TO,PARAM1,PARAM2,PAL_SEL,GLB_G,GLB_R,GLB_B,
                             * SEC_G,SEC_R,SEC_B  (16 bytes) */
#define EE_CFG_LEN    16
#define EE_PAL_BASE   (EE_CFG_BASE + EE_CFG_LEN)   /* 17: 48 palette bytes */

static const uint8_t cfg_regs[EE_CFG_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
};

static void save_state(void)
{
    uint8_t i;
    for (i = 0; i < EE_CFG_LEN; i++)
        eeprom_update_byte((uint8_t *)(EE_CFG_BASE + i), i2c_reg[cfg_regs[i]]);
    for (i = 0; i < PAL_ENTRIES * 3; i++)
        eeprom_update_byte((uint8_t *)(EE_PAL_BASE + i), i2c_reg[PAL_BASE + i]);
    eeprom_update_byte((uint8_t *)EE_MAGIC_ADDR, EE_MAGIC_VAL);   /* validate last */
}

/* Returns non-zero if a valid saved configuration was loaded. */
static uint8_t load_state(void)
{
    uint8_t i;
    if (eeprom_read_byte((uint8_t *)EE_MAGIC_ADDR) != EE_MAGIC_VAL)
        return 0;
    for (i = 0; i < EE_CFG_LEN; i++)
        i2c_reg[cfg_regs[i]] = eeprom_read_byte((uint8_t *)(EE_CFG_BASE + i));
    for (i = 0; i < PAL_ENTRIES * 3; i++)
        i2c_reg[PAL_BASE + i] = eeprom_read_byte((uint8_t *)(EE_PAL_BASE + i));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Timer0 ISR — heartbeat, button debounce, animation + idle timing     */
/* ------------------------------------------------------------------ */

ISR(TIMER0_OVF_vect)
{
    static uint8_t hb_div   = 0;
    static uint8_t btn_cnt  = 0;
    static uint8_t btn_held = 0;

    tick_flag = 1;
    if (idle_ticks < 0xFFFF)
        idle_ticks++;

    if (flash_timer) {
        flash_timer--;
        PORTB |= (1 << PIN_HB_LED);
    } else if (++hb_div >= 15) {
        hb_div = 0;
        PORTB ^= (1 << PIN_HB_LED);
    }

    /* User button — active low */
    if (!(PINB & (1 << PIN_BUTTON))) {
        if (btn_cnt < 255) btn_cnt++;
        if (btn_cnt == 3 && !btn_held) {
            btn_held  = 1;
            btn_event = 1;
        }
    } else {
        btn_cnt  = 0;
        btn_held = 0;
    }
}

static inline void hb_flash(void) { flash_timer = 6; }

/* ------------------------------------------------------------------ */
/* Configuration helpers                                                */
/* ------------------------------------------------------------------ */

static void init_default_palette(void)
{
    uint8_t i;
    for (i = 0; i < PAL_ENTRIES * 3; i++)
        i2c_reg[PAL_BASE + i] = pgm_read_byte(&default_palette[0][0] + i);
}

static void do_reset(void)
{
    REG_CTRL      = 0;
    REG_PATTERN   = PAT_SOLID;
    REG_SPEED     = 10;
    REG_FALLBACK  = PAT_RAINBOW;
    REG_N_LEDS_LO = 64;
    REG_N_LEDS_HI = 0;
    REG_N_COLS    = 8;
    REG_IDLE_TO   = 0;
    REG_PARAM1    = 1;
    REG_PARAM2    = 5;
    REG_PAL_SEL   = 0;

    REG_GLB_G = pgm_read_byte(init_color);
    REG_GLB_R = pgm_read_byte(init_color + 1);
    REG_GLB_B = pgm_read_byte(init_color + 2);
    REG_SEC_G = 0; REG_SEC_R = 0; REG_SEC_B = 0;

    init_default_palette();
    /* Slots 0 and 1 alias the primary / secondary colours */
    i2c_reg[PAL_BASE + 0] = REG_GLB_G;
    i2c_reg[PAL_BASE + 1] = REG_GLB_R;
    i2c_reg[PAL_BASE + 2] = REG_GLB_B;
    i2c_reg[PAL_BASE + 3] = REG_SEC_G;
    i2c_reg[PAL_BASE + 4] = REG_SEC_R;
    i2c_reg[PAL_BASE + 5] = REG_SEC_B;

    last_pal_sel = 0;
    pat_init(&pat);
}

static void apply_fallback(void)
{
    REG_PATTERN  = REG_FALLBACK;
    REG_CTRL     = CTRL_PAT_EN;
    last_pattern = REG_PATTERN;
    pat_init(&pat);
    dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Buffer-free frame streaming                                          */
/* ------------------------------------------------------------------ */

static void stream_frame(void)
{
    uint16_t n = ((uint16_t)REG_N_LEDS_HI << 8) | REG_N_LEDS_LO;
    uint16_t i;
    uint8_t  sreg;

    if (n == 0)
        n = 64;

    if (REG_CTRL & CTRL_IDX_EN) {
        /* Index mode is bounded by the size of the packed index buffer */
        if (n > (uint16_t)(IDX_BYTES * 2))
            n = IDX_BYTES * 2;
    } else {
        pat_prep(&pat, n);
    }

    /* Interrupts off for the whole frame: the WS2812 stream must not be
     * interrupted, and ws2812_sendarray() restores SREG (so it will not
     * re-enable interrupts while we hold them off). */
    sreg = SREG;
    cli();

    for (i = 0; i < n; i++) {
        struct cRGB px;
        if (REG_CTRL & CTRL_IDX_EN) {
            uint8_t byte = i2c_reg[IDX_BASE + (i >> 1)];
            uint8_t idx  = (i & 1) ? (byte >> 4) : (byte & 0x0F);
            uint8_t base = PAL_BASE + idx + (idx << 1);   /* PAL_BASE + idx*3 */
            px.g = i2c_reg[base];
            px.r = i2c_reg[base + 1];
            px.b = i2c_reg[base + 2];
        } else {
            compute_led(&pat, i, n, &px);
        }
        ws2812_sendarray((uint8_t *)&px, 3);
    }

    SREG = sreg;
    _delay_us(50);   /* latch */
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(void)
{
    DDRB  = (1 << PIN_NEOPIXEL) | (1 << PIN_HB_LED);
    PORTB = (1 << PIN_BUTTON);

    /* Timer0 overflow at 8 MHz / 1024 / 256 ≈ 30 Hz */
    TCCR0B = (1 << CS02) | (1 << CS00);
    TIMSK |= (1 << TOIE0);

    if (!load_state())
        do_reset();

    apply_fallback();      /* power on into the fallback effect */

    i2c_init();
    sei();

    for (;;) {
        uint8_t changed = i2c_check_stop();

        if (changed) {
            idle_ticks = 0;
            hb_flash();

            if (REG_CTRL & CTRL_RST) {
                do_reset();
                apply_fallback();
            } else {
                if (REG_CTRL & CTRL_SAVE) {
                    save_state();
                    REG_CTRL &= ~CTRL_SAVE;    /* acknowledge the save */
                }
                if (REG_PAL_SEL != last_pal_sel) {
                    load_palette(REG_PAL_SEL);
                    last_pal_sel = REG_PAL_SEL;
                }
                if (REG_PATTERN != last_pattern) {
                    last_pattern = REG_PATTERN;
                    pat_init(&pat);
                }
            }
            dirty = 1;
        }

        /* User button: cycle to the next pattern */
        if (btn_event) {
            btn_event = 0;
            uint8_t next = REG_PATTERN + 1;
            if (next > PAT_MAX) next = PAT_SOLID;
            REG_PATTERN  = next;
            REG_CTRL     = CTRL_PAT_EN;
            last_pattern = next;
            pat_init(&pat);
            idle_ticks = 0;
            dirty = 1;
        }

        /* Inactivity fallback */
        if (REG_IDLE_TO) {
            uint16_t ticks;
            cli(); ticks = idle_ticks; sei();
            if (ticks >= (uint16_t)REG_IDLE_TO * 30) {
                apply_fallback();
                cli(); idle_ticks = 0; sei();
            }
        }

        /* Advance the animation at ~30 Hz, subdivided by REG_SPEED */
        if (tick_flag) {
            tick_flag = 0;
            if (REG_CTRL & CTRL_PAT_EN) {
                if (pat_tick(&pat) && pat_is_animated(REG_PATTERN))
                    dirty = 1;
            }
        }

        if (dirty) {
            stream_frame();
            dirty = 0;
        }
    }
}
