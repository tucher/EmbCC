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
 *   - Another master on the bus (§6): its write that comes due while the
 *     part's master holds the bus waits for the part's Stop, as a master
 *     that sees the bus busy does. One whose Start comes with the part's
 *     contends for it: each byte both send is compared bit by bit, most
 *     significant first, and the first that sends a 1 where the other
 *     sends a 0 loses. The part losing sets BCL, clears TBF and TRSTAT and
 *     raises MI2Cx at that bit, and its slave takes the rest of the
 *     winner's write; the other losing goes no further, and the endpoint
 *     is told. A Start while another master's write holds the bus collides:
 *     BCL set, SEN cleared, MI2Cx raised.
 *   - I2CEN cleared resets the unit: no event under way, its status bits
 *     clear, SCLREL set. Reading I2CxRCV clears RBF; the program clears
 *     IWCOL, I2COV and BCL, and the rest of I2CxSTAT is the module's.
 *   - The bus's lines (§2.0), on the pins FPOR's ALTI2Cx selects (ASCLx and
 *     ASDAx when it is 0, DS70616G §29.0): open drains on lines the
 *     endpoint's outside pulls up and may hold low. Enabled, the unit keeps
 *     its pins, TRISx and LATx without effect on them (Register 3-1;
 *     DS30009711C §3.6), and lets them go: its own transfers and clock
 *     stretching are not on the pins, so PORTx reads them at what the
 *     outside holds. Disabled, the
 *     port drives them. The endpoint is told the part's drive on each line
 *     as it changes. The enabled unit sees the outside's Start, SDAx falling
 *     while SCLx is high (S set, P clear: §2.1.1), and its Stop, SDAx rising
 *     while SCLx is high (§2.1.2); a Start of its master while SCLx is low
 *     collides (§6.5); another master's write starts only once both lines
 *     are high (§2.1).
 *   - A bus between parts' units (--wire): its lines are the wired-AND of
 *     the members' drives, each member's outside what the others hold low.
 *     A member's master takes the bus at its Start; the others' slaves see
 *     its Start, Repeated Start and Stop as they complete, and each byte at
 *     its end, any slave's acknowledge counting. A slave that holds SCLx
 *     low suspends the master's next event until it lets go (§6.2). A Start
 *     while another member's master holds the bus collides (§6.5); two
 *     Starts under way together contend byte by byte, the loser setting BCL
 *     at its losing bit and its slave taking the winner's bytes (§6.3, §6.4,
 *     §6.7). Addressed for a read, a slave sets R/W and holds SCLx until the
 *     program lets go (§7.5.1); the master reads I2CxTRN, written or not
 *     (§7.5 shifts its bits out), and clears TBF; its ACK asks for another
 *     byte and holds SCLx again, its NACK ends the read; either raises
 *     SI2Cx with ACKSTAT the master's. The members run at one instruction
 *     clock, stepped together.
 *   - The part's own slave stays armed while its master sends, as the
 *     bridge's transport relies on: a byte to the part's own address is its
 *     slave's, acknowledged and taken as any other master's, and its clock
 *     stretch holds its own master.
 * What the part's firmwares do not use ends the run, as "not modelled": a
 * read addressed to the part by the endpoint (slave transmission), 10-bit addresses, a
 * general call, the IPMI mode, a Repeated Start, Stop, read or acknowledge
 * while another master contends for the bus, the master reading from the
 * part's own address, a byte written to I2CxTRN with no Start before it,
 * the unit disabled during another master's write to it, a Start of its
 * master while SDAx is held low and SCLx is high (§6.3: its first 1 would
 * lose), the lines changing while the part's master or another master's
 * write is on the bus, SCLx clocking after a Start seen on the lines (an
 * address at the pins), the outside holding low a line the port drives
 * high, the endpoint on a bus parts share, and a Repeated Start, Stop, read
 * or acknowledge while another part's master contends. */
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
/* FPOR's ALTI2C1 and ALTI2C2 (DS70616G §29.0: FPOR<5:4>): 0 puts I2Cx on
 * ASCLx and ASDAx; FPOR's address when the part's EDC does not give it */
#define FPOR_ADDR 0xf8000cu
#define ALTI2C_FIRST_BIT 4
#define ALTI2C_UNITS 2
#define PIN_NAME 16
#define DRIVEN_HIGH 1
#define DRIVEN_LOW 0
#define NOT_DRIVEN (-1)
#define WIRE_UNITS 4
/* another master's clock: IPMB's */
#define OUTSIDE_HZ 100000u
#define OUTSIDE_STOP_BITS 1
#define BUS_BYTES 256

