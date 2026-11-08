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
 *     Starts under way together contend byte by byte at the slower clock
 *     (§6.2), the loser setting BCL at its losing bit and its slave taking
 *     the winner's bytes (§6.3, §6.4, §6.7). While their bytes have not
 *     differed, an event of one waits for the other's next, its clock held
 *     by the other's SCLx (§6.2): the same event from both goes on together,
 *     and a Repeated Start or Stop against data, or a NACK against an ACK,
 *     loses, the other going on alone (§6.3, §6.6, §6.8). Addressed for a
 *     read, a slave sets R/W and holds SCLx until the program lets go
 *     (§7.5.1); the master reads I2CxTRN, written or not
 *     (§7.5 shifts its bits out), and clears TBF; its ACK asks for another
 *     byte and holds SCLx again, its NACK ends the read; either raises
 *     SI2Cx with ACKSTAT the master's. The members run at one instruction
 *     clock, stepped together. Between the events of its open frame the
 *     master holds SCLx low, and SDAx after its Start or Repeated Start; a
 *     slave with its byte loaded drives the byte's MSB (§5.2, §5.3): PORTx
 *     reads them, and a port's drive under that SCLx makes no Start or Stop
 *     (§2.1). A port driving the lines inside a master's event loses the
 *     event's byte: the slaves go idle at the port's Stop, the receiving
 *     master takes 1s from the port's bit on and 0s where the port held
 *     SDAx, and a 1 the master sends under that SDAx, or its Start, Repeated
 *     Start or Stop, collides (§6.4-§6.8). An event begun while a port holds
 *     SCLx waits for it (§6.2); one whose clock starts under a port's low
 *     SDAx collides on a 1 it sends there, a Repeated Start's and a NACK's
 *     among them, and a read takes those bits as 0s, a 1 of the slave's at
 *     the port's release being a Stop that sends the slaves idle.
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
 * lose), the endpoint's lines changing while the part's master or another
 * master's write is on the bus, SCLx clocking after a Start seen on the
 * lines (an address at the pins), the outside holding low a line the port
 * drives high, the endpoint on a bus parts share, two parts' masters whose
 * bytes have not differed going on to other events than those, and a port
 * driving the lines while two parts' masters contend. */
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
#define NOT_PULLED (-1)
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
    int last;                       /* its master's last event to finish */
    u32 total;                      /* the event under way's cycles, as it began */
    int garbled;                    /* a port drove the lines inside that event: its byte is lost */
    u8 garbled_rx;                  /* that event's byte, as the receiving master takes it */
    int pulled_at;                  /* the bit a port pulled SDAx low at inside it, or NOT_PULLED */
    int held;                       /* a port has held SDAx low since the event's clock started */
};

/* a bus between parts' units (--wire) */
struct wire {
    struct unit *m[WIRE_UNITS];
    int n;
    struct unit *master;            /* the member whose master took the bus, to its Stop */
    int shared;                     /* two rivals' equal byte: 1 under way, 2 delivered */
    int shared_ack;                 /* the slaves' acknowledge of it */
    int framing;                    /* the lines change by the master's frame, no port's drive */
};

struct i2c {
    struct sim *s;
    struct rf_periph *p;
    struct unit u[NU + 1];
    int running;
};

