/* pic24-tmr.c -- a PIC24FJ's Timer1 to Timer5, over the EDC's registers
 * (TMRx, PRx, TxCON, TMR3HLD, TMR5HLD: one peripheral, TMR).
 *
 *   - TMRx counts while TxCON.TON is set, one count per prescale (TCKPS:
 *     1, 8, 64, 256) instruction cycles;
 *   - when it has reached PRx it goes back to 0 on the next count and the
 *     timer's interrupt flag is set (Tx); a TMRx above PRx counts on to
 *     0xFFFF and wraps, without the flag;
 *   - T32 in T2CON (T4CON) pairs Timer2 and Timer3 (Timer4 and Timer5)
 *     into a 32-bit timer: TMR2 and PR2 the low half, TMR3 and PR3 the
 *     high, the flag Timer3's (Timer5's), T3CON (T5CON) not used. A read
 *     of TMR2 (TMR4) copies TMR3 (TMR5) to TMR3HLD (TMR5HLD), as the part
 *     latches it;
 *   - a write to TMRx or TxCON clears the prescaler's count; PRx is
 *     0xFFFF out of reset.
 * TMRx is current whenever the program can read it: between period
 * matches, where nothing but the count changes, the model's ticks may wait
 * (rf_lag). Not modelled: the external
 * clocks (TCS: the timer stops, with no clock on its pin), gated
 * accumulation, Timer1's asynchronous mode. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NT 5

struct t {
    struct rf_reg *con, *tmr, *pr, *hld;
    int irq;
    u32 acc;                        /* cycles toward the next count */
};

struct tmrs {
    struct sim *s;
    struct rf_periph *p;
    struct t t[NT + 1];             /* 1 to 5 */
    int running;
};

static const u32 prescale[4] = { 1, 8, 64, 256 };

/* timer n (2 or 4) and n + 1 as one */
static int t32(const struct tmrs *m, int n)
{
    return (n == 2 || n == 4) && m->t[n + 1].con && (m->t[n].con->value >> 3 & 1);
}

static int slave(const struct tmrs *m, int n)
{
    return (n == 3 || n == 5) && t32(m, n - 1);
}

static int on(const struct tmrs *m, int n)
{
    const struct t *t = &m->t[n];
    return t->con && (t->con->value >> 15 & 1) && !(t->con->value >> 1 & 1) &&
           !slave(m, n);
}

static u32 divisor(const struct t *t)
{
    return prescale[t->con->value >> 4 & 3];
}

static u64 count_of(const struct tmrs *m, int n)
{
    if (t32(m, n))
        return (m->t[n].tmr->value & 0xffff) | (m->t[n + 1].tmr->value & 0xffff) << 16;
    return m->t[n].tmr->value & 0xffff;
}

static u64 period_of(const struct tmrs *m, int n)
{
    if (t32(m, n))
        return (m->t[n].pr->value & 0xffff) | (m->t[n + 1].pr->value & 0xffff) << 16;
    return m->t[n].pr->value & 0xffff;
}

static void set_count(struct tmrs *m, int n, u64 c)
{
    m->t[n].tmr->value = c & 0xffff;
    if (t32(m, n))
        m->t[n + 1].tmr->value = c >> 16 & 0xffff;
}

static void advance(struct tmrs *m, int n, u64 ticks)
{
    u64 c = count_of(m, n), pr = period_of(m, n);
    u64 top = t32(m, n) ? 0xffffffffull : 0xffff;
    int flag = 0;
    if (c > pr) {
        u64 room = top - c + 1;
        if (ticks < room) {
            set_count(m, n, c + ticks);
            return;
        }
        ticks -= room;
        c = 0;
    }
    u64 to_match = pr - c + 1;
    if (ticks >= to_match) {
        flag = 1;
        ticks -= to_match;
        c = ticks % (pr + 1);
    } else
        c += ticks;
    set_count(m, n, c);
    if (flag)
        pic24_irq(m->s, t32(m, n) ? m->t[n + 1].irq : m->t[n].irq);
}

static void run_clock(struct tmrs *m)
{
    int any = 0;
    for (int n = 1; n <= NT; n++)
        any |= on(m, n);
    rf_clock(m->p, &m->running, any);
}