enum { OP_NONE, OP_START, OP_RESTART, OP_STOP, OP_TX, OP_RX, OP_ACK };

struct outside {
    int on;                         /* another master's write is under way */
    int started, held;
    int contending;                 /* its Start came with the part's: its bytes go with the master's */
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
    int losing;                     /* the byte going out loses arbitration at its end */
    u8 rx;                          /* the byte coming in */
    struct outside o;
    int pinned;                     /* its pins looked up: scl and sda, or -1 */
    int scl, sda;
    int scl_level, sda_level;       /* the lines' levels, as last looked at */
    int scl_drive, sda_drive;       /* the part's drive on them, as the endpoint was told */
    int scl_out, sda_out;           /* the outside holds them low */
    int pin_start;                  /* a Start seen on the lines, no Stop since */
    struct {
        int i;                      /* bytes of the write so far, its address the first */
        int addressed;              /* that address is the part's */
        int read;                   /* addressed for a read: the slave transmits */
        int holding;                /* it holds SCLx low: its stretch, or a read's wait */
    } sl;
    struct wire *w;                 /* the bus it shares with other parts' units, or 0 */
    struct unit *rival;             /* the member whose master's Start came with this one's */
    int frozen;                     /* its master's event waits for a slave to let SCLx go */
    int awaiting;                   /* its byte waits for the rival's, to go with it */
    int sent;                       /* the bytes its master has sent since its Start */
};

/* a bus between parts' units (--wire) */
struct wire {
    struct unit *m[WIRE_UNITS];
    int n;
    struct unit *master;            /* the member whose master took the bus, to its Stop */
    int shared;                     /* two rivals' equal byte: 1 under way, 2 delivered */
    int shared_ack;                 /* the slaves' acknowledge of it */
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

/* the endpoint's bus: an endpoint is on, and the unit's bus is not one parts share */
static int on_bus(const struct unit *u)
{
    return i2cbus_on(u->m->s) && !u->w;
}

/* the master's event runs: not held by a slave's stretch, nor waiting for its rival's byte */
static int counting(const struct unit *u)
{
    return u->op != OP_NONE && !u->frozen && !u->awaiting;
}

static void clock_state(struct i2c *m)
{
    int any = 0;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        any |= u->con && (counting(u) || (u->o.on && !u->o.held));
    }
    rf_clock(m->p, &m->running, any);
}

/* ---- the bus's lines ---------------------------------------------------- */

static void pins_changed(void *ctx, u64 when);
static void wire_lines(struct wire *w);

/* the unit's pins, by FPOR's ALTI2Cx, once the image is loaded */
static void find_pins(struct unit *u)
{
    struct sim *s = u->m->s;
    char scl[PIN_NAME], sda[PIN_NAME];
    int alternate = 0;
    if (u->pinned)
        return;
    u->pinned = 1;
    if (u->k <= ALTI2C_UNITS && s->svd)
        alternate = !(pic24_prog(s, rf_config_addr(s->svd, "FPOR", FPOR_ADDR)) >>
                      (ALTI2C_FIRST_BIT + u->k - 1) & 1);
    snprintf(scl, sizeof scl, "%sSCL%d", alternate ? "A" : "", u->k);
    snprintf(sda, sizeof sda, "%sSDA%d", alternate ? "A" : "", u->k);
    u->scl = pic24_pin(s, scl);
    u->sda = pic24_pin(s, sda);
    if (u->scl < 0 || u->sda < 0) {
        u->scl = u->sda = -1;
        return;
    }
    u->scl_drive = u->sda_drive = NOT_DRIVEN;
    u->scl_level = pic24_pin_level(s, u->scl);
    u->sda_level = pic24_pin_level(s, u->sda);
    pic24_pin_watch(s, pins_changed, u->m);
}

static int lines_free(const struct unit *u)
{
    return u->scl < 0 || (u->scl_level && u->sda_level);
}

