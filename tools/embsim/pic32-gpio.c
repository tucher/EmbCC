/* pic32-gpio.c -- a PIC32MZ's I/O ports, A to K, over the ATDF's GPIO
 * registers (ANSELx, TRISx, PORTx, LATx, ODCx, CNPUx, CNPDx...); and a
 * PIC24's, over the EDC's (ANSx for ANSELx, IOCPUx for CNPUx).
 *
 *   - Out of reset every pin is an input (TRISx all ones) and every pin
 *     that has an analog function is analog (ANSELx: its fields, which
 *     atdf.c takes from the pack's device header -- the ATDF gives every
 *     ANSELx sixteen), as on the part.
 *   - PORTx reads the pins: an output (TRISx 0) what LATx drives -- an
 *     open-drain one (ODCx) at 1 lets go, and reads as an input does; an
 *     analog pin 0 (its digital input is off); an input what the board
 *     drives on it (a button), else its pull-up (CNPUx) as 1, else 0.
 *   - A write to PORTx writes LATx, as on the part.
 *   - The board is told each pin whose driven level changes -- high,
 *     low, or let go (an LED, either way round) -- and asked for the
 *     pins it drives (struct board_io).
 *   - A PIC24's interrupt-on-change: with PADCON.IOCON set, a pin whose
 *     level rises (IOCPx) or falls (IOCNx) sets its IOCFx bit, its port's
 *     IOCSTAT bit and the IOC interrupt's flag; the program clears IOCFx
 *     bits (it cannot set them), and IOCSTAT follows. As the data sheet
 *     says, an output's LATx is what changes a pin in a test (the board's
 *     inputs are set when the run starts).
 *   - A PIC32MX's: port B's analog pins are AD1PCFG's clear bits (the
 *     ADC's register, 1 for digital), the pull-ups CNPUE's by each pin's
 *     change-notice number (CNn, the device file's pin names), and with
 *     CNCON.ON a pin whose CNEN bit is set flags CHANGE_NOTICE when its
 *     level changes, either way (the part compares with the last read of
 *     PORTx: here, with the last level).
 *   - A dsPIC33E's or PIC24E's change notification (DS70616G §11.3): a
 *     pin whose CNENx bit is set flags the CN interrupt when its level
 *     changes, either way, against its last level. The data sheet does
 *     not say whether the part compares with the last sample or the last
 *     read of PORTx; a program that reads PORTx in its handler, as the
 *     manual's examples do, sees the same interrupts either way.
 *   - A PIC24's peripherals on its pins: an output a peripheral drives
 *     (pic24_pps_out: a PPS output function, on each RPn pin whose RPORx
 *     field selects it; pic24_pin_out: a pin of its own, an MCCP's OCMxA)
 *     is the pin's level, over TRISx and LATx, as the data sheet gives
 *     the peripherals priority; one a peripheral drives open-drain and
 *     lets go (PIC24_PIN_RELEASED: an I2C unit's line) is still the
 *     peripheral's, TRISx and LATx without effect (DS30009711C §3.6), and
 *     reads as an input does (§3.3). The peripherals that watch their
 *     inputs (an input capture) are told when a pin may have changed,
 *     with the time it did. The pins are the EDC's, by name.
 *   - A bus outside the part (pic24_pin_bus: an I2C bus's lines): the pin
 *     is pulled up outside, and reads 0 while the bus holds it low or the
 *     part drives it low, else 1 (DS70000195G §2.0: open drains on a
 *     wired-AND line, with pull-up resistors).
 * PORTx's value shows in --trace-periph when a read changes it. Not
 * modelled: a PIC32's change notification interrupts (CNx), the slew
 * rate. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "pic32.h"
#include "svd-map.h"

#define NPORTS 10                   /* A to K, without I */

struct port {
    struct rf_reg *ansel, *tris, *port, *lat, *odc, *cnpu;
    struct rf_reg *iocp, *iocn, *iocf;      /* a PIC24's interrupt-on-change */
    struct rf_reg *cnen;            /* a dsPIC33E's or PIC24E's change notification */
    u32 pins;                       /* the pins the port has */
    u32 high, low;                  /* driven high and low, as the board was told */
    u32 level;                      /* the pins' levels, as last looked at */
    u32 bus, bus_low;               /* lines of a bus outside, and those it holds low */
};

