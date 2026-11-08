/* pic24-osc.c -- a PIC24FJ's oscillator (OSCCON, CLKDIV: the OSC
 * peripheral of the EDC) and its Peripheral Pin Select lock (PPS).
 *
 *   - Out of reset, OSCCON's COSC and NOSC are the configuration words'
 *     FNOSC (FOSCSEL<2:0>, in the last page of program memory), the
 *     oscillator the part starts on.
 *   - OSCCON's bytes take a write only right after their unlock sequence:
 *     0x46 then 0x57 written to the low byte, 0x78 then 0x9A to the high
 *     one (what XC16's __builtin_write_OSCCONL and _OSCCONH do); without
 *     it the write is ignored, as on the part, and the first such is a
 *     warning. Setting OSWEN switches to NOSC at once: COSC takes it,
 *     OSWEN clears, LOCK says a PLL is locked.
 *   - IOLOCK (OSCCON<6>) locks the PPS registers (RPINRx, RPORx): a write
 *     to them is ignored while it is set; with the IOL1WAY configuration
 *     bit (FOSC<4>) set, once set it stays.
 * The core's clock follows COSC and CLKDIV (pic24_fcy). Not modelled:
 * the fail-safe clock monitor, two-speed start-up, the FRC's tuning and
 * self-tuning, the reference clock output. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

/* the configuration words FOSCSEL and FOSC, where the part's EDC puts
 * them (a single-partition part's: the PIC24FJ1024GB610's by default) */
#define FOSCSEL_ADDR(s) rf_config_addr((s)->svd, "FOSCSEL", 0xabf18u)
#define FOSC_ADDR(s)    rf_config_addr((s)->svd, "FOSC", 0xabf1cu)

struct osc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *osccon;
    int key_lo, key_hi;             /* unlock sequence steps seen */
    int warned;
};

static u32 osc_read(void *ctx, u32 off, int n)
{
    struct osc *o = ctx;
    return rf_read(o->p, off, n);
}

static void locked(struct osc *o, const char *half)
{
    if (o->warned++)
        return;
    fflush(stdout);
    fprintf(stderr, "embsim: warning: a write at pc 0x%06x to OSCCON's %s byte is "
            "ignored: it needs its unlock sequence first\n",
            (unsigned)o->s->cpu->ops->pc(o->s->cpu), half);
}

static void osc_write(void *ctx, u32 off, int n, u32 v)
{
    struct osc *o = ctx;
    o->s->hz_changed = 1;
    u32 at = rf_off(o->osccon);
    if (off != at && off != at + 1) {
        rf_write(o->p, off, n, v);
        return;
    }
    u32 old = (u32)o->osccon->value, nv = old;
    /* each byte the write touches: the unlock sequence's step, or the
     * value it lets through */
    for (int b = 0; b < n; b++) {
        int hi = off + (u32)b == at + 1;
        u32 x = v >> (8 * b) & 0xff;
        int *key = hi ? &o->key_hi : &o->key_lo;
        u32 k1 = hi ? 0x78 : 0x46, k2 = hi ? 0x9a : 0x57;
        if (*key == 0 && x == k1) {
            *key = 1;
            continue;
        }
        if (*key == 1 && x == k2) {
            *key = 2;
            continue;
        }
        if (*key != 2) {
            *key = 0;
            locked(o, hi ? "high" : "low");
            continue;
        }
        *key = 0;
        if (hi)
            nv = (nv & 0x00ff) | (x & 0x07) << 8 | (nv & 0x7000);   /* NOSC */
        else {
            u32 keep = nv & 0x20;                /* LOCK: the hardware's */
            if ((nv & 0x40) && (rf_fields(o->osccon) & 0x40) &&
                (pic24_prog(o->s, FOSC_ADDR(o->s)) >> 4 & 1))
                x |= 0x40;                      /* IOL1WAY: IOLOCK stays */
            nv = (nv & 0xff00) | (x & ~0x20u) | keep;
        }
    }
    if (nv & 1) {                               /* OSWEN: switch now */
        u32 nosc = nv >> 8 & 7;
        nv = (nv & ~0x7001u) | nosc << 12;
        nv = (nosc == 1 || nosc == 3) ? nv | 0x20 : nv & ~0x20u;
    }
    if (nv != old)
        rf_hw(o->osccon, nv);
}

static void osc_reset(void *ctx)
{
    struct osc *o = ctx;
    o->s->hz_changed = 1;
    u32 fnosc = pic24_prog(o->s, FOSCSEL_ADDR(o->s)) & 7;
    u32 v = (u32)(o->osccon->value & ~0x7700ull) | fnosc << 12 | fnosc << 8;
    v = (fnosc == 1 || fnosc == 3) ? v | 0x20 : v & ~0x20u;
    o->osccon->value = o->osccon->reset = v;
    o->key_lo = o->key_hi = 0;
}

const struct dev_ops pic24_osc_ops = {
    "pic24-osc", osc_read, osc_write, osc_reset, 0, 0,
};

/* the core's reset, with the image's configuration words in place */
void pic24_osc_power_on(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "OSC") : 0;
    if (p && p->ops == &pic24_osc_ops)
        osc_reset(p->ctx);
}

void *pic24_osc_create(struct sim *s, struct rf_periph *p)
{
    struct osc *o = calloc(1, sizeof *o);
    if (!o)
        die("out of memory");
    o->s = s;
    o->p = p;
    o->osccon = rf_reg(p, "OSCCON");
    if (!o->osccon) {
        free(o);
        return 0;
    }
    return o;
}

/* ---- the PPS registers, locked by IOLOCK ----------------------------------- */

struct pps {
    struct sim *s;
    struct rf_periph *p;
};

static u32 pps_read(void *ctx, u32 off, int n)
{
    struct pps *q = ctx;
    return rf_read(q->p, off, n);
}

static void pps_write(void *ctx, u32 off, int n, u32 v)
{
    struct pps *q = ctx;
    struct rf_periph *osc = rf_periph(q->s->svd, "OSC");
    struct rf_reg *con = osc ? rf_reg(osc, "OSCCON") : 0;
    if (con && (con->value & 0x40))
        return;                                 /* IOLOCK */
    rf_write(q->p, off, n, v);
    pic24_pins_changed(q->s);                   /* a pin's output, or an input's pin */
}

const struct dev_ops pic24_pps_ops = {
    "pic24-pps", pps_read, pps_write, 0, 0, 0,
};

void *pic24_pps_create(struct sim *s, struct rf_periph *p)
{
    struct pps *q = calloc(1, sizeof *q);
    if (!q)
        die("out of memory");
    q->s = s;
    q->p = p;
    return q;
}