/* the part's drive on the lines, told to the endpoint as it changes */
static void drive_changed(struct unit *u)
{
    struct sim *s = u->m->s;
    int scl = pic24_pin_drive(s, u->scl), sda = pic24_pin_drive(s, u->sda);
    if (scl == u->scl_drive && sda == u->sda_drive)
        return;
    u->scl_drive = scl;
    u->sda_drive = sda;
    if (u->w) {
        wire_lines(u->w);
        return;
    }
    if (!on_bus(u))
        return;
    if ((scl == DRIVEN_HIGH && u->scl_out) || (sda == DRIVEN_HIGH && u->sda_out))
        not_modelled(u, "the outside holding low a line the port drives high");
    i2cbus_lines(s, u->k, scl, sda);
}

static void levels_changed(struct unit *u, int scl, int sda)
{
    int was_scl = u->scl_level, was_sda = u->sda_level;
    u->scl_level = scl;
    u->sda_level = sda;
    if (u->o.on && u->o.started)
        not_modelled(u, "its bus's lines changing during another master's write");
    if (u->w && u->w->master && u->w->master != u && (u->w->master->active || u->w->master->op != OP_NONE))
        not_modelled(u, "its bus's lines changing while another part's master is on the bus");
    if (!(con(u) & I2CEN))
        return;
    if (u->op != OP_NONE || u->active)
        not_modelled(u, "its bus's lines changing while the part's master is on the bus");
    if (scl && was_scl && sda != was_sda) {
        /* §2.1.1, §2.1.2: SDAx changing while SCLx is high */
        set_stat(u, sda ? ST_P : ST_S, sda ? ST_S : ST_P);
        u->pin_start = !sda;
    } else if (scl != was_scl && u->pin_start) {
        not_modelled(u, "SCLx clocking after a Start seen on the lines (an address at the pins)");
    }
}

static void pins_changed(void *ctx, u64 when)
{
    struct i2c *m = ctx;
    (void)when;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con || !u->pinned || u->scl < 0)
            continue;
        drive_changed(u);
        int scl = pic24_pin_level(m->s, u->scl), sda = pic24_pin_level(m->s, u->sda);
        if (scl != u->scl_level || sda != u->sda_level)
            levels_changed(u, scl, sda);
    }
}

/* the outside's hold on the lines, the bus's pull-ups otherwise */
static void set_outside(struct unit *u, int scl_low, int sda_low)
{
    struct sim *s = u->m->s;
    u->scl_out = scl_low;
    u->sda_out = sda_low;
    pic24_pin_bus(s, u->scl, scl_low);
    pic24_pin_bus(s, u->sda, sda_low);
    pic24_pins_changed(s);
}

/* the endpoint: the outside's hold on the lines */
static void outside_lines(void *ctx, int scl_low, int sda_low)
{
    struct unit *u = ctx;
    find_pins(u);
    if (u->w) {
        if (scl_low || sda_low)
            not_modelled(u, "the endpoint holding the lines of a bus parts share");
        return;
    }
    if (u->scl < 0) {
        if (scl_low || sda_low)
            not_modelled(u, "its bus's lines held, on pins the part has not");
        return;
    }
    if ((scl_low && u->scl_drive == DRIVEN_HIGH) || (sda_low && u->sda_drive == DRIVEN_HIGH))
        not_modelled(u, "the outside holding low a line the port drives high");
    set_outside(u, scl_low, sda_low);
}

/* Register 3-1: enabled, the unit takes its pins, an open drain each, let go */
static void take_pins(struct unit *u, int level)
{
    struct sim *s = u->m->s;
    find_pins(u);
    if (u->scl < 0)
        return;
    pic24_pin_out(s, u->scl, level, s->cycles);
    pic24_pin_out(s, u->sda, level, s->cycles);
}

/* ---- the master -------------------------------------------------------- */

static int wire_begin(struct unit *u, int op);
static int wire_held(const struct wire *w);
static void wire_send(struct unit *u);
static int wire_deliver(struct unit *u);
static void wire_started(struct unit *u);
static void wire_stopped(struct unit *u);
static u8 wire_read(struct unit *u);
static void wire_acked(struct unit *u, int ack);
static int take(struct unit *u, u8 byte);

#define NEVER (~0ull)
#define BYTE_BITS 8
#define TOP_BIT 0x80u

/* the bit of `mine` the part's master loses at against `other`'s: 1 to 8, or
 * 0 when the part wins at an earlier bit (or the bytes are the same) */
static int losing_bit(u8 mine, u8 other)
{
    for (int bit = 1; bit <= BYTE_BITS; bit++, mine <<= 1, other <<= 1) {
        if ((mine ^ other) & TOP_BIT)
            return (mine & TOP_BIT) ? bit : 0;
    }
    return 0;
}

