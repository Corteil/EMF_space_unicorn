/*
 * Neopixel I2C Slave — Space Unicorn firmware
 *
 * Pin map (ATtiny85):
 *   PB0 (pin 5) : I2C SDA
 *   PB1 (pin 6) : Heartbeat LED  (slow blink = alive; brief flash = I2C data)
 *   PB2 (pin 7) : I2C SCL
 *   PB3 (pin 2) : NeoPixel data
 *   PB4 (pin 3) : User button    (active-low, internal pull-up)
 *   PB5 (pin 1) : RESET
 *
 * Register map (I2C address 0x40):
 *   0x00 CTRL     bit0=RST  bit1=GLB  bit2=PAT_EN
 *   0x01 GLB_G    primary colour green
 *   0x02 GLB_R    primary colour red
 *   0x03 GLB_B    primary colour blue
 *   0x04 PATTERN  0-9 (PAT_* constants)
 *   0x05 SPEED    ticks per animation frame (1=fast, 255=slow)
 *   0x06 N_LEDS   active LED count (≤ compile-time N_LEDS)
 *   0x07 SEC_G    secondary colour green
 *   0x08 SEC_R    secondary colour red
 *   0x09 SEC_B    secondary colour blue
 *   0x0A PARAM1   pattern-specific parameter
 *   0x0B PAL_SEL  0=user colours, 1-7=predefined palette
 *   0x0C+         LED array, 3 bytes each (G, R, B)
 */

#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <Light_WS2812/light_ws2812.h>
#include <i2c/i2c_slave_defs.h>
#include <i2c/i2c_machine.h>
#include "patterns.h"

#define PIN_NEOPIXEL  PB3
#define PIN_HB_LED    PB1
#define PIN_BUTTON    PB4

volatile uint8_t i2c_reg[I2C_N_REG];
const uint8_t init_color[3] PROGMEM = { 0x00, 0xFF, 0x00 };

static struct pat_state pat;
static uint8_t last_pal_sel;

/*
 * flash_timer: set to non-zero to override heartbeat with a solid-on flash.
 * Decremented by Timer0 ISR at ~30 Hz; ~6 counts ≈ 200 ms flash.
 * btn_event: set by ISR when a debounced button press is detected.
 */
static volatile uint8_t flash_timer;
static volatile uint8_t btn_event;

/*
 * Timer0 ISR — fires at ~30 Hz (8 MHz / 1024 / 256).
 * Handles heartbeat LED and user-button debounce independently of
 * the main loop rate, which varies with NeoPixel output time.
 */
ISR(TIMER0_OVF_vect)
{
    static uint8_t hb_div   = 0;
    static uint8_t btn_cnt  = 0;
    static uint8_t btn_held = 0;

    /* Heartbeat / data-flash LED */
    if (flash_timer) {
        flash_timer--;
        PORTB |= (1 << PIN_HB_LED);    /* keep LED on during flash */
    } else {
        /* Toggle at ~1 Hz: divide 30 Hz by 15 */
        if (++hb_div >= 15) {
            hb_div = 0;
            PORTB ^= (1 << PIN_HB_LED);
        }
    }

    /* User button debounce — active low (pull-up, button shorts to GND) */
    if (!(PINB & (1 << PIN_BUTTON))) {
        if (btn_cnt < 255) btn_cnt++;
        /* 3 consecutive samples at 30 Hz ≈ 100 ms hold */
        if (btn_cnt == 3 && !btn_held) {
            btn_held  = 1;
            btn_event = 1;
        }
    } else {
        btn_cnt  = 0;
        btn_held = 0;
    }
}

static inline void hb_flash(void)
{
    flash_timer = 6; /* ~200 ms at 30 Hz */
}

static inline void set_leds_global(void)
{
    ws2812_setleds_constant((struct cRGB *)&REG_GLB_G, REG_N_LEDS);
}

static inline void update_leds(void)
{
    ws2812_sendarray((uint8_t *)(i2c_reg + I2C_N_GLB_REG), REG_N_LEDS * 3);
}