struct gpio {
    struct sim *s;
    struct rf_periph *p;
    struct port pt[NPORTS];
    struct rf_reg *padcon, *iocstat;        /* PAD's: IOCON, the ports' flags */
    int ioc_irq;
    /* a PIC32MX's: AD1PCFG, the change notices, each pin's CN number */
    struct rf_reg *ad1pcfg, *cncon, *cnen, *cnpue;
    signed char cn[NPORTS][32];
    int cn_irq;
    int port_cn_irq;                        /* a dsPIC33E's or PIC24E's CN */
    /* a PIC24's peripherals on its pins */
    struct rf_reg *rpor[32], *rpinr[64];    /* the PPS's */
    signed char rp_of[NPORTS][32];          /* a pin's RPn, or -1 */
    short pin_of_rp[64];                    /* RPn's or RPIn's pin, or -1 */
    signed char fn[64];                     /* each output function's level */
    signed char own[NPORTS][32];            /* a peripheral's own pin's */
    int driven;                             /* any of them is not -1 */
    struct { void (*fn)(void *ctx, u64 when); void *ctx; } watch[8];
    int nwatch;
};

/* the level a peripheral drives the pin at, or -1 */
static int periph_level(const struct gpio *g, int i, int b)
{
    if (!g->driven)
        return -1;
    if (g->own[i][b] >= 0)
        return g->own[i][b];
    int rp = g->rp_of[i][b];
    const struct rf_reg *r = rp >= 0 ? g->rpor[rp / 2] : 0;
    int f = r ? (int)(r->value >> (rp & 1 ? 8 : 0) & 0x3f) : 0;
    return f ? g->fn[f] : -1;
}

/* the bits driven high (outputs at 1, not an open drain let go, or a
 * peripheral's at 1) and driven low (outputs at 0, or a peripheral's) */
static u32 drives(const struct gpio *g, int i, u32 *low)
{
    const struct port *q = &g->pt[i];
    u32 out = ~(u32)q->tris->value & q->pins;
    u32 lat = (u32)q->lat->value;
    u32 od = q->odc ? (u32)q->odc->value : 0;
    u32 hi = out & lat & ~od;
    *low = out & ~lat;
    for (int b = 0; g->driven && b < 32; b++) {
        int pl = (q->pins >> b & 1) ? periph_level(g, i, b) : -1;
        if (pl >= 0) {
            u32 m = 1u << b;
            hi = (hi & ~m) | (pl == 1 ? m : 0);
            *low = (*low & ~m) | (pl == 0 ? m : 0);
        }
    }
    return hi;
}

/* a PIC32MX's pull-ups on port i: CNPUE's bits, by the pins' CN numbers */
static u32 cn_bits(const struct gpio *g, const struct rf_reg *r, int i)
{
    u32 v = 0;
    for (int b = 0; r && b < 32; b++)
        if (g->cn[i][b] >= 0 && (r->value >> g->cn[i][b] & 1))
            v |= 1u << b;
    return v;
}

static u32 pins_read(struct gpio *g, int i)
{
    struct port *q = &g->pt[i];
    struct board_io *io = &g->s->io;
    u32 tris = (u32)q->tris->value, lat = (u32)q->lat->value;
    u32 od = q->odc ? (u32)q->odc->value : 0;
    u32 an = q->ansel ? (u32)q->ansel->value : 0;
    u32 up = q->cnpu ? (u32)q->cnpu->value : cn_bits(g, g->cnpue, i), v = 0;
    if (i == 1 && g->ad1pcfg)
        an = ~(u32)g->ad1pcfg->value & 0xffff;   /* port B: 1 is digital */
    for (int b = 0; b < 32; b++) {
        u32 m = 1u << b;
        int level = 0, pl;
        if (!(q->pins & m) || (an & m))
            continue;
        pl = periph_level(g, i, b);
        if (pl == 0 || pl == 1) {
            level = pl;
        } else if (pl < 0 && !(tris & m) && !((od & m) && (lat & m))) {
            level = (lat & m) != 0;
        } else if (q->bus & m) {
            level = !(q->bus_low & m);
        } else if (!(io->pin_in && io->pin_in(io->ctx, i, b, &level))) {
            level = (up & m) != 0;
        }
        if (level)
            v |= m;
    }
    return v;
}

