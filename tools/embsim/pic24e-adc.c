/* pic24e-adc.c -- a dsPIC33E's or PIC24E's ADC1, over the EDC's registers
 * (AD1CON1-AD1CON4, AD1CHS0, AD1CSSL, AD1CSSH, ADC1BUF0-ADC1BUFF), by the
 * family reference manual's 10/12-bit ADC section, channel 0 alone.
 *
 *   - With AD1CON1.ADON: setting SAMP (or ASAM, again after each
 *     conversion) starts sampling; the conversion starts when the program
 *     clears SAMP (SSRC 000) or after SAMC TADs of sampling (SSRC 111,
 *     auto-convert), and takes 12 TADs (14 at 12 bits, AD12B). A TAD is
 *     ADCS + 1 instruction cycles, or with ADRC the RC clock's, taken as
 *     the instruction cycle's.
 *   - The input: CH0SA, or with CSCNA the next of those AD1CSSL and
 *     AD1CSSH select, in ascending order; ANn is the board's (struct
 *     board_io's analog, 0 V where nothing is connected), against AVDD at
 *     3.3 V, truncated to 10 or 12 bits, in FORM's format (integer or
 *     fractional, unsigned or signed).
 *   - The result goes to the next of ADC1BUF0-ADC1BUFF; DONE is set;
 *     after SMPI + 1 conversions the ADC1 interrupt (AD1) is flagged and
 *     the buffer starts again at ADC1BUF0 (at ADC1BUF8 every other time
 *     with BUFM, BUFS saying which).
 * Not modelled: the other trigger sources (the timers, INT0, the PWM, the
 * PTG), channels 1-3 and simultaneous sampling, MUX B (ALTS), the DMA's
 * buffer modes (ADDMAEN), the external references, ADC2. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NBUF 16
#define AVDD 3.3

#define CON1_ADON  (1u << 15)
#define CON1_AD12B (1u << 10)
#define CON1_ASAM  (1u << 2)
#define CON1_SAMP  (1u << 1)
#define CON1_DONE  1u

struct adc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *con1, *con2, *con3, *chs, *cssl, *cssh, *buf[NBUF];
    int irq;
    int running;
    int phase;                      /* 0 idle, 1 sampling, 2 converting */
    u64 left;                       /* cycles of the phase */
    int scan, fill, count, half, in;
};

static u64 tad(const struct adc *a)
{
    u64 c3 = a->con3->value;
    return (c3 & 0x8000) ? 1 : (c3 & 0xff) + 1;
}

static int ssrc(const struct adc *a)
{
    return (int)(a->con1->value >> 5 & 7);
}

static void set_clock(struct adc *a)
{
    rf_clock(a->p, &a->running, a->phase != 0);
}

static u32 selected(const struct adc *a)
{
    u32 sel = a->cssl ? (u32)a->cssl->value & 0xffff : 0;
    if (a->cssh)
        sel |= (u32)(a->cssh->value & 0xffff) << 16;
    return sel;
}

static int next_input(struct adc *a)
{
    u32 sel = selected(a);
    if (!(a->con2->value & (1u << 10)) || !sel)  /* CSCNA */
        return (int)(a->chs->value & 31);
    for (int k = 0; k < 32; k++) {
        int in = (a->scan + k) % 32;
        if (sel >> in & 1) {
            a->scan = in + 1;
            return in;
        }
    }
    return 0;
}

static void start_sampling(struct adc *a)
{
    rf_hw(a->con1, a->con1->value | CON1_SAMP);
    a->phase = 1;
    u64 samc = a->con3->value >> 8 & 0x1f;
    a->left = (samc ? samc : 1) * tad(a);
    set_clock(a);
}

static void start_conversion(struct adc *a)
{
    rf_hw(a->con1, a->con1->value & ~(u64)(CON1_SAMP | CON1_DONE));
    a->in = next_input(a);
    a->phase = 2;
    a->left = ((a->con1->value & CON1_AD12B) ? 14 : 12) * tad(a);
    set_clock(a);
}

