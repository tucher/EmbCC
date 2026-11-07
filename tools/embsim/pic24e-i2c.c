/* pic24e-i2c.c -- a dsPIC33E's or PIC24E's I2C units (I2CxCON, I2CxSTAT,
 * I2CxTRN, I2CxRCV, I2CxBRG, I2CxADD, I2CxMSK: one peripheral, I2C), by the
 * family reference manual's I2C section (DS70000195G), on the bus an
 * endpoint keeps (--i2c-bus, i2c-bus.c), or on a bus with nothing else on
 * it without one.
 *
 *   - The master: SEN, RSEN, PEN, RCEN and ACKEN start their event, and a
 *     byte written to I2CxTRN goes out. A BRG period is I2CxBRG + 2 cycles
 *     and a bit two of them (Equation 4-1): a Start takes two periods, a
 *     Repeated Start and a Stop three, an acknowledge two, a byte sent with
 *     its acknowledge nine bits, a byte received eight. Each ends with
 *     MI2Cx. Writing I2CxTRN sets TBF and TRSTAT, and the byte's end clears
 *     them and writes the slave's acknowledge into ACKSTAT. A write to
 *     I2CxTRN while an event is under way sets IWCOL and is lost, and the
 *     lower five bits of I2CxCON keep their value until it ends. The slave
 *     logic sees the master's own Start (S set, P clear) and Stop.
 *   - The slave: another master's write, from the endpoint, at the rate of
 *     IPMB (100 kHz). Its Start sets S and clears P. Its address byte,
 *     against I2CxADD and I2CxMSK, goes to I2CxRCV with RBF set and D/A and
 *     R/W clear, is acknowledged, and raises SI2Cx; each data byte after it
 *     the same with D/A set. A byte that arrives while RBF is set is not
 *     taken and not acknowledged, and sets I2COV (Table 7-3). With STREN,
 *     the unit holds the clock after each byte (SCLREL cleared) until the
 *     program sets SCLREL. The write ends after the last byte, or after a
 *     byte the part did not acknowledge, with a Stop: P set, S clear.
 *     Another master's write to an address that is not the part's runs to
 *     its end: a slave outside the part answers it.
 *   - I2CEN cleared resets the unit: no event under way, its status bits
 *     clear, SCLREL set. Reading I2CxRCV clears RBF; the program clears
 *     IWCOL, I2COV and BCL, and the rest of I2CxSTAT is the module's.
 * What the part's firmwares do not use ends the run, as "not modelled": a
 * read addressed to the part (slave transmission), 10-bit addresses, a
 * general call, the IPMI mode, another master on the bus while the part's
 * master is active (arbitration, bus collisions), the master writing to the
 * part's own address, a byte written to I2CxTRN with no Start before it,
 * and the unit disabled during another master's write to it. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

#define NU 4

/* I2CxCON */
#define I2CEN   (1u << 15)
#define SCLREL  (1u << 12)
#define IPMIEN  (1u << 11)
#define A10M    (1u << 10)
#define GCEN    (1u << 7)
#define STREN   (1u << 6)
#define ACKDT   (1u << 5)
#define ACKEN   (1u << 4)
#define RCEN    (1u << 3)
#define PEN     (1u << 2)
#define RSEN    (1u << 1)
#define SEN     1u
#define EVENTS  (ACKEN | RCEN | PEN | RSEN | SEN)
#define CON_IMPLEMENTED 0xbfffu     /* bit 14 reads 0 */
/* I2CxSTAT */
#define ACKSTAT (1u << 15)
#define TRSTAT  (1u << 14)
#define BCL     (1u << 10)
#define IWCOL   (1u << 7)
#define I2COV   (1u << 6)
#define D_A     (1u << 5)
#define ST_P    (1u << 4)
#define ST_S    (1u << 3)
#define R_W     (1u << 2)
#define RBF     (1u << 1)
#define TBF     1u
#define PROGRAM_CLEARS (IWCOL | I2COV | BCL)
#define ADDRESS_MASK 0x7fu
#define READ_BIT 1u
#define GENERAL_CALL 0x00u

/* periods of the BRG each master event lasts */
#define START_PERIODS 2
#define RESTART_PERIODS 3
#define STOP_PERIODS 3
#define ACK_PERIODS 2
#define BIT_PERIODS 2
#define SENT_BITS 9                 /* eight and the acknowledge */
#define RECEIVED_BITS 8
#define BRG_OFFSET 2
#define BRG_MINIMUM 2               /* §4.3: smaller values are not supported */
/* another master's clock: IPMB's */
#define OUTSIDE_HZ 100000u
#define OUTSIDE_STOP_BITS 1
#define BUS_BYTES 256

enum { OP_NONE, OP_START, OP_RESTART, OP_STOP, OP_TX, OP_RX, OP_ACK };