static void tmr_tick(void *ctx, u32 cycles)
{
    struct tmrs *m = ctx;
    for (int n = 1; n <= NT; n++) {
        if (!on(m, n))
            continue;
        struct t *t = &m->t[n];
        u32 d = divisor(t);
        u64 a = (u64)t->acc + cycles;
        t->acc = (u32)(a % d);
        if (a >= d)
            advance(m, n, a / d);
    }
}

/* the cycles until running timer n next matches its period */
static u64 to_match(const struct tmrs *m, int n)
{
    u64 c = count_of(m, n), pr = period_of(m, n);
    u64 top = t32(m, n) ? 0xffffffffull : 0xffff;
    u64 ticks = c <= pr ? pr - c + 1 : top - c + 1 + pr + 1;
    return ticks * divisor(&m->t[n]) - m->t[n].acc;
}

/* the first match of the running timers, those whose interrupt is enabled
 * (`enabled_only`) or all */
static int first_match(const struct tmrs *m, int enabled_only, u32 *cycles)
{
    u64 best = ~0ull;
    for (int n = 1; n <= NT; n++) {
        if (!on(m, n))
            continue;
        int irq = t32(m, n) ? m->t[n + 1].irq : m->t[n].irq;
        if (enabled_only && !pic24_irq_enabled(m->s, irq))
            continue;
        u64 cyc = to_match(m, n);
        if (cyc < best)
            best = cyc;
    }
    if (best == ~0ull)
        return 0;
    *cycles = best > 0xffffffffu ? 0xffffffffu : (u32)best;
    return 1;
}

/* when a flag whose interrupt is enabled next sets */
static int tmr_next(void *ctx, u32 *cycles)
{
    return first_match(ctx, 1, cycles);
}

/* when a flag next sets, and the timer's event goes to the models listening */
static int tmr_next_effect(void *ctx, u32 *cycles)
{
    return first_match(ctx, 0, cycles);
}

static u32 tmr_read(void *ctx, u32 off, int n)
{
    struct tmrs *m = ctx;
    for (int k = 2; k <= 4; k += 2)
        if (m->t[k].tmr && off == rf_off(m->t[k].tmr) && m->t[k + 1].hld &&
            m->t[k + 1].tmr && !m->s->bus.debug)
            m->t[k + 1].hld->value = m->t[k + 1].tmr->value;
    return rf_read(m->p, off, n);
}

static void tmr_write(void *ctx, u32 off, int n, u32 v)
{
    struct tmrs *m = ctx;
    rf_write(m->p, off, n, v);
    for (int k = 1; k <= NT; k++) {
        struct t *t = &m->t[k];
        if (t->con && (off == rf_off(t->tmr) || off == rf_off(t->con)))
            t->acc = 0;
    }
    run_clock(m);
}

static void tmr_reset(void *ctx)
{
    struct tmrs *m = ctx;
    for (int n = 1; n <= NT; n++)
        m->t[n].acc = 0;
    m->running = 0;
}

u32 pic24_timer_div(struct sim *s, int n)
{
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "TMR") : 0;
    const struct tmrs *m = p && p->ops == &pic24_tmr_ops ? p->ctx : 0;
    if (!m || n < 1 || n > NT || !m->t[n].con)
        return 0;
    if (slave(m, n))
        n--;                        /* Timer3 (5) counts on Timer2's (4) clock */
    return on(m, n) ? divisor(&m->t[n]) : 0;
}

const struct dev_ops pic24_tmr_ops = {
    "pic24-tmr", tmr_read, tmr_write, tmr_reset, tmr_tick, tmr_next,
};

void *pic24_tmr_create(struct sim *s, struct rf_periph *p)
{
    struct tmrs *m = calloc(1, sizeof *m);
    char nm[16];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    rf_lag(p, tmr_next_effect);
    for (int n = 1; n <= NT; n++) {
        struct t *t = &m->t[n];
        snprintf(nm, sizeof nm, "T%dCON", n);
        t->con = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "TMR%d", n);
        t->tmr = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "PR%d", n);
        t->pr = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "TMR%dHLD", n);
        t->hld = rf_reg(p, nm);
        if (!t->con || !t->tmr || !t->pr) {
            memset(t, 0, sizeof *t);
            continue;
        }
        any = 1;
        snprintf(nm, sizeof nm, "T%d", n);
        t->irq = pic24_irq_named(s, nm);
    }
    if (!any) {
        free(m);
        return 0;
    }
    return m;
}