static void tell_board(struct gpio *g)
{
    struct board_io *io = &g->s->io;
    for (int i = 0; i < NPORTS; i++) {
        struct port *q = &g->pt[i];
        if (!q->port)
            continue;
        u32 lo, hi = drives(g, i, &lo), ch = (hi ^ q->high) | (lo ^ q->low);
        q->high = hi;
        q->low = lo;
        for (int b = 0; ch && b < 32; b++)
            if ((ch >> b & 1) && io->pin_out)
                io->pin_out(io->ctx, i, b, hi >> b & 1 ? 1 : lo >> b & 1 ? 0 : -1);
    }
}

/* the interrupt-on-change: each port's pins against their last levels
 * (a PIC24's IOC, a PIC32MX's change notices) */
static void ioc_check(struct gpio *g)
{
    int on = g->padcon && (g->padcon->value >> 15 & 1);
    int cn_on = g->cncon && (g->cncon->value >> 15 & 1);
    for (int i = 0; i < NPORTS; i++) {
        struct port *q = &g->pt[i];
        if (!q->port || (!q->iocf && !g->cncon && !q->cnen))
            continue;
        u32 lv = pins_read(g, i), was = q->level;
        q->level = lv;
        if (cn_on && ((lv ^ was) & cn_bits(g, g->cnen, i)) && g->cn_irq >= 0)
            pic32_irq(g->s, g->cn_irq);
        if (q->cnen && ((lv ^ was) & (u32)q->cnen->value) && g->port_cn_irq >= 0)
            pic24_irq(g->s, g->port_cn_irq);
        if (!on || !q->iocf)
            continue;
        u32 ch = (lv & ~was & (q->iocp ? (u32)q->iocp->value : 0)) |
                 (~lv & was & (q->iocn ? (u32)q->iocn->value : 0));
        if (!ch)
            continue;
        rf_hw(q->iocf, q->iocf->value | ch);
        if (g->iocstat)
            rf_hw(g->iocstat, g->iocstat->value | 1u << i);
        if (g->ioc_irq >= 0)
            pic24_irq(g->s, g->ioc_irq);
    }
}

/* the pins may have changed: the board told, the interrupt-on-change
 * and the watchers looking */
static void changed(struct gpio *g, u64 when)
{
    tell_board(g);
    ioc_check(g);
    for (int i = 0; i < g->nwatch; i++)
        g->watch[i].fn(g->watch[i].ctx, when);
}

static int port_of(struct gpio *g, u32 off)
{
    for (int i = 0; i < NPORTS; i++)
        if (g->pt[i].port && off == rf_off(g->pt[i].port))
            return i;
    return -1;
}

static u32 gpio_read(void *ctx, u32 off, int n)
{
    struct gpio *g = ctx;
    int i = port_of(g, off);
    if (i >= 0) {
        struct rf_reg *r = g->pt[i].port;
        u64 v = (r->value & ~(u64)g->pt[i].pins) | pins_read(g, i);
        if (v != r->value)
            rf_hw(r, v);
    }
    return rf_read(g->p, off, n);
}

static void gpio_write(void *ctx, u32 off, int n, u32 v)
{
    struct gpio *g = ctx;
    int i = port_of(g, off);
    if (i >= 0) {
        /* PORTx written is LATx written */
        struct rf_reg *lat = g->pt[i].lat;
        rf_write(g->p, rf_off(lat), n, v);
    } else {
        int f = -1;
        for (int k = 0; k < NPORTS; k++)
            if (g->pt[k].iocf && off == rf_off(g->pt[k].iocf))
                f = k;
        if (f >= 0) {
            /* IOCFx: its bits cleared, never set, by the program */
            struct rf_reg *r = g->pt[f].iocf;
            u64 old = r->value;
            rf_write(g->p, off, n, v);
            rf_hw(r, r->value & old);
            if (g->iocstat && !r->value)
                rf_hw(g->iocstat, g->iocstat->value & ~(1u << f));
        } else {
            rf_write(g->p, off, n, v);
        }
    }
    changed(g, g->s->cycles);
}