void do_reset(void)
{
    uint8_t i;
    volatile uint8_t *p = i2c_reg + I2C_N_GLB_REG;

    cli();

    REG_GLB_G = 0; REG_GLB_R = 0; REG_GLB_B = 0;
    ws2812_setleds_constant((struct cRGB *)&REG_GLB_G, N_LEDS);

    REG_GLB_G = pgm_read_byte(init_color);
    REG_GLB_R = pgm_read_byte(init_color + 1);
    REG_GLB_B = pgm_read_byte(init_color + 2);

    REG_CTRL    = 0;
    REG_PATTERN = PAT_SOLID;
    REG_SPEED   = 10;
    REG_N_LEDS  = N_LEDS;
    REG_SEC_G   = 0; REG_SEC_R = 0; REG_SEC_B = 0;
    REG_PARAM1  = 1;
    REG_PAL_SEL = 0;

    for (i = N_LEDS * 3; i; i--)
        *p++ = 0;

    sei();

    pat_init(&pat);
    last_pal_sel = 0;
}

void swirly(void)
{
    uint8_t led = N_LEDS;
    volatile uint8_t *p = i2c_reg + I2C_N_GLB_REG + (N_LEDS * 3);
    uint8_t g = pgm_read_byte(init_color);
    uint8_t r = pgm_read_byte(init_color + 1);
    uint8_t b = pgm_read_byte(init_color + 2);
    uint8_t tmp;

    /* Initialise a bright spot with a dimming tail: { 255, 127, 63, 31, 15... } */
    while (led--) {
        *(--p) = b; if (b & 0xf0) b >>= 1;
        *(--p) = r; if (r & 0xf0) r >>= 1;
        *(--p) = g; if (g & 0xf0) g >>= 1;
    }

    g = pgm_read_byte(init_color);
    r = pgm_read_byte(init_color + 1);
    b = pgm_read_byte(init_color + 2);

    while (1) {
        update_leds();

        led = N_LEDS;
        p = &i2c_reg[I2C_N_GLB_REG];
        while (led--) {
            tmp = *p; *(p++) = g; if (led) g = tmp;
            tmp = *p; *(p++) = r; if (led) r = tmp;
            tmp = *p; *(p++) = b; if (led) b = tmp;
        }

        tmp = 70;
        while (tmp--) {
            if (i2c_check_stop())
                return;
            if (btn_event)
                return;
            _delay_ms(1);
        }
    }
}

int main(void)
{
    /* PB1 and PB3 outputs; PB4 input with internal pull-up */
    DDRB  = (1 << PIN_NEOPIXEL) | (1 << PIN_HB_LED);
    PORTB = (1 << PIN_BUTTON);

    /* Timer0: overflow at 8 MHz / 1024 / 256 ≈ 30 Hz for heartbeat + button */
    TCCR0B = (1 << CS02) | (1 << CS00);
    TIMSK |= (1 << TOIE0);

    REG_PATTERN = PAT_SOLID;
    REG_SPEED   = 10;
    REG_N_LEDS  = N_LEDS;
    REG_PARAM1  = 1;

    pat_init(&pat);
    last_pal_sel = 0;

    i2c_init();
    sei();

    swirly();
    goto inner;

    while (1) {
        if (i2c_check_stop()) {
inner:
            hb_flash();

            if (REG_CTRL & CTRL_RST) {
                do_reset();
            } else {
                if (REG_PAL_SEL != last_pal_sel) {
                    load_palette(REG_PAL_SEL);
                    last_pal_sel = REG_PAL_SEL;
                }
                if (!(REG_CTRL & CTRL_PAT_EN)) {
                    if (REG_CTRL & CTRL_GLB)
                        set_leds_global();
                    else
                        update_leds();
                }
            }
        }

        /* Button press: cycle to next pattern and enable pattern engine */
        if (btn_event) {
            btn_event = 0;
            uint8_t next = REG_PATTERN + 1;
            if (next > PAT_RETRO_BLINK) next = PAT_SOLID;
            REG_PATTERN = next;
            if (!REG_GLB_G && !REG_GLB_R && !REG_GLB_B) {
                REG_GLB_G = pgm_read_byte(init_color);
                REG_GLB_R = pgm_read_byte(init_color + 1);
                REG_GLB_B = pgm_read_byte(init_color + 2);
            }
            REG_CTRL |= CTRL_PAT_EN;
            pat_init(&pat);
        }

        if (REG_CTRL & CTRL_PAT_EN) {
            pat_tick(&pat);
            update_leds();
        }
    }
}