/* §6.5: a Start while another master's write holds the bus */
static void start_collides(struct unit *u)
{
    set_stat(u, BCL, 0);
    set_con(u, 0, SEN);
    if (u->mirq >= 0)
        pic24_irq(u->m->s, u->mirq);
}

static void begin(struct unit *u, int op, u32 periods)
{
    struct sim *s = u->m->s;
    struct outside *o = &u->o;
    if (op == OP_START && !u->scl_level && u->scl >= 0) {
        start_collides(u);      /* §6.5: SCLx low before SDAx is driven low */
        return;
    }
    if (op == OP_START && !u->sda_level && u->scl >= 0)
        not_modelled(u, "a Start while SDAx is held low and SCLx is high (§6.3: its first 1 would lose)");
    if (u->w && !wire_begin(u, op))
        return;
    if (o->on && o->contending && op != OP_TX)
        not_modelled(u, "a Repeated Start, Stop, read or acknowledge while another master contends for the bus (§6)");
    if (op == OP_START && o->on) {
        if (o->started) {
            start_collides(u);
            return;
        }
        if (o->at <= s->cycles) {
            o->started = o->contending = 1;
            o->at = NEVER;
        }
    }
    u->op = op;
    u->left = periods * period(u);
    /* §6.2: a slave holding SCLx low suspends the master's clock, the part's own too */
    u->frozen = op != OP_START && (u->w ? wire_held(u->w) : u->sl.holding);
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
        if (u->active == 1 && (byte & READ_BIT) &&
            ((byte >> 1) & ADDRESS_MASK) == ((u32)u->add->value & ADDRESS_MASK))
            not_modelled(u, "the master reading from the part's own address");
        if (o->on && o->contending) {
            int bit = losing_bit(byte, o->b[o->i]);
            if (bit) {
                u->losing = 1;
                u->left = (u32)bit * BIT_PERIODS * period(u);
                break;
            }
            if (byte != o->b[o->i]) {
                o->on = 0;
                i2cbus_beaten(s, u->k, o->acked);
            } else if (++o->i == o->n) {
                not_modelled(u, "another master ending its write while the part's master goes on (§6)");
            }
        }
        u->ack = on_bus(u) ? i2cbus_write(s, u->k, byte) : 0;
        u->active = 2;
        if (u->w)
            wire_send(u);
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
        u->sent = 0;
        u->sl.i = u->sl.addressed = u->sl.read = 0;
        if (u->w)
            wire_started(u);
        break;
    case OP_STOP:
        set_stat(u, ST_P, ST_S);
        set_con(u, 0, PEN);
        u->active = 0;
        if (on_bus(u))
            i2cbus_stop(u->m->s, u->k);
        if (u->w)
            wire_stopped(u);
        break;
    case OP_TX:
        if (u->losing) {
            /* §6.4, §6.7: the bus is the winner's; its byte goes on to the slaves, this one's too */
            struct outside *o = &u->o;
            set_stat(u, BCL, TBF | TRSTAT);
            u->losing = 0;
            u->active = 0;
            if (u->w)
                break;
            o->contending = 0;
            u->sl.i = o->i;
            u->sl.addressed = 0;
            o->at = u->m->s->cycles + (u64)(SENT_BITS - (u32)losing_bit((u8)u->trn->value, o->b[o->i])) *
                                          outside_bit(u);
            if (on_bus(u))
                i2cbus_lost(u->m->s, u->k);
            break;
        }
        if (u->w)
            u->ack = wire_deliver(u);
        /* the part's own slave stays armed while its master sends (the transport's slave
         * handler, i2c1_bus_slave_isr_handler, relies on it): a byte to its address it takes */
        u->ack |= take(u, (u8)u->trn->value);
        u->sent++;
        set_stat(u, u->ack ? 0 : ACKSTAT, TBF | TRSTAT | (u->ack ? ACKSTAT : 0));
        break;
    case OP_RX:
        if (u->w)
            u->rx = wire_read(u);
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
        if (u->w)
            wire_acked(u, !(con(u) & ACKDT));
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
    if (u->w)
        not_modelled(u, "the endpoint writing on a bus parts share");
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

/* the slave holds SCLx low (SCLREL cleared) until the program lets it go */
static void hold_clock(struct unit *u)
{
    set_con(u, 0, SCLREL);
    u->sl.holding = 1;
    if (u->o.on)
        u->o.held = 1;
}

/* the slave's part in the byte just clocked in: whether it acknowledges */
static int take(struct unit *u, u8 byte)
{
    u32 c = con(u), st = stat(u);
    int first = u->sl.i++ == 0;
    if (first) {
        if (c & A10M)
            not_modelled(u, "10-bit slave addresses");
        if (c & IPMIEN)
            not_modelled(u, "the IPMI mode (every address acknowledged)");
        if ((byte >> 1) == GENERAL_CALL && (c & GCEN))
            not_modelled(u, "a general call");
        u32 mask = (u32)u->msk->value & ADDRESS_MASK;
        u->sl.addressed = (c & I2CEN) &&
                          (((byte >> 1) ^ (u32)u->add->value) & ADDRESS_MASK & ~mask) == 0;
        u->sl.read = 0;
        if (!u->sl.addressed)
            return 0;
        if (byte & READ_BIT) {
            if (!u->w)
                not_modelled(u, "a read addressed to the part by the endpoint (slave transmission)");
            u->sl.read = 1;
        }
    }
    if (!u->sl.addressed)
        return 0;
    int ack;
    if (st & RBF) {
        set_stat(u, I2COV, 0);      /* Table 7-3: not taken, not acknowledged */
        ack = 0;
    } else {
        rf_hw(u->rcv, byte);
        ack = !(st & I2COV);
        if (!first)
            set_stat(u, RBF | D_A, R_W);
        else
            set_stat(u, RBF | (u->sl.read ? R_W : 0), D_A | (u->sl.read ? 0 : R_W));
    }
    /* §7.5.1: a read's address waits for the slave's byte, whatever STREN says */
    if ((c & STREN) || u->sl.read)
        hold_clock(u);
    if (u->sirq >= 0)
        pic24_irq(u->m->s, u->sirq);
    return ack;
}

static void outside_step(struct unit *u)
{
    struct outside *o = &u->o;
    struct sim *s = u->m->s;
    if (!o->started) {
        if (!lines_free(u)) {
            o->at = s->cycles + outside_bit(u); /* §2.1: a line is held, the bus is busy */
            return;
        }
        if (u->op == OP_START && !u->active) {
            o->started = o->contending = 1;     /* both Starts at once: they contend */
            o->at = NEVER;
            return;
        }
        if (u->op != OP_NONE || u->active) {
            o->at = s->cycles + outside_bit(u); /* the bus is busy: it waits */
            return;
        }
        o->started = 1;
        u->sl.i = u->sl.addressed = 0;
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
        int more = o->i < o->n && (ack || !u->sl.addressed);
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

/* ---- a bus between parts' units (--wire) -------------------------------- */

/* the lines' wired-AND: each member's outside is what the others hold low */
static void wire_lines(struct wire *w)
{
    for (int i = 0; i < w->n; i++) {
        struct unit *x = w->m[i];
        int scl = 0, sda = 0;
        for (int j = 0; j < w->n; j++) {
            if (w->m[j] == x)
                continue;
            scl |= w->m[j]->scl_drive == DRIVEN_LOW;
            sda |= w->m[j]->sda_drive == DRIVEN_LOW;
        }
        if ((scl && x->scl_drive == DRIVEN_HIGH) || (sda && x->sda_drive == DRIVEN_HIGH))
            not_modelled(x, "another part holding low a line the port drives high");
        if (scl != x->scl_out || sda != x->sda_out)
            set_outside(x, scl, sda);
    }
}

static int wire_held(const struct wire *w)
{
    for (int i = 0; i < w->n; i++)
        if (w->m[i]->sl.holding)
            return 1;
    return 0;
}

/* a slave let SCLx go: the masters it held go on once none holds it */
static void wire_released(struct wire *w)
{
    if (wire_held(w))
        return;
    for (int i = 0; i < w->n; i++)
        if (w->m[i]->frozen) {
            w->m[i]->frozen = 0;
            clock_state(w->m[i]->m);
        }
}

/* whether the master's event goes ahead: a Start takes the bus, contends with another
 * member's Start under way (§6.7), or collides with a master that holds it (§6.5) */
static int wire_begin(struct unit *u, int op)
{
    struct wire *w = u->w;
    if (op != OP_START) {
        if (u->rival && op != OP_TX)
            not_modelled(u, "a Repeated Start, Stop, read or acknowledge while another part's master "
                            "contends for the bus (§6)");
        return 1;
    }
    for (int i = 0; i < w->n; i++)
        if (pic24_fcy(w->m[i]->m->s) != pic24_fcy(u->m->s))
            not_modelled(u, "parts at different instruction clocks on one bus");
    struct unit *holder = w->master;
    if (!holder || holder == u) {
        w->master = u;
        return 1;
    }
    if (holder->op == OP_START && !holder->active) {
        u->rival = holder;
        holder->rival = u;
        return 1;
    }
    start_collides(u);
    return 0;
}

/* §6.3, §6.7: two rivals' bytes go bit by bit together; the one that sends a 1 where the
 * other sends a 0 loses at that bit, and the other's byte goes on */
static void contend(struct unit *a, struct unit *b)
{
    struct wire *w = a->w;
    u8 x = (u8)a->trn->value, y = (u8)b->trn->value;
    a->awaiting = b->awaiting = 0;
    a->left = SENT_BITS * BIT_PERIODS * period(a);
    b->left = SENT_BITS * BIT_PERIODS * period(b);
    if (x != y) {
        struct unit *loser = losing_bit(x, y) ? a : b, *winner = loser == a ? b : a;
        loser->losing = 1;
        loser->left = (u32)losing_bit((u8)loser->trn->value, (u8)winner->trn->value) * BIT_PERIODS *
                      period(loser);
        loser->sl.i = winner->sent;
        loser->sl.addressed = 0;
        loser->rival = winner->rival = 0;
        w->master = winner;
    } else {
        w->shared = 1;
    }
    clock_state(a->m);
    clock_state(b->m);
}

/* a master's byte: with a rival, it waits for the rival's byte to go with it */
static void wire_send(struct unit *u)
{
    struct unit *r = u->rival;
    if (!r)
        return;
    if (r->op == OP_TX && r->awaiting)
        contend(u, r);
    else
        u->awaiting = 1;
}

/* the byte reaches the other members' slaves at its end: whether any acknowledged it */
static int wire_deliver(struct unit *u)
{
    struct wire *w = u->w;
    if (u->rival && w->shared == 2) {
        w->shared = 0;              /* the rival's end delivered this byte, the same */
        return w->shared_ack;
    }
    int ack = 0;
    for (int i = 0; i < w->n; i++)
        if (w->m[i] != u && w->m[i] != u->rival)
            ack |= take(w->m[i], (u8)u->trn->value);
    if (u->rival && w->shared == 1) {
        w->shared = 2;
        w->shared_ack = ack;
    }
    return ack;
}

/* the other members' slaves see the master's Start or Repeated Start: an address comes next */
static void wire_started(struct unit *u)
{
    struct wire *w = u->w;
    for (int i = 0; i < w->n; i++) {
        struct unit *x = w->m[i];
        if (x == u)
            continue;
        if (con(x) & I2CEN)
            set_stat(x, ST_S, ST_P);
        x->sl.i = x->sl.addressed = x->sl.read = 0;
    }
}

static void wire_stopped(struct unit *u)
{
    struct wire *w = u->w;
    for (int i = 0; i < w->n; i++) {
        struct unit *x = w->m[i];
        if (x == u)
            continue;
        if (con(x) & I2CEN)
            set_stat(x, ST_P, ST_S);
        x->sl.i = x->sl.addressed = x->sl.read = 0;
    }
    if (w->master == u)
        w->master = 0;
}

/* the slave a read addressed: it transmits */
static struct unit *transmitter(struct wire *w)
{
    for (int i = 0; i < w->n; i++)
        if (w->m[i]->sl.read)
            return w->m[i];
    return 0;
}

/* §7.5: the byte the read's slave sends, I2CxTRN's, its eight bits out; none, the pull-ups */
static u8 wire_read(struct unit *u)
{
    struct unit *x = transmitter(u->w);
    if (!x)
        return 0xff;
    set_stat(x, 0, TBF);
    return (u8)x->trn->value;
}

/* §7.5, §7.5.1: the master's acknowledge of the byte: an ACK asks for another, and the slave
 * waits for it; a NACK ends the read */
static void wire_acked(struct unit *u, int ack)
{
    struct unit *x = transmitter(u->w);
    if (!x)
        return;
    set_stat(x, ack ? 0 : ACKSTAT, ack ? ACKSTAT : 0);
    if (ack)
        hold_clock(x);
    else
        x->sl.read = 0;
    if (x->sirq >= 0)
        pic24_irq(x->m->s, x->sirq);
}

/* --wire: two parts' units on one bus, pulled up */
static void join(void *ctx, void *other)
{
    struct unit *add[] = { ctx, other };
    struct wire *w = add[0]->w ? add[0]->w : add[1]->w;
    if (add[0]->w && add[1]->w && add[0]->w != add[1]->w)
        not_modelled(add[0], "joining two buses parts already share");
    if (!w && !(w = calloc(1, sizeof *w)))
        die("out of memory");
    for (int i = 0; i < 2; i++) {
        struct unit *x = add[i];
        if (x->w)
            continue;
        if (w->n == WIRE_UNITS)
            die("--wire: at most %d units on one bus", WIRE_UNITS);
        find_pins(x);
        if (x->scl < 0)
            not_modelled(x, "a bus on pins the part has not");
        x->w = w;
        w->m[w->n++] = x;
        set_outside(x, 0, 0);
    }
}

/* ---- the unit's clock -------------------------------------------------- */

static void i2c_tick(void *ctx, u32 cycles)
{
    struct i2c *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        if (!u->con)
            continue;
        if (counting(u)) {
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
        if (counting(u) && u->left < best)
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
    if (u->o.on && u->sl.addressed)
        not_modelled(u, "the unit disabled during another master's write to it");
    u->op = OP_NONE;
    u->active = 0;
    u->pin_start = 0;
    u->frozen = u->awaiting = 0;
    memset(&u->sl, 0, sizeof u->sl);
    if (u->w) {
        struct wire *w = u->w;
        if (w->master == u)
            w->master = 0;
        if (u->rival)
            u->rival->rival = 0;
        u->rival = 0;
        wire_released(w);
    }
    rf_hw(u->stat, 0);
    set_con(u, SCLREL, 0);
    take_pins(u, NOT_DRIVEN);
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
        if (u->sl.read && !u->active) {
            rf_write(m->p, off, n, v);
            set_stat(u, TBF | D_A, 0);  /* §7.5, Figure 7-15: the slave's byte; D/A says data */
            return;
        }
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
    if (u->sl.holding && (now & SCLREL) && !(was & SCLREL)) {
        u->sl.holding = 0;
        if (u->o.held) {
            u->o.held = 0;
            u->o.at = m->s->cycles + u->o.resume;
            clock_state(m);
        }
        if (u->w) {
            wire_released(u->w);
        } else if (u->frozen) {
            u->frozen = 0;
            clock_state(m);
        }
    }
    if (!(now & I2CEN)) {
        if (was & I2CEN)
            reset_unit(u);
        return;
    }
    if (!(was & I2CEN))
        take_pins(u, PIC24_PIN_RELEASED);
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

/* A reset makes every pin an input and every unit gives its pins back: the
 * part lets its lines go, and they are at what the outside holds. */
static void i2c_reset(void *ctx)
{
    struct i2c *m = ctx;
    for (int k = 1; k <= NU; k++) {
        struct unit *u = &m->u[k];
        u->op = OP_NONE;
        u->active = 0;
        u->pin_start = 0;
        memset(&u->o, 0, sizeof u->o);
        if (!u->con || !u->pinned || u->scl < 0)
            continue;
        u->scl_level = !u->scl_out;
        u->sda_level = !u->sda_out;
        u->frozen = u->awaiting = 0;
        memset(&u->sl, 0, sizeof u->sl);
        if (u->w) {
            if (u->w->master == u)
                u->w->master = 0;
            if (u->rival)
                u->rival->rival = 0;
            u->rival = 0;
        }
        if (u->scl_drive != NOT_DRIVEN || u->sda_drive != NOT_DRIVEN) {
            u->scl_drive = u->sda_drive = NOT_DRIVEN;
            if (u->w)
                wire_lines(u->w);
            else if (on_bus(u))
                i2cbus_lines(m->s, k, NOT_DRIVEN, NOT_DRIVEN);
        }
    }
    m->running = 0;
}

static const struct i2c_unit_ops unit_ops = { inject, outside_lines, join };

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
        u->scl = u->sda = -1;
        i2cbus_attach(s, k, u, &unit_ops);
    }
    if (!any) {
        free(m);
        return 0;
    }
    return m;
}