static void gpio_reset(void *ctx)
{
    struct gpio *g = ctx;
    memset(g->fn, -1, sizeof g->fn);
    memset(g->own, -1, sizeof g->own);
    g->driven = 0;
    for (int i = 0; i < NPORTS; i++) {
        g->pt[i].high = g->pt[i].low = 0;
        g->pt[i].level = g->pt[i].port ? pins_read(g, i) : 0;
    }
}

const struct dev_ops pic32_gpio_ops = {
    "pic32-gpio", gpio_read, gpio_write, gpio_reset, 0, 0,
};

void *pic32_gpio_create(struct sim *s, struct rf_periph *p)
{
    struct gpio *g = calloc(1, sizeof *g);
    char nm[16];
    int any = 0;
    if (!g)
        die("out of memory");
    g->s = s;
    g->p = p;
    for (int i = 0; i < NPORTS; i++) {
        struct port *q = &g->pt[i];
        char l = (char)('A' + i + (i >= 8));    /* no port I */
/* one statement: `if (!q->ansel) REG(ansel, "ANS")` guarded only the
 * snprintf, and the assignment always ran (GCC's -Wmultistatement-macros) */
#define REG(f, name) do { snprintf(nm, sizeof nm, name "%c", l); \
                          q->f = rf_reg(p, nm); } while (0)
        REG(ansel, "ANSEL");
        if (!q->ansel)
            REG(ansel, "ANS");                  /* a PIC24's */
        REG(tris, "TRIS");
        REG(port, "PORT");
        REG(lat, "LAT");
        REG(odc, "ODC");
        REG(cnpu, "CNPU");
        if (!q->cnpu)
            REG(cnpu, "IOCPU");                 /* a PIC24's */
        REG(iocp, "IOCP");
        REG(iocn, "IOCN");
        REG(iocf, "IOCF");
        if (pic24_is(s->cpu))
            REG(cnen, "CNEN");
#undef REG
        if (!q->port || !q->tris || !q->lat) {
            q->port = 0;
            continue;
        }
        any = 1;
        q->pins = (u32)rf_fields(q->port);
        rf_set_reset(q->tris, rf_fields(q->tris));
        if (q->ansel)
            rf_set_reset(q->ansel, rf_fields(q->ansel));
    }
    if (!any) {
        free(g);
        return 0;
    }
    struct rf_periph *pad = s->svd ? rf_periph(s->svd, "PAD") : 0;
    g->padcon = pad ? rf_reg(pad, "PADCON") : 0;
    g->iocstat = pad ? rf_reg(pad, "IOCSTAT") : 0;
    g->ioc_irq = g->padcon ? pic24_irq_named(s, "IOC") : -1;
    g->port_cn_irq = pic24_is(s->cpu) ? pic24_irq_named(s, "CN") : -1;
    /* a PIC32MX's AD1PCFG and change notices */
    struct rf_periph *adc = s->svd ? rf_periph(s->svd, "ADC10") : 0;
    g->ad1pcfg = adc && !g->pt[1].ansel ? rf_reg(adc, "AD1PCFG") : 0;
    g->cncon = rf_reg(p, "CNCON");
    g->cnen = rf_reg(p, "CNEN");
    g->cnpue = rf_reg(p, "CNPUE");
    memset(g->cn, -1, sizeof g->cn);
    for (int k = 0; g->cncon && k < 32; k++) {
        snprintf(nm, sizeof nm, "CN%d", k);
        int pin = rf_pin(s->svd, nm);
        if (pin >= 0 && pin / 32 < NPORTS)
            g->cn[pin / 32][pin % 32] = (signed char)k;
    }
    g->cn_irq = g->cncon ? pic32_irq_named(s, "CHANGE_NOTICE") : -1;
    /* the PPS's pins: RPn (input and output) and RPIn (input) */
    struct rf_periph *pps = s->svd ? rf_periph(s->svd, "PPS") : 0;
    memset(g->rp_of, -1, sizeof g->rp_of);
    memset(g->fn, -1, sizeof g->fn);
    memset(g->own, -1, sizeof g->own);
    for (int k = 0; k < 64; k++) {
        int pin;
        g->pin_of_rp[k] = -1;
        snprintf(nm, sizeof nm, "RP%d", k);
        if ((pin = rf_pin(s->svd, nm)) >= 0 && pin / 32 < NPORTS) {
            g->pin_of_rp[k] = (short)pin;
            g->rp_of[pin / 32][pin % 32] = (signed char)k;
        } else {
            snprintf(nm, sizeof nm, "RPI%d", k);
            if ((pin = rf_pin(s->svd, nm)) >= 0 && pin / 32 < NPORTS)
                g->pin_of_rp[k] = (short)pin;
        }
        snprintf(nm, sizeof nm, "RPINR%d", k);
        g->rpinr[k] = pps ? rf_reg(pps, nm) : 0;
        if (k < 32) {
            snprintf(nm, sizeof nm, "RPOR%d", k);
            g->rpor[k] = pps ? rf_reg(pps, nm) : 0;
        }
    }
    return g;
}