struct outside {
    int on;                         /* another master's write is under way */
    int started, held, addressed;
    u64 at;                         /* the core's cycle its next step comes */
    u64 resume;                     /* held: the cycles from the release to the next step */
    u8 b[BUS_BYTES];
    int n, i, acked;
};

struct i2c;

struct unit {
    struct i2c *m;
    int k;                          /* the unit, and the endpoint's bus: I2Ck */
    struct rf_reg *con, *stat, *trn, *rcv, *brg, *add, *msk;
    int mirq, sirq;
    int op;                         /* the master's event under way */
    u32 left;                       /* its cycles to go */
    int active;                     /* between the master's Start and its Stop */
    int ack;                        /* the slave's acknowledge of the byte going out */
    u8 rx;                          /* the byte coming in */
    struct outside o;
};

struct i2c {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int running;
};

static void not_modelled(const struct unit *u, const char *what)
{
    die("I2C%d: not modelled: %s", u->k, what);
}

static u32 con(const struct unit *u)
{
    return (u32)u->con->value;
}

static u32 stat(const struct unit *u)
{
    return (u32)u->stat->value;
}

static void set_stat(struct unit *u, u32 set, u32 clear)
{
    rf_hw(u->stat, (stat(u) & ~clear) | set);
}

static void set_con(struct unit *u, u32 set, u32 clear)
{
    rf_hw(u->con, (con(u) & ~clear) | set);
}

static u32 period(const struct unit *u)
{
    u32 brg = (u32)u->brg->value;
    if (brg < BRG_MINIMUM)
        not_modelled(u, "an I2CxBRG below 2 (§4.3)");
    return brg + BRG_OFFSET;
}

static u64 outside_bit(const struct unit *u)
{
    return pic24_fcy(u->m->s) / OUTSIDE_HZ;
}

static int on_bus(const struct unit *u)
{
    return i2cbus_on(u->m->s);
}

static void clock_state(struct i2c *m)
{
    int any = 0;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        any |= u->con && (u->op != OP_NONE || (u->o.on && !u->o.held));
    }
    rf_clock(m->p, &m->running, any);
}

/* ---- the master -------------------------------------------------------- */

static void begin(struct unit *u, int op, u32 periods)
{
    struct sim *s = u->m->s;
    if (u->o.on)
        not_modelled(u, "a master event while another master's write is under way (§6)");
    u->op = op;
    u->left = periods * period(u);
    switch (op) {
    case OP_START:
        if (on_bus(u))
            i2cbus_start(s, u->k);
        break;
    case OP_RESTART:
        if (on_bus(u))
            i2cbus_restart(s, u->k);
        break;
    case OP_TX: {
        u8 byte = (u8)u->trn->value;
        if (u->active == 1 && ((byte >> 1) & ADDRESS_MASK) == ((u32)u->add->value & ADDRESS_MASK))
            not_modelled(u, "the master writing to the part's own address");
        u->ack = on_bus(u) ? i2cbus_write(s, u->k, byte) : 0;
        u->active = 2;
        break;
    }
    case OP_RX:
        u->rx = on_bus(u) ? i2cbus_read(s, u->k) : 0xff;    /* nothing answers: the pull-ups */
        break;
    case OP_ACK:
        if (on_bus(u))
            i2cbus_master_ack(s, u->k, !(con(u) & ACKDT));
        break;
    }
    clock_state(u->m);
}

static void finish(struct unit *u)
{
    switch (u->op) {
    case OP_START:
    case OP_RESTART:
        set_stat(u, ST_S, ST_P);
        set_con(u, 0, SEN | RSEN);
        u->active = 1;
        break;
    case OP_STOP:
        set_stat(u, ST_P, ST_S);
        set_con(u, 0, PEN);
        u->active = 0;
        if (on_bus(u))
            i2cbus_stop(u->m->s, u->k);
        break;
    case OP_TX:
        set_stat(u, u->ack ? 0 : ACKSTAT, TBF | TRSTAT | (u->ack ? ACKSTAT : 0));
        break;
    case OP_RX:
        if (stat(u) & RBF) {
            set_stat(u, I2COV, 0);
        } else {
            rf_hw(u->rcv, u->rx);
            set_stat(u, RBF, 0);
        }
        set_con(u, 0, RCEN);
        break;
    case OP_ACK:
        set_con(u, 0, ACKEN);
        break;
    }
    u->op = OP_NONE;
    if (u->mirq >= 0)
        pic24_irq(u->m->s, u->mirq);
}

/* ---- the slave: another master's write ------------------------------- */

static void inject(void *ctx, u64 at, const u8 *b, int n)
{
    struct unit *u = ctx;
    if (u->o.on)
        not_modelled(u, "another master's write while one is under way");
    if (n > BUS_BYTES)
        die("I2C%d: a write of %d bytes, more than %d", u->k, n, BUS_BYTES);
    memset(&u->o, 0, sizeof u->o);
    u->o.on = 1;
    u->o.at = at;
    u->o.n = n;
    memcpy(u->o.b, b, (size_t)n);
    clock_state(u->m);
}