/* in a run of several parts, the message names the part */
static void not_modelled(const struct unit *u, const char *what)
{
    if (u->m->s->never_idle)
        die("part %d's I2C%d: not modelled: %s", u->m->s->part, u->k, what);
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
static void port_drove(struct unit *u, int scl_was, int sda_was);
static void frame_changed(struct wire *w);
static struct unit *frame_master(const struct wire *w);
static int port_holds_line(const struct unit *u, int sda);
static void clock_starts(struct unit *u);
static int resolve_held(struct unit *m, int to);

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
    int scl_was = u->scl_drive, sda_was = u->sda_drive;
    u->scl_drive = scl;
    u->sda_drive = sda;
    if (u->w) {
        port_drove(u, scl_was, sda_was);
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
    /* a frame's own lines, and a port's under the SCLx a frame holds low (§2.1): no Start, no Stop */
    if (u->w && (u->w->framing || frame_master(u->w)))
        return;
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
static void wire_meet(struct unit *u);
static int wire_deliver(struct unit *u);
static void wire_started(struct unit *u);
static void wire_stopped(struct unit *u);
static u8 wire_read(struct unit *u);
static void wire_acked(struct unit *u, int ack);
static int take(struct unit *u, u8 byte);

#define NEVER (~0ull)
#define BYTE_BITS 8
#define TOP_BIT 0x80u
#define BYTE_MASK 0xffu

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
    u->total = u->left;
    u->garbled = 0;
    u->held = 0;
    u->pulled_at = NOT_PULLED;
    /* §6.2: a slave holding SCLx low suspends the master's clock, the part's own too */
    u->frozen = op != OP_START && (u->w ? wire_held(u->w) || port_holds_line(u, 0) : u->sl.holding);
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
    if (u->w && u->rival && op != OP_START)
        wire_meet(u);
    if (u->w && op != OP_START && !u->frozen && !u->awaiting)
        clock_starts(u);
    clock_state(u->m);
}

static int wire_pulled_to_the_end(struct unit *u);

static void finish(struct unit *u)
{
    if (u->w && u->held && resolve_held(u, BYTE_BITS))
        return;
    if (u->w && wire_pulled_to_the_end(u))
        return;
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
        if (u->rival)
            u->rival = u->rival->rival = 0;
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
            u->ack = u->garbled ? 0 : wire_deliver(u);
        /* the part's own slave stays armed while its master sends (the transport's slave
         * handler, i2c1_bus_slave_isr_handler, relies on it): a byte to its address it takes */
        u->ack |= take(u, (u8)u->trn->value);
        u->sent++;
        set_stat(u, u->ack ? 0 : ACKSTAT, TBF | TRSTAT | (u->ack ? ACKSTAT : 0));
        break;
    case OP_RX:
        if (u->w)
            u->rx = u->garbled ? u->garbled_rx : wire_read(u);
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
    u->last = u->op;
    u->op = OP_NONE;
    u->garbled = 0;
    u->held = 0;
    u->pulled_at = NOT_PULLED;
    if (u->w)
        frame_changed(u->w);
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

static void frame_holds(struct wire *w, int *scl, int *sda);

/* the lines' wired-AND: each member's outside is what the open frame and the others hold low */
static void wire_lines(struct wire *w)
{
    int frame_scl, frame_sda;
    frame_holds(w, &frame_scl, &frame_sda);
    for (int i = 0; i < w->n; i++) {
        struct unit *x = w->m[i];
        int scl = frame_scl, sda = frame_sda;
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

/* a slave or a port let SCLx go: the masters it held go on once nothing holds it (§6.2) */
static void wire_released(struct wire *w)
{
    if (wire_held(w))
        return;
    for (int i = 0; i < w->n; i++) {
        struct unit *m = w->m[i];
        if (m->frozen && !port_holds_line(m, 0)) {
            m->frozen = 0;
            clock_starts(m);
            clock_state(m->m);
        }
    }
}

/* whether the master's event goes ahead: a Start takes the bus, contends with another
 * member's Start under way (§6.7), or collides with a master that holds it (§6.5) */
static int wire_begin(struct unit *u, int op)
{
    struct wire *w = u->w;
    if (op != OP_START)
        return 1;
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

/* LEFT cycles of unit FROM's clock, on unit TO's: one time on two parts' clocks */
static u32 on_clock_of(const struct unit *to, const struct unit *from, u64 left)
{
    u32 hz_to = pic24_fcy(to->m->s), hz_from = pic24_fcy(from->m->s);
    return hz_to == hz_from ? (u32)left : (u32)(left * hz_to / hz_from);
}

/* whether A's LEFT cycles last longer than B's, each on its own part's clock */
static int lasts_longer(const struct unit *a, u32 left_a, const struct unit *b, u32 left_b)
{
    return (u64)left_a * pic24_fcy(b->m->s) > (u64)left_b * pic24_fcy(a->m->s);
}

/* §6.3, §6.7: two rivals' bytes go bit by bit together; the one that sends a 1 where the
 * other sends a 0 loses at that bit, and the other's byte goes on */
static void contend(struct unit *a, struct unit *b)
{
    struct wire *w = a->w;
    u8 x = (u8)a->trn->value, y = (u8)b->trn->value;
    /* §6.2: their clocks synchronised, the bits go at the slower one's */
    const struct unit *slow = lasts_longer(b, period(b), a, period(a)) ? b : a;
    a->awaiting = b->awaiting = 0;
    a->left = a->total = on_clock_of(a, slow, (u64)SENT_BITS * BIT_PERIODS * period(slow));
    b->left = b->total = on_clock_of(b, slow, (u64)SENT_BITS * BIT_PERIODS * period(slow));
    if (x != y) {
        struct unit *loser = losing_bit(x, y) ? a : b, *winner = loser == a ? b : a;
        loser->losing = 1;
        loser->left = on_clock_of(loser, slow, (u64)losing_bit((u8)loser->trn->value, (u8)winner->trn->value) *
                                                   BIT_PERIODS * period(slow));
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

/* the member whose master's frame is open, its Start done and no Stop yet; 0 when none is */
static struct unit *frame_master(const struct wire *w)
{
    return w->master && w->master->active ? w->master : 0;
}

/* §5.2 (Figure 5-3), §5.3: between its events the master of an open frame holds SCLx low; SDAx
 * is low after its Start or Repeated Start, and after an address or acknowledge the slave drives
 * its loaded byte's MSB (Figure 5-4). Inside an event the lines are as between: what a port finds
 * there, its bit under way's, is not looked at. */
static void frame_holds(struct wire *w, int *scl, int *sda)
{
    struct unit *m = frame_master(w);
    *scl = *sda = 0;
    if (!m)
        return;
    *scl = 1;
    if (m->last == OP_START || m->last == OP_RESTART) {
        *sda = 1;
        return;
    }
    struct unit *x = transmitter(w);
    *sda = x && (stat(x) & TBF) && !((u32)x->trn->value & TOP_BIT);
}

/* the frame changed the lines, no port's drive: no Start or Stop comes of it */
static void frame_changed(struct wire *w)
{
    w->framing = 1;
    wire_lines(w);
    w->framing = 0;
}

/* another member's port holds SCLx (sda 0) or SDAx (sda 1) low */
static int port_holds_line(const struct unit *u, int sda)
{
    for (int i = 0; i < u->w->n; i++) {
        const struct unit *x = u->w->m[i];
        if (x != u && (sda ? x->sda_drive : x->scl_drive) == DRIVEN_LOW)
            return 1;
    }
    return 0;
}

/* the event's clock starts: a port's low SDAx under its first bit is resolved as it lets go */
static void clock_starts(struct unit *u)
{
    if (!port_holds_line(u, 1))
        return;
    u->held = 1;
    u->pulled_at = 0;
}

static int bit_under_way(const struct unit *m)
{
    return (int)((m->total - m->left) / (BIT_PERIODS * period(m)));
}

/* §6.4: the master's event ends in a bus collision: halted, TBF clear, its lines let go, its
 * frame over */
static void collided(struct unit *m)
{
    set_stat(m, BCL, TBF | TRSTAT);
    set_con(m, 0, EVENTS);
    m->op = OP_NONE;
    m->active = 0;
    m->garbled = 0;
    m->pulled_at = NOT_PULLED;
    if (m->w->master == m)
        m->w->master = 0;
    clock_state(m->m);
    if (m->mirq >= 0)
        pic24_irq(m->m->s, m->mirq);
}

/* §2.1: a port's bus clear ends in its Stop: every member's slave goes back to idle, no byte
 * taken until a Start addresses it, a slave transmitter's byte gone */
static void slaves_lost(struct wire *w)
{
    for (int i = 0; i < w->n; i++) {
        struct unit *x = w->m[i];
        if (x->sl.read)
            set_stat(x, 0, TBF);
        if (con(x) & I2CEN)
            set_stat(x, ST_P, ST_S);
        x->sl.i = 1;
        x->sl.addressed = x->sl.read = 0;
    }
}

/* §6.4, §6.7: whether the master sends a 1 in bits FROM to TO of its event */
static int sends_a_1(const struct unit *m, int from, int to)
{
    if (m->op == OP_ACK)
        return (con(m) & ACKDT) != 0;
    if (m->op == OP_RESTART)
        return 1;                   /* §6.6: SDAx let go high before it falls */
    if (m->op != OP_TX)
        return 0;
    for (int bit = from; bit <= to && bit < BYTE_BITS; bit++)
        if (((u8)m->trn->value << bit) & TOP_BIT)
            return 1;
    return 0;
}

/* a port held SDAx low over bits FROM to TO: a 1 the master sends under it collides, and a
 * receiving master takes those bits as 0s; whether the master collided */
static int pulled_under(struct unit *m, int from, int to)
{
    if (sends_a_1(m, from, to)) {
        collided(m);
        return 1;
    }
    if (m->op == OP_RX)
        for (int bit = from; bit <= to && bit < BYTE_BITS; bit++)
            m->garbled_rx &= (u8)~(TOP_BIT >> bit);
    return 0;
}

/* §2.1, §6.2: a port inside the master's event. Its edges come between the master's clock's,
 * which does not follow them, and its bus clear's Stop ends the slaves' part: the byte under way
 * is lost, and the receiving master, not seeing it, takes the slave's bits before the port's and
 * 1s after. A Start, Repeated Start or Stop the port's lines meet collides (§6.5, §6.6, §6.8).
 * Whether the master's frame ended */
static int disturbed(struct unit *m, int sda_pulled, int sda_let_go)
{
    int bit = bit_under_way(m);
    if (m->op == OP_START || m->op == OP_RESTART || m->op == OP_STOP) {
        slaves_lost(m->w);
        collided(m);
        return 1;
    }
    if (!m->garbled) {
        struct unit *x = transmitter(m->w);
        u8 kept = (u8)(BYTE_MASK << (BYTE_BITS - (bit < BYTE_BITS ? bit : BYTE_BITS)));
        u8 sent = x && (stat(x) & TBF) ? (u8)x->trn->value : BYTE_MASK;
        m->garbled = 1;
        m->garbled_rx = (u8)((sent & kept) | ~kept);
        slaves_lost(m->w);
    }
    if (sda_pulled)
        m->pulled_at = bit;
    if (sda_let_go && m->pulled_at != NOT_PULLED) {
        int from = m->pulled_at;
        m->pulled_at = NOT_PULLED;
        return pulled_under(m, from, bit);
    }
    return 0;
}

/* §6.3, §6.7, §2.1: the port let SDAx go, low since the event's clock started, to bit TO: a 1
 * the master sent under it collides (a Repeated Start's SDAx high among them); a receiving master
 * took those bits as 0s, and a 1 of the slave's at the release is the port's Stop, which sends
 * the slaves idle; under a 0 the release is not seen. Whether the master collided */
static int resolve_held(struct unit *m, int to)
{
    m->held = 0;
    m->pulled_at = NOT_PULLED;
    if (sends_a_1(m, 0, to)) {
        collided(m);
        frame_changed(m->w);
        return 1;
    }
    if (m->op == OP_RX) {
        struct unit *x = transmitter(m->w);
        u8 sent = x && (stat(x) & TBF) ? (u8)x->trn->value : BYTE_MASK;
        int last = to < BYTE_BITS ? to : BYTE_BITS - 1;
        u8 under = (u8)(BYTE_MASK << (BYTE_BITS - 1 - last));
        m->garbled = 1;
        m->garbled_rx = (u8)(sent & ~under);
        if ((u8)(sent << last) & TOP_BIT) {
            m->garbled_rx = (u8)~under;
            slaves_lost(m->w);
        }
    }
    return 0;
}

/* a member's port drove the bus's lines: inside another member's master's event it disturbs the
 * event; between the events of a frame, under its SCLx held low, it makes no Start or Stop; an
 * event its SCLx held goes on as it lets go (§6.2) */
static void port_drove(struct unit *u, int scl_was, int sda_was)
{
    struct wire *w = u->w;
    struct unit *m = w->master;
    int sda_pulled = u->sda_drive == DRIVEN_LOW && sda_was != DRIVEN_LOW;
    int sda_let_go = sda_was == DRIVEN_LOW && u->sda_drive != DRIVEN_LOW;
    int scl_let_go = scl_was == DRIVEN_LOW && u->scl_drive != DRIVEN_LOW;
    int pulled = sda_pulled || (u->scl_drive == DRIVEN_LOW && scl_was != DRIVEN_LOW);
    if (m && m != u && m->op != OP_NONE && !m->frozen && (pulled || sda_let_go)) {
        if (m->rival || m->awaiting)
            not_modelled(m, "another part's port driving the bus's lines while two parts' masters contend for it (§6)");
        if (m->held && sda_let_go && !pulled) {
            if (!port_holds_line(m, 1) && resolve_held(m, bit_under_way(m)))
                return;
        } else {
            m->held = 0;
            if (disturbed(m, sda_pulled, sda_let_go)) {
                frame_changed(w);
                return;
            }
        }
    }
    wire_lines(w);
    if (scl_let_go)
        wire_released(w);
}

/* §6.3, §6.6, §6.8: the master whose event meets the other's data or ACK loses: its event ends
 * in a bus collision, and the other's goes on alone */
static void loses(struct unit *loser, struct unit *winner)
{
    struct wire *w = loser->w;
    loser->rival = winner->rival = 0;
    loser->awaiting = 0;
    collided(loser);
    w->master = winner;
    if (winner->awaiting) {
        winner->awaiting = 0;
        clock_state(winner->m);
    }
    frame_changed(w);
}

/* the same event from both: they go on together, at the slower clock (§6.2) */
static void together(struct unit *a, struct unit *b)
{
    struct unit *slow = lasts_longer(b, b->left, a, a->left) ? b : a, *fast = slow == a ? b : a;
    a->awaiting = b->awaiting = 0;
    fast->left = on_clock_of(fast, slow, slow->left);
    a->total = a->left;
    b->total = b->left;
    clock_state(a->m);
    clock_state(b->m);
}

static void meet(struct unit *a, struct unit *b)
{
    if (a->op == OP_TX && b->op == OP_TX) {
        contend(a, b);
        return;
    }
    if (a->op == b->op && (a->op != OP_ACK || !(con(a) & ACKDT) == !(con(b) & ACKDT))) {
        together(a, b);
        return;
    }
    struct unit *loser;
    if (a->op == OP_ACK && b->op == OP_ACK)
        loser = (con(a) & ACKDT) ? a : b;                       /* §6.3: the NACK */
    else if ((a->op == OP_RESTART || a->op == OP_STOP) && b->op == OP_TX)
        loser = a;                                              /* §6.6, §6.8 */
    else if ((b->op == OP_RESTART || b->op == OP_STOP) && a->op == OP_TX)
        loser = b;
    else
        not_modelled(a, "two parts' masters whose bytes have not differed going on to a Repeated "
                        "Start, Stop or read against anything but data, or a read against data (§6)");
    loses(loser, loser == a ? b : a);
}

/* §6.2, §6.7: two masters whose bytes have not differed go on together. An event of one waits,
 * its clock held by the other's SCLx low between events, for the other's next; there they meet */
static void wire_meet(struct unit *u)
{
    struct unit *r = u->rival;
    if (r->op == OP_NONE || !r->awaiting) {
        u->awaiting = 1;
        return;
    }
    meet(u, r);
}

/* a port still holding SDAx low as the master's event ends: whether the master collided */
static int wire_pulled_to_the_end(struct unit *u)
{
    if (!u->garbled || u->pulled_at == NOT_PULLED)
        return 0;
    int from = u->pulled_at;
    u->pulled_at = NOT_PULLED;
    if (!pulled_under(u, from, BYTE_BITS))
        return 0;
    frame_changed(u->w);
    return 1;
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
        if (u->rival) {
            struct unit *r = u->rival;
            r->rival = 0;
            if (r->awaiting) {
                r->awaiting = 0;    /* its clock no longer held by this one's */
                clock_state(r->m);
            }
        }
        u->rival = 0;
        u->garbled = 0;
        u->pulled_at = NOT_PULLED;
        wire_released(w);
        frame_changed(w);
    }
    rf_hw(u->stat, 0);
    /* Register 3-1, §11.0: the module's reset ends the event under way, its control bit with it */
    set_con(u, SCLREL, EVENTS);
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
            if (u->w)
                frame_changed(u->w);
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
