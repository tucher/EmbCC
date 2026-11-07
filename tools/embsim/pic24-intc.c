/* pic24-intc.c -- a PIC24F's interrupt controller, over its registers in
 * the EDC (INTCON1, INTCON2, INTCON4, IFSx, IECx, IPCx, INTTREG),
 * driving the core (struct pic24_intc).
 *
 *   - Interrupt n is requested while its flag (IFS<n/16>, bit n%16) and
 *     its enable (IEC) are set and its priority (IPC<n/4>, three bits at
 *     4*(n%4)) is not 0, and INTCON2.GIE is set. The highest priority is
 *     presented to the core, then the lowest number (the natural order);
 *     the core takes it when it is above its own priority.
 *   - Taking it, INTTREG says which: VECNUM (the vector number, n + 8)
 *     and ILR (the new priority).
 *   - The core's traps set INTCON1's status bits (OSCFAIL, ADDRERR,
 *     STKERR, MATHERR) and INTCON4.SGHT for a hard trap, a dsPIC33E's
 *     INTCON3.DOOVR for its soft trap (a DO stack overflow); the program
 *     clears them in its handler. Of the math error trap's conditions,
 *     a divide by zero sets DIV0ERR and an accumulator shifted too far
 *     SFTACERR; an accumulator's overflow is one only with OVATE or
 *     OVBTE (then OVAERR, OVBERR), a catastrophic one with COVTE
 *     (COVAERR, COVBERR).
 *   - INTCON1.NSTDIS: an interrupt runs at priority 7, so none nests.
 * A model raises its interrupt with pic24_irq; the program clears the
 * flag. Writing a flag sets it too, as on the part.
 *
 * Not modelled: the external interrupt pins (INTx and their edges),
 * the alternate vector table (AIVTEN), the software trap (SWTRAP). */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

struct intc {
    struct sim *s;
    struct rf_periph *p;
    struct rf_reg *intcon1, *intcon2, *intcon3, *intcon4, *inttreg;
    struct rf_reg *ifs[16], *iec[16], *ipc[64];
    int nw;
    struct pic24_intc ic;
    /* told of every interrupt event and sync event (the DMA's triggers,
     * the capture and compare modules' sync sources) */
    struct { void (*fn)(void *ctx, int ev, u64 when); void *ctx; } listen[8];
    int nlisten;
    /* the request presented, as last worked out: it changes only with a
     * flag, an enable, a priority or GIE, which every write here and every
     * flag a model raises makes unknown */
    int req_known, req_best, req_irq;
};

static int request(void *ctx, int *irq);

/* the controller the core has attached (pic24_intc_attach), or 0 */
static struct intc *intc_of(struct sim *s)
{
    struct pic24_intc *ic = pic24_is(s->cpu) ? ((struct pic24_state *)s->cpu)->intc : 0;
    return ic && ic->request == request ? ic->ctx : 0;
}

static int prio(const struct intc *c, int irq)
{
    const struct rf_reg *r = c->ipc[irq / 4];
    return r ? (int)(r->value >> (4 * (irq % 4)) & 7) : 0;
}

static int request(void *ctx, int *irq)
{
    struct intc *c = ctx;
    if (c->req_known) {
        *irq = c->req_irq;
        return c->req_best;
    }
    c->req_known = 1;
    c->req_best = 0;
    if (c->intcon2 && !(c->intcon2->value & 0x8000))
        return 0;
    int best = 0;
    for (int w = 0; w < c->nw; w++) {
        u32 pend = (u32)(c->ifs[w]->value & c->iec[w]->value) & 0xffff;
        while (pend) {
            int bit = 0;
            while (!(pend >> bit & 1))
                bit++;
            pend &= pend - 1;
            int n = 16 * w + bit, pr = prio(c, n);
            if (pr > best) {
                best = pr;
                *irq = n;
            }
        }
    }
    c->req_best = best;
    c->req_irq = best ? *irq : 0;
    return best;
}

static void taken(void *ctx, int irq, int ipl)
{
    struct intc *c = ctx;
    if (c->inttreg)
        rf_hw(c->inttreg, (u64)((u32)(ipl & 15) << 8 | (u32)((irq + 8) & 0xff)));
}

static void trap(void *ctx, int n)
{
    static const u16 bit[8] = { 0x0002, 0x0008, 0, 0x0004, 0x0010, 0, 0, 0 };
    struct intc *c = ctx;
    if (bit[n] && c->intcon1)
        rf_hw(c->intcon1, c->intcon1->value | bit[n]);
    if (n == TRAP_HARD && c->intcon4)
        rf_hw(c->intcon4, c->intcon4->value | 1);
    if (n == TRAP_SOFT && c->intcon3)
        rf_hw(c->intcon3, c->intcon3->value | rf_mask(c->intcon3, "DOOVR"));
}

static int math(void *ctx, int why)
{
    static const char *const err[] = {
        "DIV0ERR", "OVAERR", "OVBERR", "COVAERR", "COVBERR", "SFTACERR" };
    static const char *const en[] = { 0, "OVATE", "OVBTE", "COVTE", "COVTE", 0 };
    struct intc *c = ctx;
    if (!c->intcon1)
        return !en[why];
    if (en[why] && !(c->intcon1->value & rf_mask(c->intcon1, en[why])))
        return 0;
    rf_hw(c->intcon1, c->intcon1->value | rf_mask(c->intcon1, err[why]));
    return 1;
}