/* the slave's part in the byte just clocked in: whether it acknowledges */
static int take(struct unit *u, u8 byte)
{
    u32 c = con(u), st = stat(u);
    if (u->o.i == 0) {
        if (c & A10M)
            not_modelled(u, "10-bit slave addresses");
        if (c & IPMIEN)
            not_modelled(u, "the IPMI mode (every address acknowledged)");
        if ((byte >> 1) == GENERAL_CALL && (c & GCEN))
            not_modelled(u, "a general call");
        u32 mask = (u32)u->msk->value & ADDRESS_MASK;
        u->o.addressed = (c & I2CEN) &&
                         (((byte >> 1) ^ (u32)u->add->value) & ADDRESS_MASK & ~mask) == 0;
        if (!u->o.addressed)
            return 0;
        if (byte & READ_BIT)
            not_modelled(u, "a read addressed to the part (slave transmission)");
    }
    if (!u->o.addressed)
        return 0;
    int ack;
    if (st & RBF) {
        set_stat(u, I2COV, 0);      /* Table 7-3: not taken, not acknowledged */
        ack = 0;
    } else {
        rf_hw(u->rcv, byte);
        ack = !(st & I2COV);
        set_stat(u, RBF | (u->o.i ? D_A : 0), u->o.i ? R_W : (D_A | R_W));
    }
    if (c & STREN) {
        set_con(u, 0, SCLREL);
        u->o.held = 1;
    }
    if (u->sirq >= 0)
        pic24_irq(u->m->s, u->sirq);
    return ack;
}

static void outside_step(struct unit *u)
{
    struct outside *o = &u->o;
    struct sim *s = u->m->s;
    if (!o->started) {
        if (u->op != OP_NONE || u->active)
            not_modelled(u, "another master's write while the part's master is active (§6)");
        o->started = 1;
        if (con(u) & I2CEN)
            set_stat(u, ST_S, ST_P);
        o->at = s->cycles + SENT_BITS * outside_bit(u);
        return;
    }
    if (o->i < o->n) {
        int ack = take(u, o->b[o->i]);
        if (ack)
            o->acked++;
        o->i++;
        /* a write the part's slave was not addressed by, a slave outside the part answers */
        int more = o->i < o->n && (ack || !o->addressed);
        if (!more)
            o->i = o->n;
        u64 next = (more ? SENT_BITS : OUTSIDE_STOP_BITS) * outside_bit(u);
        if (o->held)
            o->resume = next;
        else
            o->at = s->cycles + next;
        return;
    }
    if (con(u) & I2CEN)
        set_stat(u, ST_P, ST_S);
    o->on = 0;
    i2cbus_delivered(s, u->k, o->acked);
}

/* ---- the unit's clock -------------------------------------------------- */

static void i2c_tick(void *ctx, u32 cycles)
{
    struct i2c *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con)
            continue;
        if (u->op != OP_NONE) {
            if (cycles >= u->left)
                finish(u);
            else
                u->left -= cycles;
        }
        if (u->o.on && !u->o.held && m->s->cycles >= u->o.at)
            outside_step(u);
    }
    clock_state(m);
}

static int i2c_next(void *ctx, u32 *cycles)
{
    struct i2c *m = ctx;
    u64 best = ~0ull;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con)
            continue;
        if (u->op != OP_NONE && u->left < best)
            best = u->left;
        if (u->o.on && !u->o.held) {
            u64 to = u->o.at > m->s->cycles ? u->o.at - m->s->cycles : 0;
            if (to < best)
                best = to;
        }
    }
    if (best == ~0ull)
        return 0;
    *cycles = best > 0xffffffffull ? 0xffffffffu : (u32)best;
    return 1;
}

/* ---- the program's side ------------------------------------------------ */

static struct unit *unit_of(struct i2c *m, u32 off, struct rf_reg **which)
{
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con)
            continue;
        struct rf_reg *r[] = { u->con, u->stat, u->trn, u->rcv, u->brg, u->add, u->msk };
        for (size_t j = 0; j < sizeof r / sizeof r[0]; j++)
            if ((off & ~1u) == rf_off(r[j])) {
                *which = r[j];
                return u;
            }
    }
    return 0;
}

static u32 i2c_read(void *ctx, u32 off, int n)
{
    struct i2c *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    u32 v = rf_read(m->p, off, n);
    if (u && r == u->rcv && !m->s->bus.debug)
        set_stat(u, 0, RBF);
    return v;
}