static void finish(struct adc *a)
{
    struct board_io *io = &a->s->io;
    u64 c1 = a->con1->value, c2 = a->con2->value;
    int bits = (c1 & CON1_AD12B) ? 12 : 10;
    u32 top = (1u << bits) - 1;
    double v = io->analog ? io->analog(io->ctx, a->in) : 0.0;
    u32 code = v <= 0 ? 0 : v >= AVDD ? top : (u32)(v / AVDD * (top + 1));
    int form = (int)(c1 >> 8 & 3);
    /* signed: about mid-scale, extended to 16 bits; fractional: in the
     * word's top bits */
    s32 x = (form & 1) ? (s32)code - (s32)(1u << (bits - 1)) : (s32)code;
    if (form & 2)
        x *= (s32)1 << (16 - bits);
    u32 r = (u32)x & 0xffff;
    int slot = (a->half ? 8 : 0) + a->fill++;
    if (slot < NBUF && a->buf[slot])
        rf_hw(a->buf[slot], r);
    rf_hw(a->con1, a->con1->value | CON1_DONE);
    a->phase = 0;
    if (++a->count > (int)(c2 >> 2 & 31)) {
        a->count = 0;
        a->fill = 0;
        if (c2 & 2) {                           /* BUFM */
            a->half = !a->half;
            rf_hw(a->con2, (a->con2->value & ~0x80ull) | (a->half ? 0x80u : 0));
        }
        pic24_irq(a->s, a->irq);
    }
    if (a->con1->value & CON1_ASAM)
        start_sampling(a);
    set_clock(a);
}

static void adc_tick(void *ctx, u32 cycles)
{
    struct adc *a = ctx;
    u64 c = cycles;
    while (c && a->phase) {
        if (!(a->con1->value & CON1_ADON)) {
            a->phase = 0;
            break;
        }
        if (c < a->left) {
            a->left -= c;
            break;
        }
        c -= a->left;
        a->left = 0;
        if (a->phase == 1) {
            if (ssrc(a) == 7)
                start_conversion(a);
            else {
                a->left = ~0ull >> 1;           /* until SAMP clears */
                break;
            }
        } else {
            finish(a);
        }
    }
    set_clock(a);
}

static int adc_next_event(void *ctx, u32 *cycles)
{
    struct adc *a = ctx;
    if (!a->phase || !(a->con1->value & CON1_ADON))
        return 0;
    u64 t = a->left;
    if (a->phase == 1) {
        if (ssrc(a) != 7)
            return 0;
        t += ((a->con1->value & CON1_AD12B) ? 14 : 12) * tad(a);
    }
    *cycles = t > 0xffffffffu ? 0xffffffffu : (u32)t;
    return 1;
}

static u32 adc_read(void *ctx, u32 off, int n)
{
    struct adc *a = ctx;
    return rf_read(a->p, off, n);
}

static void adc_write(void *ctx, u32 off, int n, u32 v)
{
    struct adc *a = ctx;
    u64 before = a->con1->value;
    rf_write(a->p, off, n, v);
    if ((off & ~1u) != rf_off(a->con1))
        return;
    u64 now = a->con1->value;
    if (!(now & CON1_ADON)) {
        a->phase = 0;
        a->fill = a->count = a->scan = 0;
        set_clock(a);
        return;
    }
    int on = !(before & CON1_ADON);
    if ((now & CON1_SAMP) && !(before & CON1_SAMP) && a->phase == 0)
        start_sampling(a);
    else if ((now & CON1_ASAM) && (on || !(before & CON1_ASAM)) && a->phase == 0)
        start_sampling(a);
    else if (!(now & CON1_SAMP) && (before & CON1_SAMP) && a->phase == 1 && ssrc(a) == 0)
        start_conversion(a);                    /* SAMP cleared: convert */
}

static void adc_reset(void *ctx)
{
    struct adc *a = ctx;
    a->phase = 0;
    a->left = 0;
    a->scan = a->fill = a->count = a->half = 0;
    a->running = 0;
}

const struct dev_ops pic24e_adc_ops = {
    "pic24e-adc", adc_read, adc_write, adc_reset, adc_tick, adc_next_event,
};

void *pic24e_adc_create(struct sim *s, struct rf_periph *p)
{
    struct adc *a = calloc(1, sizeof *a);
    char nm[16];
    if (!a)
        die("out of memory");
    a->s = s;
    a->p = p;
    /* a conversion's end is its only effect: the input read, the flag */
    rf_lag(p, adc_next_event);
    a->con1 = rf_reg(p, "AD1CON1");
    a->con2 = rf_reg(p, "AD1CON2");
    a->con3 = rf_reg(p, "AD1CON3");
    a->chs = rf_reg(p, "AD1CHS0");
    a->cssl = rf_reg(p, "AD1CSSL");
    a->cssh = rf_reg(p, "AD1CSSH");
    if (!a->con1 || !a->con2 || !a->con3 || !a->chs) {
        free(a);
        return 0;
    }
    for (int k = 0; k < NBUF; k++) {
        snprintf(nm, sizeof nm, "ADC1BUF%X", k);
        a->buf[k] = rf_reg(p, nm);
    }
    rf_set_acc(a->con2, 0x80, ACC_RO);         /* BUFS */
    a->irq = pic24_irq_named(s, "AD1");
    return a;
}