static int nstdis(void *ctx)
{
    struct intc *c = ctx;
    return c->intcon1 && (c->intcon1->value & 0x8000);
}

static u32 intc_read(void *ctx, u32 off, int n)
{
    struct intc *c = ctx;
    return rf_read(c->p, off, n);
}

static void intc_write(void *ctx, u32 off, int n, u32 v)
{
    struct intc *c = ctx;
    rf_write(c->p, off, n, v);
    c->req_known = 0;
}

static void intc_reset(void *ctx)
{
    struct intc *c = ctx;
    c->req_known = 0;
}

const struct dev_ops pic24_intc_ops = {
    "pic24-intc", intc_read, intc_write, intc_reset, 0, 0,
};

void pic24_irq(struct sim *s, int irq)
{
    pic24_irq_at(s, irq, s->cycles);
}

void pic24_irq_at(struct sim *s, int irq, u64 when)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return;
    struct rf_reg *r = c->ifs[irq / 16];
    if (!(r->value >> (irq % 16) & 1)) {
        rf_hw(r, r->value | 1u << (irq % 16));
        c->req_known = 0;
    }
    pic24_event(s, irq, when);
}

void pic24_event(struct sim *s, int ev, u64 when)
{
    struct intc *c = intc_of(s);
    for (int i = 0; c && i < c->nlisten; i++)
        c->listen[i].fn(c->listen[i].ctx, ev, when);
}

void pic24_irq_listen(struct sim *s, void (*fn)(void *ctx, int ev, u64 when), void *ctx)
{
    struct intc *c = intc_of(s);
    if (!c)
        return;
    for (int i = 0; i < c->nlisten; i++)
        if (c->listen[i].ctx == ctx && c->listen[i].fn == fn)
            return;                 /* a reset's: once is enough */
    if (c->nlisten == (int)(sizeof c->listen / sizeof c->listen[0]))
        die("too many models listening to the PIC24's interrupts");
    c->listen[c->nlisten].fn = fn;
    c->listen[c->nlisten++].ctx = ctx;
}

int pic24_event_named(struct sim *s, const char *name)
{
    size_t l = strlen(name);
    if (l > 4 && !strcmp(name + l - 4, "sync")) {
        int k = atoi(name + (name[0] == 'O' ? 2 : 3));
        return (name[0] == 'O' ? PIC24_EV_OC_SYNC : PIC24_EV_CCP_SYNC) + k;
    }
    return pic24_irq_named(s, name);
}

int pic24_irq_flag(struct sim *s, int irq)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return 0;
    return (int)(c->ifs[irq / 16]->value >> (irq % 16) & 1);
}

int pic24_irq_enabled(struct sim *s, int irq)
{
    struct intc *c = intc_of(s);
    if (!c || irq < 0 || irq / 16 >= c->nw)
        return 0;
    return (int)(c->iec[irq / 16]->value >> (irq % 16) & 1);
}

int pic24_irq_named(struct sim *s, const char *name)
{
    return s->svd ? rf_dev_irq(s->svd, name) : -1;
}

void *pic24_intc_create(struct sim *s, struct rf_periph *p)
{
    if (!pic24_is(s->cpu))
        die("the PIC24 interrupt controller is for the PIC24 core");
    struct intc *c = calloc(1, sizeof *c);
    if (!c)
        die("out of memory");
    c->s = s;
    c->p = p;
    c->intcon1 = rf_reg(p, "INTCON1");
    c->intcon2 = rf_reg(p, "INTCON2");
    c->intcon3 = rf_reg(p, "INTCON3");
    /* the trap flags a dsPIC33E's EDC makes read-only are the program's to
     * clear (the family reference manual's INTCON1, DS70000600: R/W-0) */
    if (c->intcon1) {
        static const char *const flag[] = { "OVAERR", "OVBERR", "COVAERR", "COVBERR",
                                            "SFTACERR", "DIV0ERR" };
        for (int k = 0; k < 6; k++)
            if (rf_mask(c->intcon1, flag[k]))
                rf_set_acc(c->intcon1, rf_mask(c->intcon1, flag[k]), ACC_RW);
    }
    c->intcon4 = rf_reg(p, "INTCON4");
    c->inttreg = rf_reg(p, "INTTREG");
    char nm[16];
    for (int w = 0; w < 16; w++) {
        snprintf(nm, sizeof nm, "IFS%d", w);
        c->ifs[w] = rf_reg(p, nm);
        snprintf(nm, sizeof nm, "IEC%d", w);
        c->iec[w] = rf_reg(p, nm);
        if (!c->ifs[w] || !c->iec[w])
            break;
        c->nw = w + 1;
    }
    for (int k = 0; k < 64; k++) {
        snprintf(nm, sizeof nm, "IPC%d", k);
        c->ipc[k] = rf_reg(p, nm);
    }
    if (!c->nw)
        die("the PIC24's INT peripheral has no IFS0 and IEC0");
    c->ic.request = request;
    c->ic.taken = taken;
    c->ic.trap = trap;
    c->ic.math = math;
    c->ic.nstdis = nstdis;
    c->ic.ctx = c;
    pic24_intc_attach(s, &c->ic);
    return c;
}