/* a byte write's bits in their word, the other byte's as `rest` */
static u32 widened(u32 off, int n, u32 v, u32 rest)
{
    if (n == 2)
        return v;
    return off & 1 ? ((v & 0xff) << 8) | (rest & 0xff) : (v & 0xff) | (rest & 0xff00);
}

static void reset_unit(struct unit *u)
{
    if (u->o.on && u->o.addressed)
        not_modelled(u, "the unit disabled during another master's write to it");
    u->op = OP_NONE;
    u->active = 0;
    rf_hw(u->stat, 0);
    set_con(u, SCLREL, 0);
}

static void i2c_write(void *ctx, u32 off, int n, u32 v)
{
    struct i2c *m = ctx;
    struct rf_reg *r = 0;
    struct unit *u = unit_of(m, off, &r);
    if (!u) {
        rf_write(m->p, off, n, v);
        return;
    }
    if (r == u->trn) {
        if (!(con(u) & I2CEN))
            return;
        if (u->op != OP_NONE || (con(u) & EVENTS)) {
            set_stat(u, IWCOL, 0);  /* §5.1.1, §5.2.7: the write does not occur */
            return;
        }
        if (!u->active)
            not_modelled(u, "a byte written to I2CxTRN with no Start before it");
        rf_write(m->p, off, n, v);
        set_stat(u, TBF | TRSTAT, 0);
        begin(u, OP_TX, SENT_BITS * BIT_PERIODS);
        return;
    }
    if (r == u->stat) {
        u32 x = widened(off, n, v, 0xffff);
        set_stat(u, 0, ~x & PROGRAM_CLEARS);
        return;
    }
    if (r != u->con) {
        rf_write(m->p, off, n, v);
        return;
    }
    u32 was = con(u);
    u32 now = widened(off, n, v, was) & CON_IMPLEMENTED;
    if (u->op != OP_NONE)       /* §5.2.7 and the others: events do not queue */
        now = (now & ~EVENTS) | (was & EVENTS);
    if (!(now & STREN) && (was & SCLREL))
        now |= SCLREL;          /* Register 3-1: with STREN = 0 the program may only release */
    rf_hw(u->con, now);
    if (u->o.held && (now & SCLREL) && !(was & SCLREL)) {
        u->o.held = 0;
        u->o.at = m->s->cycles + u->o.resume;
        clock_state(m);
    }
    if (!(now & I2CEN)) {
        if (was & I2CEN)
            reset_unit(u);
        return;
    }
    u32 rose = now & ~was;
    if (u->op != OP_NONE)
        return;
    if (rose & SEN)
        begin(u, OP_START, START_PERIODS);
    else if (rose & RSEN)
        begin(u, OP_RESTART, RESTART_PERIODS);
    else if (rose & PEN)
        begin(u, OP_STOP, STOP_PERIODS);
    else if (rose & RCEN)
        begin(u, OP_RX, RECEIVED_BITS * BIT_PERIODS);
    else if (rose & ACKEN)
        begin(u, OP_ACK, ACK_PERIODS);
}

static void i2c_reset(void *ctx)
{
    struct i2c *m = ctx;
    for (int k = 1; k <= NU; k++) {
        m->u[k].op = OP_NONE;
        m->u[k].active = 0;
        memset(&m->u[k].o, 0, sizeof m->u[k].o);
    }
    m->running = 0;
}

const struct dev_ops pic24e_i2c_ops = {
    "pic24e-i2c", i2c_read, i2c_write, i2c_reset, i2c_tick, i2c_next,
};

void *pic24e_i2c_create(struct sim *s, struct rf_periph *p)
{
    struct i2c *m = calloc(1, sizeof *m);
    char nm[16];
    int any = 0;
    if (!m)
        die("out of memory");
    m->s = s;
    m->p = p;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
#define R(f, name) snprintf(nm, sizeof nm, "I2C%d" name, k); u->f = rf_reg(p, nm)
        R(con, "CON"); R(stat, "STAT"); R(trn, "TRN"); R(rcv, "RCV"); R(brg, "BRG");
        R(add, "ADD"); R(msk, "MSK");
#undef R
        if (!u->con || !u->stat || !u->trn || !u->rcv || !u->brg || !u->add || !u->msk) {
            memset(u, 0, sizeof *u);
            continue;
        }
        any = 1;
        u->m = m;
        u->k = k;
        snprintf(nm, sizeof nm, "MI2C%d", k);
        u->mirq = pic24_irq_named(s, nm);
        snprintf(nm, sizeof nm, "SI2C%d", k);
        u->sirq = pic24_irq_named(s, nm);
        rf_set_acc(u->stat, 0xffff, ACC_RO);
        rf_set_acc(u->rcv, 0xffff, ACC_RO);
        rf_set_acc(u->trn, 0xffff, ACC_RW);
        i2cbus_attach(s, k, u, inject);
    }
    if (!any) {
        free(m);
        return 0;
    }
    return m;
}