/* ---- a PIC24's peripherals on its pins ------------------------------------ */

static struct gpio *gpio_of(struct sim *s)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "GPIO") : 0;
    return p && p->ops == &pic32_gpio_ops ? p->ctx : 0;
}

int pic24_pin(struct sim *s, const char *name)
{
    return s->svd ? rf_pin(s->svd, name) : -1;
}

int pic24_pin_level(struct sim *s, int pin)
{
    struct gpio *g = gpio_of(s);
    if (!g || pin < 0 || pin / 32 >= NPORTS || !g->pt[pin / 32].port)
        return 0;
    return (int)(pins_read(g, pin / 32) >> (pin % 32) & 1);
}

int pic24_pin_of_input(struct sim *s, const char *field)
{
    struct gpio *g = gpio_of(s);
    for (int k = 0; g && k < 64; k++) {
        u64 m = g->rpinr[k] ? rf_mask(g->rpinr[k], field) : 0;
        if (!m)
            continue;
        u64 v = g->rpinr[k]->value & m;
        while (!(m & 1)) {
            m >>= 1;
            v >>= 1;
        }
        return v < 64 ? g->pin_of_rp[v] : -1;
    }
    return -1;
}

static void drive(struct gpio *g, signed char *at, int level, u64 when)
{
    level = level < 0 ? -1 : level == PIC24_PIN_RELEASED ? level : level != 0;
    if (*at == level)
        return;
    *at = (signed char)level;
    if (level >= 0)
        g->driven = 1;
    changed(g, when);
}

void pic24_pps_out(struct sim *s, int fn, int level, u64 when)
{
    struct gpio *g = gpio_of(s);
    if (g && fn > 0 && fn < 64)
        drive(g, &g->fn[fn], level, when);
}

void pic24_pin_out(struct sim *s, int pin, int level, u64 when)
{
    struct gpio *g = gpio_of(s);
    if (g && pin >= 0 && pin / 32 < NPORTS)
        drive(g, &g->own[pin / 32][pin % 32], level, when);
}

void pic24_pin_bus(struct sim *s, int pin, int low)
{
    struct gpio *g = gpio_of(s);
    if (!g || pin < 0 || pin / 32 >= NPORTS || !g->pt[pin / 32].port)
        die("a bus line on pin %d, which the part's ports have not", pin);
    struct port *q = &g->pt[pin / 32];
    u32 m = 1u << (pin % 32);
    q->bus |= m;
    q->bus_low = low ? q->bus_low | m : q->bus_low & ~m;
}

int pic24_pin_drive(struct sim *s, int pin)
{
    struct gpio *g = gpio_of(s);
    if (!g || pin < 0 || pin / 32 >= NPORTS || !g->pt[pin / 32].port)
        return -1;
    u32 low, high = drives(g, pin / 32, &low), m = 1u << (pin % 32);
    return high & m ? 1 : low & m ? 0 : -1;
}

void pic24_pin_watch(struct sim *s, void (*fn)(void *ctx, u64 when), void *ctx)
{
    struct gpio *g = gpio_of(s);
    if (!g)
        return;
    for (int i = 0; i < g->nwatch; i++)
        if (g->watch[i].fn == fn && g->watch[i].ctx == ctx)
            return;
    if (g->nwatch == (int)(sizeof g->watch / sizeof g->watch[0]))
        die("too many models watching the PIC24's pins");
    g->watch[g->nwatch].fn = fn;
    g->watch[g->nwatch++].ctx = ctx;
}

void pic24_pins_changed(struct sim *s)
{
    struct gpio *g = gpio_of(s);
    if (g)
        changed(g, s->cycles);
}
