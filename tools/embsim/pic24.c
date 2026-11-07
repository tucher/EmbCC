/* pic24.c -- the PIC24F core (a PIC24FJ1024GB610's: the PIC24 CPU with
 * the Extended Data Space), and a dsPIC33EP's, by Microchip's "16-Bit
 * MCU and DSC Programmer's Reference Manual" (DS70000157) and the parts'
 * data sheets.
 *
 *   - The instruction set of the PIC24F: every instruction the manual
 *     marks as the PIC24F's, with its addressing modes (register direct,
 *     indirect with pre/post increment and decrement, register offset,
 *     signed offset, the file registers' 13 and 16-bit addresses), byte
 *     and word forms, and SR's flags as each instruction's description
 *     gives them (DC, N, OV, Z, C; Z sticky for ADDC, SUBB, SUBBR, CPB).
 *     The dsPIC's DSP instructions and the PIC24E's are not the core's:
 *     an opcode it does not have makes the part reset (RCON.IOPUWR), and
 *     the run ends there, naming it.
 *   - REPEAT, RCOUNT and SR.RA: the target executes RCOUNT + 1 times and
 *     may be interrupted between any two of them. DIV.S, DIV.U and their
 *     .D forms give their quotient, remainder and flags on the first of
 *     the 18 executions (REPEAT #17), and the other 17 change nothing, so
 *     an interrupt in the middle leaves W0 and W1 as the program will see
 *     them; divided by zero, the math error trap.
 *   - The W registers, SPLIM, PCL/PCH, DSRPAG, DSWPAG, RCOUNT, SR, CORCON,
 *     DISICNT and TBLPAG in the data space at 0x0000-0x0055 (the core's
 *     own); a W register used as a pointer before anything is written to
 *     it, the part resets (RCON.IOPUWR), and the run ends.
 *   - The data space: the SFRs and RAM below 0x8000; above, the 32 KiB
 *     EDS window through DSRPAG (reads) and DSWPAG (writes): pages 1 to
 *     0x1FF are the EDS (RAM above 0x8000, then the EPMP's space), 0x200
 *     to 0x2FF program memory's low words and 0x300 to 0x3FF its high
 *     bytes (program space visibility, reads only); page 0 there is an
 *     address error. A word access at an odd address is an address
 *     error. W15 as a pointer below 0x0800 or above SPLIM is a stack
 *     error.
 *   - TBLRDL and TBLRDH read program and configuration memory (TBLPAG and
 *     the address); TBLWTL and TBLWTH write the flash's latches, which
 *     are not modelled: they are ignored.
 *   - Exceptions: the traps (their status in INTCON1, through the
 *     controller) and the interrupt the controller presents when its
 *     priority is above the CPU's (SR.IPL, CORCON.IPL3), unless DISI
 *     holds priorities 1 to 6 off. The entry pushes the PC and SR's low
 *     byte with IPL3, raises the priority, and goes to the vector's
 *     address in the IVT (0x000004 + 2n); RETFIE undoes it. A trap prints
 *     what raised it on stderr, as a part with a debugger would stop.
 *   - PWRSAV: until an interrupt; with none that can come, the run ends.
 *     A branch to itself with no interrupt to come ends it too.
 *
 * A dsPIC33EP's core (pic24.h's `e` and `dsp`) is the PIC24E's with the
 * DSP engine (pic24-dsp.c), by the same manual's pages for the dsPIC33E:
 *   - the PIC24E's instructions where the PIC24F's differ: CALL, RCALL,
 *     CALL.L and LNK keep the stack frame flag (CORCON.SFA) in the
 *     return address's bit 0, RETURN, RETLW and RETFIE restore it, ULNK
 *     clears it, as an exception's entry stacks it; the computed RCALL,
 *     GOTO and BRA's other encodings, GOTO.L; CP and CPB with an 8-bit
 *     literal; REPEAT #lit15 and of a whole Wn; CPBEQ, CPBNE, CPBGT and
 *     CPBLT (their offset 1: the compare and skip); MULW's 16-bit
 *     product; MOVPAG;
 *   - the dsPIC's: DO (#lit15 or Wn; four nested, the outer loops'
 *     registers on the DO stack, a fifth the soft trap; DCOUNT, DOSTART,
 *     DOEND, CORCON.DL, SR.DA; CORCON.EDT ends a loop at its pass's end),
 *     BRA OA, OB, SA and SB, DIVF (the quotient of Wm * 2^15 / Wn, the
 *     remainder in the same scale);
 *   - the core's registers: the accumulators, DCOUNT, DOSTART (read-only)
 *     and DOEND, MODCON, XMODSRT, XMODEND, YMODSRT, YMODEND, XBREV; SR's
 *     DSP bits (OA, OB, SA and SB writable, clearing SAB clears SA and
 *     SB) and CORCON's; W15 at the RAM's start (0x1000), DSRPAG and
 *     DSWPAG 1 out of reset.
 *
 * Cycles are the manual's: one an instruction, two for a taken branch, a
 * GOTO, CALL, RCALL, MOV.D, PUSH.D, POP.D and the table instructions,
 * three for RETURN, RETLW and RETFIE, one more for a skip over a word and
 * two over a two-word instruction, one more for a read through the EDS
 * window, five for an exception's entry. A PIC24E's: four for a taken
 * branch, GOTO, CALL and RCALL, five for a taken compare and branch, six
 * for RETURN, RETLW and RETFIE, three more for the table instructions,
 * four more for a read of program memory through the EDS window, one more
 * for a read of a peripheral's register. */
#include <stdlib.h>
#include <string.h>

#include "devices.h"
#include "pic24.h"
#include "svd-map.h"

struct pic24_state *ps;

static struct bus *bus(void)
{
    return &ps->sim->bus;
}

/* ---- program memory ------------------------------------------------------- */

u32 pic24_prog(struct sim *s, u32 addr)
{
    u32 a = 2 * (addr & ~1u), v;
    struct pic24_state *k = (struct pic24_state *)s->cpu;
    if ((addr & 0xffff00u) == PIC24_LATCHES && pic24_is(s->cpu) && k->nvm)
        return k->nvm->latch_read(k->nvm->ctx, addr) & 0xffffff;
    struct region *r = bus_region(&s->bus, a, 3);
    if (r)
        return (u32)r->mem[a - r->base] | (u32)r->mem[a - r->base + 1] << 8 |
               (u32)r->mem[a - r->base + 2] << 16;
    if (bus_debug_read(&s->bus, a, 4, &v))
        return 0xffffff;                        /* erased */
    return v & 0xffffff;
}

/* ---- ending the run, traps ------------------------------------------------ */

/* RESET with --reset-runs-on: the part resets and starts again at its reset
 * vector. RCON keeps the flags it had and gains SWR (the data sheet's
 * Register 8-1); the PC is the reset's, not the next instruction's. */
#define RCON_SWR 0x0040u

static int software_reset(void)
{
    struct sim *s = ps->sim;
    struct rf_periph *p = s->svd ? rf_periph(s->svd, "RCON") : 0;
    struct rf_reg *rcon = p ? rf_reg(p, "RCON") : 0;
    if (!rcon)
        die("--reset-runs-on: the part's register map has no RCON");
    u64 flags = rcon->value;
    sim_part_reset(s);
    rcon->value = flags | RCON_SWR;
    ps->npc = ps->pc;
    ps->changed = 1;
    return 1;
}

static int illegal(const char *why)
{
    sim_end(ps->sim, END_LOCKUP, "lockup: %s at 0x%06x (the part resets: "
            "RCON.IOPUWR)", why, (unsigned)ps->pc);
    return 0;
}

static int bad_opcode(void)
{
    char m[64];
    snprintf(m, sizeof m, "0x%06x is not a %s instruction",
             (unsigned)ps->insn, ps->dsp ? "dsPIC33E" : ps->e ? "PIC24E" : "PIC24F");
    return illegal(m);
}

static const char *const trap_name[8] = {
    "an oscillator failure", "an address error", "a hard trap",
    "a stack error", "a math error", "trap 5", "a soft trap", "trap 7",
};

static int traps_told;

const char *pic24_trap_name(int n)
{
    return trap_name[n & 7];
}

/* the instruction executing raises trap n: the first is the one */
static int trap(int n, const char *why, u32 at)
{
    if (!ps->trap) {
        ps->trap = n + 1;
        ps->trap_pc = ps->pc;
        if (at != ~0u)
            snprintf(ps->trap_why, sizeof ps->trap_why, "%s 0x%04x", why, (unsigned)at);
        else
            snprintf(ps->trap_why, sizeof ps->trap_why, "%s", why);
        if (traps_told++ < 10) {
            fflush(stdout);
            fprintf(stderr, "embsim: %s trap at pc 0x%06x: %s", trap_name[n],
                    (unsigned)ps->pc, why);
            if (at != ~0u)
                fprintf(stderr, " 0x%04x", (unsigned)at);
            fputc('\n', stderr);
        }
    }
    return -1;
}

/* ---- the core's registers in the data space -------------------------------- */

static u16 disicnt(void)
{
    u64 gone = ps->sim->cycles - ps->disi_at;
    return gone >= ps->disicnt ? 0 : (u16)(ps->disicnt - gone);
}

/* SR with a dsPIC's summary bits: OAB of OA and OB, SAB of SA and SB */
static void sr_summary(void)
{
    ps->sr = (u16)(ps->sr & ~(SR_OAB | SR_SAB));
    if (ps->sr & (SR_OA | SR_OB))
        ps->sr |= SR_OAB;
    if (ps->sr & (SR_SA | SR_SB))
        ps->sr |= SR_SAB;
}

static u16 creg_read(u32 a)
{
    if (a < 0x20)
        return ps->w[a / 2];
    if (ps->dsp)
        switch (a) {
        case 0x22: case 0x24: case 0x26: case 0x28: case 0x2a: case 0x2c:
            return pic24_acc_read(a);
        case 0x38: return ps->dcount;
        case 0x3a: return (u16)ps->dostart;
        case 0x3c: return (u16)(ps->dostart >> 16 & 0x3f);
        case 0x3e: return (u16)ps->doend;
        case 0x40: return (u16)(ps->doend >> 16 & 0x3f);
        case 0x46: return ps->modcon;
        case 0x48: return ps->xmodsrt;
        case 0x4a: return ps->xmodend;
        case 0x4c: return ps->ymodsrt;
        case 0x4e: return ps->ymodend;
        case 0x50: return ps->xbrev;
        case 0x58: return ps->mstrpr;
        }
    switch (a) {
    case 0x20: return ps->splim;
    case 0x2e: return (u16)ps->pc;
    case 0x30: return (u16)(ps->pc >> 16 & 0x7f);
    case 0x32: return ps->dsrpag;
    case 0x34: return ps->dswpag;
    case 0x36: return ps->rcount;
    case 0x42: return ps->sr;
    case 0x44: return ps->corcon;
    case 0x52: return disicnt();
    case 0x54: return ps->tblpag;
    }
    return 0;
}

static void irq_changed(void);

static void set_w(int n, u16 v)
{
    ps->w[n] = n == 15 ? (u16)(v & ~1u) : v;
    ps->winit |= (u16)(1u << n);
    ps->changed = 1;
}

/* a write of the bits under mask */
static void creg_write(u32 a, u16 v, u16 mask)
{
    u16 old = creg_read(a), x = (u16)((old & ~mask) | (v & mask));
    ps->changed = 1;
    if (a < 0x20) {
        set_w((int)(a / 2), x);
        return;
    }
    if (ps->dsp)
        switch (a) {
        case 0x22: case 0x24: case 0x26: case 0x28: case 0x2a: case 0x2c:
            pic24_acc_write(a, x);
            return;
        case 0x38: ps->dcount = x; return;
        case 0x3a: case 0x3c: return;           /* DOSTART: read-only */
        case 0x3e: ps->doend = (ps->doend & 0x3f0000u) | (x & 0xfffeu); return;
        case 0x40: ps->doend = (ps->doend & 0xfffeu) | (u32)(x & 0x3f) << 16; return;
        case 0x46: ps->modcon = (u16)(x & 0xcfff); return;
        case 0x48: ps->xmodsrt = (u16)(x & ~1u); return;
        case 0x4a: ps->xmodend = (u16)(x | 1u); return;
        case 0x4c: ps->ymodsrt = (u16)(x & ~1u); return;
        case 0x4e: ps->ymodend = (u16)(x | 1u); return;
        case 0x50: ps->xbrev = x; return;
        case 0x58: ps->mstrpr = x; return;
        }
    switch (a) {
    case 0x20: ps->splim = (u16)(x & ~1u); ps->splim_set = 1; break;
    case 0x32: ps->dsrpag = (u16)(x & 0x3ff); break;
    case 0x34: ps->dswpag = (u16)(x & 0x1ff); break;
    case 0x36: ps->rcount = x; break;
    case 0x42: {
        u16 keep = SR_RA;
        if (ps->intc && ps->intc->nstdis && ps->intc->nstdis(ps->intc->ctx))
            keep |= SR_IPL;
        if (!ps->dsp) {
            ps->sr = (u16)(((ps->sr & keep) | (x & ~keep)) & 0x1ff);
            irq_changed();
            break;
        }
        /* a dsPIC33E's OA, OB, SA, SB are the program's to write too;
         * clearing SAB clears SA and SB; OAB, SAB, DA are the hardware's */
        keep |= SR_DA;
        if ((old & SR_SAB) && !(x & SR_SAB))
            x = (u16)(x & ~(SR_SA | SR_SB));
        ps->sr = (u16)((ps->sr & keep) | (x & ~keep & ~(SR_OAB | SR_SAB)));
        sr_summary();
        irq_changed();
        break;
    }
    case 0x44:
        if (ps->dsp) {
            /* DL and SFA are the hardware's; EDT ends the DO loop */
            u16 rw = CORCON_VAR | CORCON_US | CORCON_SATA | CORCON_SATB |
                     CORCON_SATDW | CORCON_ACCSAT | CORCON_RND | CORCON_IF;
            if (x & CORCON_EDT)
                ps->edt = 1;
            ps->corcon = (u16)((x & rw) | (ps->corcon & (CORCON_DL | CORCON_SFA)) |
                               (ps->corcon & x & CORCON_IPL3));
        } else if (ps->e)
            ps->corcon = (u16)((ps->corcon & CORCON_SFA) | (ps->corcon & x & CORCON_IPL3));
        else
            /* IPL3 can only be cleared; PSV is the program's */
            ps->corcon = (u16)((x & 0x4) | (ps->corcon & x & CORCON_IPL3));
        irq_changed();
        break;
    case 0x52:
        ps->disicnt = (u16)(x & 0x3fff);
        ps->disi_at = ps->sim->cycles;
        break;
    case 0x54: ps->tblpag = (u16)(x & 0xff); break;
    }
}

/* ---- the data space ------------------------------------------------------------ */

static void watched(u32 ba, int n, int write)
{
    if (bus()->nwatch)
        bus_watch_check(bus(), ba, n, write);
}

/* a read of n bytes (1 or 2) at data address ea; -1 with a trap raised */
static int dread(u32 ea, int n, u16 *v)
{
    ea &= 0xffff;
    if (n == 2 && (ea & 1))
        return trap(TRAP_ADDR, "a word read at the odd address", ea);
    if (ea < 0x80) {
        u16 x = creg_read(ea & ~1u);
        *v = n == 2 ? x : (u16)((ea & 1) ? x >> 8 : x & 0xff);
        return 0;
    }
    u32 ba;
    if (ea < 0x8000) {
        ba = PIC24_DATA + ea;
        if (ps->e && ea < ps->ram_base)
            ps->extra = 1;                      /* a peripheral's register */
    } else {
        u16 pg = ps->dsrpag;
        ps->extra = 1;
        if (pg & 0x200) {                       /* program space visibility */
            if (ps->e)
                ps->extra = 4;
            u32 p = (u32)(pg & 0xff) << 15 | (ea & 0x7fff);
            u32 word = pic24_prog(ps->sim, p);
            u16 half = (pg & 0x100) ? (u16)(word >> 16 & 0xff) : (u16)word;
            *v = n == 2 ? half : (u16)((p & 1) ? half >> 8 : half & 0xff);
            return 0;
        }
        if (!pg)
            return trap(TRAP_ADDR, "a read through the EDS window with DSRPAG "
                        "0 at", ea);
        ba = PIC24_DATA + ((u32)(pg & 0x1ff) << 15 | (ea & 0x7fff));
    }
    watched(ba, n, 0);
    struct region *r = bus_region(bus(), ba, (u32)n);
    u32 x;
    if (r)
        x = mem_rd_le(r->mem + (ba - r->base), n);
    else if (bus_read(bus(), ba, n, &x))
        x = 0;                                  /* nothing there reads 0 */
    *v = (u16)x;
    return 0;
}

static int dwrite(u32 ea, int n, u16 v)
{
    ea &= 0xffff;
    if (n == 2 && (ea & 1))
        return trap(TRAP_ADDR, "a word write at the odd address", ea);
    if (ea < 0x80) {
        if (n == 2)
            creg_write(ea, v, 0xffff);
        else if (ea & 1)
            creg_write(ea & ~1u, (u16)(v << 8), 0xff00);
        else
            creg_write(ea, (u16)(v & 0xff), 0x00ff);
        return 0;
    }
    u32 ba;
    if (ea < 0x8000)
        ba = PIC24_DATA + ea;
    else {
        u16 pg = ps->dswpag;
        if (!pg)
            return trap(TRAP_ADDR, "a write through the EDS window with DSWPAG "
                        "0 at", ea);
        ba = PIC24_DATA + ((u32)(pg & 0x1ff) << 15 | (ea & 0x7fff));
    }
    watched(ba, n, 1);
    ps->changed = 1;
    struct region *r = bus_region(bus(), ba, (u32)n);
    if (r) {
        if (!r->rom)
            mem_wr_le(r->mem + (ba - r->base), n, v);
    } else
        bus_write(bus(), ba, n, v);             /* nothing there: ignored */
    return 0;
}

/* ---- operands --------------------------------------------------------------------- */

/* W n as a pointer: written since reset, else the part resets */
static int pointer(int n)
{
    if (!(ps->winit >> n & 1)) {
        char m[64];
        snprintf(m, sizeof m, "W%d, never written, used as a pointer", n);
        return illegal(m);
    }
    return 1;
}

static int stack_check(u32 ea)
{
    if (ea < ps->ram_base)
        return trap(TRAP_STACK, "W15 points below the RAM, at", ea);
    if (ps->splim_set && ea > ps->splim)
        return trap(TRAP_STACK, "W15 is past SPLIM, at", ea);
    return 0;
}

/* W r modified by d through the X AGU: on a dsPIC, modulo or
 * bit-reversed as MODCON and XBREV say (pic24-dsp.c) */
static u16 step_w(int r, u16 w, int d, int write)
{
    return ps->dsp ? pic24_agu_step(r, w, d, write, 0) : (u16)(w + d);
}

/* The effective address of an indirect operand (modes 1-7 of ppp, qqq,
 * ggg, hhh: [W], [W--], [W++], [--W], [++W], [W+Wb]), its register
 * modified; -1 when it cannot be formed. `write`: the operand is the
 * instruction's destination (a dsPIC's bit-reversed addressing is the
 * X AGU's writes'). */
static int ea_of(int mode, int r, int wb, int n, u32 *ea, int write)
{
    if (!pointer(r))
        return -1;
    u16 w = ps->w[r];
    switch (mode) {
    case 1: *ea = w; break;
    case 2: *ea = w; set_w(r, step_w(r, w, -n, write)); break;
    case 3: *ea = w; set_w(r, step_w(r, w, n, write)); break;
    case 4: set_w(r, step_w(r, w, -n, write)); *ea = ps->w[r]; break;
    case 5: set_w(r, step_w(r, w, n, write)); *ea = ps->w[r]; break;
    default:
        if (wb < 0) {
            bad_opcode();
            return -1;
        }
        if (!pointer(wb))
            return -1;
        *ea = (u16)(w + ps->w[wb]);
        if (ps->dsp)
            *ea = pic24_agu_offset(r, (u16)*ea, (ps->w[wb] & 0x8000) != 0, 0);
        break;
    }
    if (r == 15 && stack_check(*ea))
        return -1;
    return 0;
}

/* a source operand of n bytes (mode 0: the register itself) */
static int src(int mode, int r, int wb, int n, u16 *v)
{
    if (!mode) {
        *v = n == 2 ? ps->w[r] : (u16)(ps->w[r] & 0xff);
        return 0;
    }
    u32 ea;
    if (ea_of(mode, r, wb, n, &ea, 0))
        return -1;
    return dread(ea, n, v);
}

/* a destination operand (a byte into a register is its low byte) */
static int dst(int mode, int r, int wb, int n, u16 v)
{
    if (!mode) {
        set_w(r, n == 2 ? v : (u16)((ps->w[r] & 0xff00) | (v & 0xff)));
        return 0;
    }
    u32 ea;
    if (ea_of(mode, r, wb, n, &ea, 1))
        return -1;
    return dwrite(ea, n, v);
}

static int push(u16 v)
{
    u32 ea = ps->w[15];
    if (stack_check(ea))
        return -1;
    set_w(15, (u16)(ea + 2));
    return dwrite(ea, 2, v);
}

static int pop(u16 *v)
{
    u32 ea = (u16)(ps->w[15] - 2);
    if (stack_check(ea))
        return -1;
    set_w(15, (u16)ea);
    return dread(ea, 2, v);
}

/* ---- flags ------------------------------------------------------------------------- */

static void flags(u16 mask, u16 v)
{
    ps->sr = (u16)((ps->sr & ~mask) | (v & mask));
}

static u16 nz(u32 r, int n)
{
    u32 m = n == 2 ? 0xffff : 0xff, top = n == 2 ? 0x8000 : 0x80;
    return (u16)(((r & top) ? SR_N : 0) | ((r & m) ? 0 : SR_Z));
}

/* a + b + cin, with DC, N, OV, Z, C; Z only cleared when sticky */
static u16 add(u32 a, u32 b, u32 cin, int n, int sticky)
{
    u32 m = n == 2 ? 0xffff : 0xff, top = n == 2 ? 0x8000 : 0x80;
    u32 dcbit = n == 2 ? 0x100 : 0x10;
    a &= m;
    b &= m;
    u32 r = a + b + cin;
    u16 f = (u16)(((r & top) ? SR_N : 0) | ((r > m) ? SR_C : 0) |
                  ((((a & (dcbit - 1)) + (b & (dcbit - 1)) + cin) & dcbit) ? SR_DC : 0) |
                  ((~(a ^ b) & (a ^ r) & top) ? SR_OV : 0));
    if (sticky)
        flags(SR_N | SR_C | SR_DC | SR_OV | ((r & m) ? SR_Z : 0), f);
    else
        flags(SR_N | SR_C | SR_DC | SR_OV | SR_Z, (u16)(f | ((r & m) ? 0 : SR_Z)));
    return (u16)(r & m);
}

/* a - b - !cin: as a + ~b + cin, C the carry (no borrow) */
static u16 sub(u32 a, u32 b, u32 cin, int n, int sticky)
{
    u32 m = n == 2 ? 0xffff : 0xff;
    return add(a, ~b & m, cin, n, sticky);
}

static int carry(void)
{
    return ps->sr & SR_C ? 1 : 0;
}

/* ---- branches ---------------------------------------------------------------------- */

/* a branch taken: two cycles, a PIC24E's four */
static void jump(u32 to)
{
    ps->npc = to & 0x7ffffe;
    ps->extra += ps->e ? 3 : 1;
}

static int cond(int c)
{
    int C = (ps->sr & SR_C) != 0, Z = (ps->sr & SR_Z) != 0;
    int N = (ps->sr & SR_N) != 0, OV = (ps->sr & SR_OV) != 0;
    switch (c) {
    case 0x0: return OV;
    case 0x1: return C;
    case 0x2: return Z;
    case 0x3: return N;
    case 0x4: return Z || (N != OV);
    case 0x5: return N != OV;
    case 0x6: return !C || Z;
    case 0x7: return 1;
    case 0x8: return !OV;
    case 0x9: return !C;
    case 0xa: return !Z;
    case 0xb: return !N;
    case 0xc: return !Z && N == OV;
    case 0xd: return N == OV;
    case 0xe: return C && !Z;
    }
    return 0;
}

static int two_word(u32 i)
{
    u32 op = i >> 16 & 0xff;
    return op == 0x02 || op == 0x04 || (op == 0x08 && ps->dsp);
}

/* skip the next instruction: one cycle more, two over a two-word one */
static void skip(void)
{
    u32 next = pic24_prog(ps->sim, ps->npc);
    int words = two_word(next) ? 2 : 1;
    ps->npc = (ps->npc + 2 * (u32)words) & 0x7ffffe;
    ps->extra += words;
}

/* a call: the return address stacked (a PIC24E's with CORCON.SFA in its
 * bit 0, which the call clears) */
static int call(u32 to, u32 ret)
{
    u16 sfa = ps->e && (ps->corcon & CORCON_SFA) ? 1 : 0;
    if (push((u16)(ret | sfa)) || push((u16)(ret >> 16 & 0x7f)))
        return 0;
    if (ps->e)
        ps->corcon &= (u16)~CORCON_SFA;
    jump(to);
    return 1;
}

/* a return's address, popped: a PIC24E's CORCON.SFA from its bit 0 */
static void return_to(u16 hi, u16 lo)
{
    if (ps->e)
        ps->corcon = (u16)((ps->corcon & ~CORCON_SFA) | ((lo & 1) ? CORCON_SFA : 0));
    ps->npc = ((u32)(hi & 0x7f) << 16 | lo) & 0x7ffffe;
    ps->extra += ps->e ? 5 : 2;
}

static u32 sext(u32 v, int bits)
{
    u32 s = 1u << (bits - 1);
    return ((v & ((s << 1) - 1)) ^ s) - s;
}

/* ---- the instruction set ------------------------------------------------------------- */

#define F(hi, lo) (i >> (lo) & ((1u << ((hi) - (lo) + 1)) - 1))

/* the ALU's two-operand operations (ADD..IOR in their Wb,Ws,Wd, f and
 * literal forms): op 0 ADD 1 ADDC 2 SUB 3 SUBB 4 AND 5 XOR 6 IOR 7 SUBR
 * 8 SUBBR; a the first operand (Wb, f), b the second (Ws, WREG, lit) */
static u16 alu(int op, u32 a, u32 b, int n)
{
    u32 m = n == 2 ? 0xffff : 0xff;
    u32 r;
    switch (op) {
    case 0: return add(a, b, 0, n, 0);
    case 1: return add(a, b, (u32)carry(), n, 1);
    case 2: return sub(a, b, 1, n, 0);
    case 3: return sub(a, b, (u32)carry(), n, 1);
    case 7: return sub(b, a, 1, n, 0);
    case 8: return sub(b, a, (u32)carry(), n, 1);
    case 4: r = a & b; break;
    case 5: r = a ^ b; break;
    default: r = a | b; break;
    }
    flags(SR_N | SR_Z, nz(r, n));
    return (u16)(r & m);
}

/* Wb, Ws, Wd and Wb, #lit5, Wd: 0www wBqq qddd dppp ssss */
static int alu_w(int op, u32 i)
{
    int n = F(14, 14) ? 1 : 2, wb = (int)F(18, 15);
    u16 b, a = n == 2 ? ps->w[wb] : (u16)(ps->w[wb] & 0xff);
    int pm = (int)F(6, 4);
    if ((pm & 6) == 6)
        b = (u16)F(4, 0);
    else if (src(pm, (int)F(3, 0), -1, n, &b))
        return 1;
    u16 r = alu(op, a, b, n);
    dst((int)F(13, 11), (int)F(10, 7), -1, n, r);
    return 1;
}

/* f {,WREG}: B D f(13) */
static int alu_f(int op, u32 i)
{
    int n = F(14, 14) ? 1 : 2;
    u32 f = F(12, 0);
    u16 a, wreg = n == 2 ? ps->w[0] : (u16)(ps->w[0] & 0xff);
    if (dread(f, n, &a))
        return 1;
    u16 r = alu(op, a, wreg, n);
    if (F(13, 13))
        dwrite(f, n, r);
    else
        dst(0, 0, -1, n, r);
    return 1;
}

/* #lit10, Wn: B k(10) dddd */
static int alu_lit(int op, u32 i)
{
    int n = F(14, 14) ? 1 : 2, d = (int)F(3, 0);
    u32 k = F(13, 4);
    if (n == 1)
        k &= 0xff;
    u16 a = n == 2 ? ps->w[d] : (u16)(ps->w[d] & 0xff);
    dst(0, d, -1, n, alu(op, a, k, n));
    return 1;
}

/* the one-operand operations on Ws -> Wd and on f: INC, INC2, DEC, DEC2,
 * NEG, COM, the shifts and rotates by one */
enum { U_INC, U_INC2, U_DEC, U_DEC2, U_NEG, U_COM, U_SL, U_ASR, U_LSR, U_RLC,
       U_RLNC, U_RRC, U_RRNC };

static u16 unary(int op, u32 a, int n)
{
    u32 m = n == 2 ? 0xffff : 0xff, top = n == 2 ? 0x8000 : 0x80, r;
    a &= m;
    switch (op) {
    case U_INC: return add(a, 1, 0, n, 0);
    case U_INC2: return add(a, 2, 0, n, 0);
    case U_DEC: return sub(a, 1, 1, n, 0);
    case U_DEC2: return sub(a, 2, 1, n, 0);
    case U_NEG: return sub(0, a, 1, n, 0);
    case U_COM:
        r = ~a & m;
        flags(SR_N | SR_Z, nz(r, n));
        return (u16)r;
    case U_SL:
        r = (a << 1) & m;
        flags(SR_N | SR_Z | SR_C, (u16)(nz(r, n) | ((a & top) ? SR_C : 0)));
        return (u16)r;
    case U_ASR:
        r = (a >> 1) | (a & top);
        flags(SR_N | SR_Z | SR_C, (u16)(nz(r, n) | ((a & 1) ? SR_C : 0)));
        return (u16)r;
    case U_LSR:
        r = a >> 1;
        flags(SR_N | SR_Z | SR_C, (u16)(nz(r, n) | ((a & 1) ? SR_C : 0)));
        return (u16)r;
    case U_RLC:
        r = ((a << 1) | (u32)carry()) & m;
        flags(SR_N | SR_Z | SR_C, (u16)(nz(r, n) | ((a & top) ? SR_C : 0)));
        return (u16)r;
    case U_RLNC:
        r = ((a << 1) | ((a & top) ? 1 : 0)) & m;
        flags(SR_N | SR_Z, nz(r, n));
        return (u16)r;
    case U_RRC:
        r = (a >> 1) | (carry() ? top : 0);
        flags(SR_N | SR_Z | SR_C, (u16)(nz(r, n) | ((a & 1) ? SR_C : 0)));
        return (u16)r;
    default:                                    /* RRNC */
        r = (a >> 1) | ((a & 1) ? top : 0);
        flags(SR_N | SR_Z, nz(r, n));
        return (u16)r;
    }
}

/* Ws -> Wd: Bqq qddd dppp ssss */
static int unary_w(int op, u32 i)
{
    int n = F(14, 14) ? 1 : 2;
    u16 a;
    if (src((int)F(6, 4), (int)F(3, 0), -1, n, &a))
        return 1;
    dst((int)F(13, 11), (int)F(10, 7), -1, n, unary(op, a, n));
    return 1;
}

/* f {,WREG}: B D f(13) */
static int unary_f(int op, u32 i)
{
    int n = F(14, 14) ? 1 : 2;
    u32 f = F(12, 0);
    u16 a;
    if (dread(f, n, &a))
        return 1;
    u16 r = unary(op, a, n);
    if (F(13, 13))
        dwrite(f, n, r);
    else
        dst(0, 0, -1, n, r);
    return 1;
}

/* SL, ASR, LSR of Wb by #lit4 or by Wns (its 5 low bits; past 15, all
 * shifted out): N and Z */
static int shift_by(int op, u32 i)
{
    u32 a = ps->w[F(14, 11)];
    u32 k = F(6, 6) ? F(3, 0) : (ps->w[F(3, 0)] & 0x1fu), r;
    if (op == U_SL)
        r = k > 15 ? 0 : (a << k) & 0xffff;
    else if (op == U_LSR)
        r = k > 15 ? 0 : a >> k;
    else
        r = (u32)((s32)(int16_t)a >> (k > 15 ? 15 : k)) & 0xffff;
    flags(SR_N | SR_Z, nz(r, 2));
    set_w((int)F(10, 7), (u16)r);
    return 1;
}

static int mul(u32 i, int su_a, int su_b)
{
    u16 b;
    int pm = (int)F(6, 4), d = (int)F(10, 7);
    if ((pm & 6) == 6)
        b = (u16)F(4, 0);
    else if (src(pm, (int)F(3, 0), -1, 2, &b))
        return 1;
    u16 a = ps->w[F(14, 11)];
    s64 x = su_a ? (s64)(int16_t)a : (s64)a;
    s64 y = su_b && (pm & 6) != 6 ? (s64)(int16_t)b : (s64)b;
    u32 p = (u32)(x * y);
    if (d >= 14 && ps->dsp) {                   /* MUL.xx Wb, Ws, Acc */
        s64 q = x * y;
        pic24_dsp_mul_acc(d - 14, (ps->corcon & CORCON_IF) ? q : 2 * q);
        return 1;
    }
    if ((d & 1) && d <= 13 && ps->e) {          /* MULW: Wnd = the low word */
        set_w(d - 1, (u16)p);
        return 1;
    }
    if (d & 1 || d > 12)
        return bad_opcode();
    set_w(d, (u16)p);
    set_w(d + 1, (u16)(p >> 16));
    return 1;
}

/* DIVF Wm, Wn: Wm and Wn 1.15 fractions, the quotient 1.15 (Wm * 2^15 /
 * Wn) in W0, the remainder in W1; computed on the first of its 18
 * executions as DIV's are: 1101 1001 0ttt t000 0000 ssss */
static int divf(u32 i)
{
    int first = !(ps->sr & SR_RA) || ps->rcount == 17;
    if ((i & 0x8780) || !ps->dsp)
        return bad_opcode();
    ps->changed = 1;
    if (!first)
        return 1;
    s64 a = (int16_t)ps->w[F(14, 11)], b = (int16_t)ps->w[F(3, 0)];
    if (!b) {
        pic24_core_math(PIC24_MATH_DIV0);
        return 1;
    }
    s64 num = a * 32768, q = num / b, r = num % b;
    set_w(0, (u16)q);
    set_w(1, (u16)r);
    flags(SR_N | SR_Z | SR_OV | SR_C, (u16)((r < 0 ? SR_N : 0) | (r ? 0 : SR_Z) |
                                            (q > 32767 || q < -32768 ? SR_OV : 0)));
    return 1;
}

/* DO #lit15 or Wn, Expr: the loop's registers to the DO stack, the new
 * loop's in them, DL one more, SR.DA */
static int do_loop(u32 i)
{
    if (!ps->dsp || (F(15, 15) && (i & 0x7ff0)))
        return bad_opcode();
    int dl = ps->corcon >> 8 & 7;
    if (dl >= PIC24_DO_LEVELS) {
        trap(TRAP_SOFT, "a DO loop nested past the DO stack's four levels", ~0u);
        return 1;
    }
    u32 w2 = pic24_prog(ps->sim, ps->pc + 2);
    ps->dostack[dl].dcount = ps->dcount;
    ps->dostack[dl].dostart = ps->dostart;
    ps->dostack[dl].doend = ps->doend;
    ps->dcount = F(15, 15) ? ps->w[i & 15] : (u16)(i & 0x7fff);
    ps->dostart = (ps->pc + 4) & 0x7ffffe;
    ps->doend = (ps->pc + 4 + 2 * sext(w2 & 0xffff, 16)) & 0x7ffffe;
    ps->corcon = (u16)((ps->corcon & ~CORCON_DL) | (u32)(dl + 1) << 8);
    ps->sr |= SR_DA;
    ps->edt = 0;
    ps->changed = 1;
    ps->extra++;
    return 1;
}

/* the last instruction of the DO loop done: again from DOSTART while
 * DCOUNT is not 0 (taking one from it), else the loop ends and the outer
 * loop's registers come back */
static void do_end(void)
{
    if (ps->dcount && !ps->edt) {
        ps->dcount--;
        ps->npc = ps->dostart;
        return;
    }
    int dl = ps->corcon >> 8 & 7;
    if (dl)
        dl--;
    ps->dcount = ps->dostack[dl].dcount;
    ps->dostart = ps->dostack[dl].dostart;
    ps->doend = ps->dostack[dl].doend;
    ps->corcon = (u16)((ps->corcon & ~CORCON_DL) | (u32)dl << 8);
    if (!dl)
        ps->sr &= (u16)~SR_DA;
    ps->edt = 0;
}

/* DIV.S, DIV.U and their .D forms: Uttt tvvv vW00 ssss */
static int divide(u32 i)
{
    int uns = (int)F(15, 15), dbl = (int)F(6, 6);
    int m = (int)F(10, 7), mh = (int)F(14, 11), dn = (int)F(3, 0);
    int first = !(ps->sr & SR_RA) || ps->rcount == 17;
    ps->changed = 1;
    if (!first)
        return 1;
    u32 div = ps->w[dn];
    u32 lo = ps->w[m], hi = dbl ? ps->w[mh] : uns ? 0 : ((lo & 0x8000) ? 0xffff : 0);
    if (!div) {
        pic24_core_math(PIC24_MATH_DIV0);
        return 1;
    }
    u32 q, r;
    int ov;
    if (uns) {
        u32 n = hi << 16 | lo;
        q = n / div;
        r = n % div;
        ov = q > 0xffff;
    } else {
        s32 n = (s32)(hi << 16 | lo), dv = (int16_t)div;
        s64 qq = (s64)n / dv, rr = (s64)n % dv;
        q = (u32)qq;
        r = (u32)rr;
        ov = qq > 32767 || qq < -32768;
        if (!dbl && n == -32768 && dv == -1)
            q = 0x8000, r = 0, ov = 1;          /* the one word overflow */
    }
    set_w(0, (u16)q);
    set_w(1, (u16)r);
    flags(SR_N | SR_Z | SR_OV | SR_C,
          (u16)((!uns && (r & 0x8000) ? SR_N : 0) | ((r & 0xffff) ? 0 : SR_Z) |
                (ov ? SR_OV : 0)));
    return 1;
}

/* MOV [Ws+Slit10], Wnd and MOV Wns, [Wd+Slit10]: kkkk B kkk dddd kkk ssss */
static int mov_offset(u32 i, int store)
{
    int n = F(14, 14) ? 1 : 2, d = (int)F(10, 7), s = (int)F(3, 0);
    u32 k = F(18, 15) << 6 | F(13, 11) << 3 | F(6, 4);
    u32 off = sext(k, 10) * (u32)n;
    u16 v;
    if (store) {
        if (!pointer(d))
            return 1;
        u32 ea = (u16)(ps->w[d] + off);
        if (ps->dsp)
            ea = pic24_agu_offset(d, (u16)ea, (off & 0x8000) != 0, 0);
        if (d == 15 && stack_check(ea))
            return 1;
        dwrite(ea, n, n == 2 ? ps->w[s] : (u16)(ps->w[s] & 0xff));
        return 1;
    }
    if (!pointer(s))
        return 1;
    u32 ea = (u16)(ps->w[s] + off);
    if (ps->dsp)
        ea = pic24_agu_offset(s, (u16)ea, (off & 0x8000) != 0, 0);
    if (s == 15 && stack_check(ea))
        return 1;
    if (dread(ea, n, &v))
        return 1;
    dst(0, d, -1, n, v);
    return 1;
}

/* bit operations on Ws: bbbb ... (BSET, BCLR, BTG, BTST, BTSTS, BTSS,
 * BTSC; the bit in Wb for BTST Ws,Wb and BSW) */
static int bit_w(u32 op8, u32 i)
{
    int pm = (int)F(6, 4), s = (int)F(3, 0), n = 2, bit;
    int zflag = op8 == 0xa3 || op8 == 0xa4 ? (int)F(11, 11) : 1;
    if (op8 == 0xa5 || op8 == 0xad) {           /* BTST Ws,Wb; BSW */
        bit = ps->w[F(14, 11)] & 15;
        zflag = (int)F(15, 15);
    } else {
        bit = (int)F(15, 12);
        if (op8 <= 0xa2 && F(10, 10))
            n = 1;
    }
    if (n == 1 && bit > 7)
        return bad_opcode();
    /* the operand once: read, then written back where the operation writes */
    u32 ea = 0;
    u16 v;
    if (pm) {
        if (ea_of(pm, s, -1, n, &ea, 0) || dread(ea, n, &v))
            return 1;
    } else
        v = n == 2 ? ps->w[s] : (u16)(ps->w[s] & 0xff);
    int b = v >> bit & 1, wr = 0;
    switch (op8) {
    case 0xa0: v = (u16)(v | 1u << bit); wr = 1; break;      /* BSET */
    case 0xa1: v = (u16)(v & ~(1u << bit)); wr = 1; break;   /* BCLR */
    case 0xa2: v = (u16)(v ^ 1u << bit); wr = 1; break;      /* BTG */
    case 0xa3: case 0xa5:                                    /* BTST */
        if (zflag)
            flags(SR_Z, b ? 0 : SR_Z);
        else
            flags(SR_C, b ? SR_C : 0);
        break;
    case 0xa4:                                               /* BTSTS */
        if (zflag)
            flags(SR_Z, b ? 0 : SR_Z);
        else
            flags(SR_C, b ? SR_C : 0);
        v = (u16)(v | 1u << bit);
        wr = 1;
        break;
    case 0xa6: if (b) skip(); break;                         /* BTSS */
    case 0xa7: if (!b) skip(); break;                        /* BTSC */
    case 0xad: {                                             /* BSW: C, or Z's complement */
        int c = zflag ? !(ps->sr & SR_Z) : (ps->sr & SR_C) != 0;
        v = (u16)((v & ~(1u << bit)) | (u32)c << bit);
        wr = 1;
        break;
    }
    }
    if (!wr)
        return 1;
    if (pm)
        dwrite(ea, n, v);
    else
        dst(0, s, -1, n, v);
    return 1;
}

/* bit operations on f: bbb f(12) b -- a word, the bit 0-15: its low
 * three bits at 15:13, its fourth at 0 (what XC16 assembles) */
static int bit_f(u32 op8, u32 i)
{
    u32 f = i & 0x1ffe;
    int bit = (int)(F(15, 13) | (i & 1) << 3);
    u16 v;
    if (dread(f, 2, &v))
        return 1;
    int b = v >> bit & 1;
    switch (op8) {
    case 0xa8: dwrite(f, 2, (u16)(v | 1u << bit)); break;
    case 0xa9: dwrite(f, 2, (u16)(v & ~(1u << bit))); break;
    case 0xaa: dwrite(f, 2, (u16)(v ^ 1u << bit)); break;
    case 0xab: flags(SR_Z, b ? 0 : SR_Z); break;
    case 0xac:
        flags(SR_Z, b ? 0 : SR_Z);
        dwrite(f, 2, (u16)(v | 1u << bit));
        break;
    case 0xae: if (b) skip(); break;
    case 0xaf: if (!b) skip(); break;
    }
    return 1;
}

/* TBLRDL, TBLRDH: Bqq qddd dppp ssss */
static int table_read(u32 i, int high)
{
    int n = F(14, 14) ? 1 : 2, pm = (int)F(6, 4), s = (int)F(3, 0);
    u32 ea;
    ps->extra++;
    if (!pm || ea_of(pm, s, -1, n, &ea, 0))
        return pm ? 1 : bad_opcode();
    if (ps->e)
        ps->extra += 3;
    u32 p = (u32)ps->tblpag << 16 | ea;
    u32 word = pic24_prog(ps->sim, p);
    u16 v;
    if (high)
        v = n == 2 ? (u16)(word >> 16 & 0xff) : (u16)((p & 1) ? 0 : word >> 16 & 0xff);
    else
        v = n == 2 ? (u16)word : (u16)((p & 1) ? word >> 8 & 0xff : word & 0xff);
    dst((int)F(13, 11), (int)F(10, 7), -1, n, v);
    return 1;
}

/* TBLWTL, TBLWTH: to the flash controller's write latches (TBLPAG
 * 0xFA); elsewhere nothing, but the operands' address modifications */
static int table_write(u32 i)
{
    int n = F(14, 14) ? 1 : 2, qm = (int)F(13, 11), d = (int)F(10, 7);
    u16 v;
    u32 ea;
    ps->extra++;
    if (src((int)F(6, 4), (int)F(3, 0), -1, n, &v))
        return 1;
    if (!qm)
        return bad_opcode();
    if (ea_of(qm, d, -1, n, &ea, 0))
        return 1;
    if (ps->e)
        ps->extra += 3;
    u32 p = (u32)ps->tblpag << 16 | ea;
    if ((p & 0xffff00u) == PIC24_LATCHES && ps->nvm)
        ps->nvm->latch_write(ps->nvm->ctx, p, (int)F(15, 15), n, v);
    return 1;
}

static int retfie(void)
{
    u16 hi, lo;
    if (pop(&hi) || pop(&lo))
        return 1;
    ps->sr = (u16)((ps->sr & 0xff00) | (hi >> 8));
    ps->corcon = (u16)((ps->corcon & ~CORCON_IPL3) | ((hi & 0x80) ? CORCON_IPL3 : 0));
    return_to(hi, lo);
    irq_changed();
    if (ps->sim->an)
        an_exc_return(ps->sim);
    return 1;
}

static int ret(void)
{
    u16 hi, lo;
    if (pop(&hi) || pop(&lo))
        return 1;
    return_to(hi, lo);
    return 1;
}

static int pwrsav(void)
{
    struct sim *s = ps->sim;
    u32 left;
    int irq;
    for (;;) {
        int pr = ps->intc ? ps->intc->request(ps->intc->ctx, &irq) : 0;
        if (pr)
            break;                              /* it wakes; taken if above ipl */
        if (!sim_next_event(s, &left)) {
            if (s->never_idle)
                die("part %d: not modelled: asleep (PWRSAV) while the run's other parts go on",
                    s->part);
            sim_end(s, END_IDLE, "asleep (PWRSAV) with nothing to wake it, "
                    "at 0x%06x", (unsigned)ps->pc);
            return 0;
        }
        if (!left)
            left = 1;
        s->cycles += left;
        sim_advance(s, left);
    }
    return 1;
}

static int exec(u32 i)
{
    u32 op8 = i >> 16;
    int n;
    u16 v;
    switch (op8) {
    case 0x00:                                  /* NOP */
        return 1;
    case 0x01: {
        /* the PIC24E's RCALL, GOTO and BRA Wn: 0x0200, 0x0400, 0x0600 */
        u32 k = i & 0xfff0;
        if (ps->e && (k & 0x8000)) {            /* CALL.L, GOTO.L Wn */
            int lo = (int)(i & 15), hi = (int)F(14, 11);
            if ((i & 0x03f0) || (lo & 1) || hi != lo + 1)
                return bad_opcode();
            u32 to = (u32)(ps->w[hi] & 0x7f) << 16 | ps->w[lo];
            if (F(10, 10))
                jump(to);
            else
                call(to, ps->pc + 2);
            return 1;
        }
        if (ps->e)
            k = k == 0x0200 ? 0x2000 : k == 0x0400 ? 0x4000 : k == 0x0600 ? 0x6000 :
                k == 0x0000 ? 0 : 1;
        switch (k) {
        case 0x0000:                            /* CALL Wn */
            call(ps->w[i & 15], ps->pc + 2);
            return 1;
        case 0x2000:                            /* RCALL Wn */
            call(ps->pc + 2 + 2 * sext(ps->w[i & 15], 16), ps->pc + 2);
            return 1;
        case 0x4000:                            /* GOTO Wn */
            jump(ps->w[i & 15]);
            return 1;
        case 0x6000:                            /* BRA Wn */
            jump(ps->pc + 2 + 2 * sext(ps->w[i & 15], 16));
            return 1;
        }
        return bad_opcode();
    }
    case 0x02: {                                /* CALL lit23 */
        u32 w2 = pic24_prog(ps->sim, ps->pc + 2);
        call((i & 0xfffe) | (w2 & 0x7f) << 16, ps->pc + 4);
        return 1;
    }
    case 0x04: {                                /* GOTO lit23 */
        u32 w2 = pic24_prog(ps->sim, ps->pc + 2);
        jump((i & 0xfffe) | (w2 & 0x7f) << 16);
        return 1;
    }
    case 0x05: {                                /* RETLW #lit10, Wn */
        n = F(14, 14) ? 1 : 2;
        u32 k = F(13, 4);
        u16 hi, lo;
        if (pop(&hi) || pop(&lo))
            return 1;
        dst(0, (int)F(3, 0), -1, n, (u16)(n == 1 ? k & 0xff : k));
        return_to(hi, lo);
        return 1;
    }
    case 0x06:
        if (i == 0x060000)                      /* RETURN */
            return ret();
        if (i == 0x064000)                      /* RETFIE */
            return retfie();
        return bad_opcode();
    case 0x07:                                  /* RCALL */
        call(ps->pc + 2 + 2 * sext(i & 0xffff, 16), ps->pc + 2);
        return 1;
    case 0x08:                                  /* DO */
        return do_loop(i);
    case 0x09:                                  /* REPEAT */
        if (F(15, 15)) {
            if ((i & 0x7ff0) != 0)
                return bad_opcode();
            ps->rcount = (u16)(ps->e ? ps->w[i & 15] : ps->w[i & 15] & 0x3fff);
        } else if (ps->e) {
            ps->rcount = (u16)(i & 0x7fff);     /* #lit15 */
        } else {
            if (F(14, 14))
                return bad_opcode();
            ps->rcount = (u16)(i & 0x3fff);
        }
        if (ps->rcount)
            ps->sr |= SR_RA;
        ps->changed = 1;
        return 1;
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
        return alu_w(7, i);                     /* SUBR */
    case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x1d: case 0x1e: case 0x1f:
        return alu_w(8, i);                     /* SUBBR */
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
        return alu_w(0, i);
    case 0x48: case 0x49: case 0x4a: case 0x4b:
    case 0x4c: case 0x4d: case 0x4e: case 0x4f:
        return alu_w(1, i);
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        return alu_w(2, i);
    case 0x58: case 0x59: case 0x5a: case 0x5b:
    case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        return alu_w(3, i);
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
        return alu_w(4, i);
    case 0x68: case 0x69: case 0x6a: case 0x6b:
    case 0x6c: case 0x6d: case 0x6e: case 0x6f:
        return alu_w(5, i);
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
        return alu_w(6, i);
    case 0x78: case 0x79: case 0x7a: case 0x7b:
    case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
        /* MOV Ws, Wd: 0111 1www wBhh hddd dggg ssss */
        int wb = (int)F(18, 15);
        n = F(14, 14) ? 1 : 2;
        if (src((int)F(6, 4), (int)F(3, 0), wb, n, &v))
            return 1;
        dst((int)F(13, 11), (int)F(10, 7), wb, n, v);
        return 1;
    }
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
        if (!dread(F(18, 4) << 1, 2, &v))      /* MOV f, Wnd */
            set_w((int)F(3, 0), v);
        return 1;
    case 0x88: case 0x89: case 0x8a: case 0x8b:
    case 0x8c: case 0x8d: case 0x8e: case 0x8f:
        dwrite(F(18, 4) << 1, 2, ps->w[F(3, 0)]); /* MOV Wns, f */
        return 1;
    case 0x90: case 0x91: case 0x92: case 0x93:
    case 0x94: case 0x95: case 0x96: case 0x97:
        return mov_offset(i, 0);
    case 0x98: case 0x99: case 0x9a: case 0x9b:
    case 0x9c: case 0x9d: case 0x9e: case 0x9f:
        return mov_offset(i, 1);
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
    case 0xa5: case 0xa6: case 0xa7: case 0xad:
        if ((i & (op8 <= 0xa2 ? 0x0b80u : op8 == 0xa6 || op8 == 0xa7 ? 0x0f80u
                                                                     : 0x0780u)) != 0)
            return bad_opcode();
        return bit_w(op8, i);
    case 0xa8: case 0xa9: case 0xaa: case 0xab:
    case 0xac: case 0xae: case 0xaf:
        return bit_f(op8, i);
    case 0xb0:                                  /* ADD, ADDC #lit10, Wn */
        return alu_lit(F(15, 15) ? 1 : 0, i);
    case 0xb1:                                  /* SUB, SUBB #lit10, Wn */
        return alu_lit(F(15, 15) ? 3 : 2, i);
    case 0xb2:                                  /* AND, XOR #lit10, Wn */
        return alu_lit(F(15, 15) ? 5 : 4, i);
    case 0xb3:
        if (!F(15, 15))                         /* IOR #lit10, Wn */
            return alu_lit(6, i);
        if (F(15, 12) == 0xc) {                 /* MOV.B #lit8, Wnd */
            dst(0, (int)F(3, 0), -1, 1, (u16)F(11, 4));
            return 1;
        }
        return bad_opcode();
    case 0xb4:
        return alu_f(F(15, 15) ? 1 : 0, i);
    case 0xb5:
        return alu_f(F(15, 15) ? 3 : 2, i);
    case 0xb6:
        return alu_f(F(15, 15) ? 5 : 4, i);
    case 0xb7:
        if (!F(15, 15))
            return alu_f(6, i);
        if (F(13, 13)) {                        /* MOV WREG, f */
            n = F(14, 14) ? 1 : 2;
            dwrite(F(12, 0), n, n == 2 ? ps->w[0] : (u16)(ps->w[0] & 0xff));
            return 1;
        }
        return bad_opcode();
    case 0xb8:                                  /* MUL.UU, MUL.US */
        return mul(i, 0, F(15, 15) ? 1 : 0);
    case 0xb9:                                  /* MUL.SU, MUL.SS */
        return mul(i, 1, F(15, 15) ? 1 : 0);
    case 0xba:
        return table_read(i, (int)F(15, 15));
    case 0xbb:
        return table_write(i);
    case 0xbc: {                                /* MUL f */
        if (F(15, 15) || F(13, 13))
            return bad_opcode();
        n = F(14, 14) ? 1 : 2;
        if (dread(F(12, 0), n, &v))
            return 1;
        u32 a = n == 2 ? ps->w[0] : ps->w[0] & 0xffu;
        u32 p = a * v;
        set_w(2, (u16)p);
        if (n == 2)
            set_w(3, (u16)(p >> 16));
        return 1;
    }
    case 0xbd:                                  /* SUBR, SUBBR f */
        return alu_f(F(15, 15) ? 8 : 7, i);
    case 0xbe: {                                /* MOV.D */
        ps->extra++;
        if (!F(15, 15)) {                       /* MOV.D Ws, Wnd */
            int d = (int)F(10, 8) * 2, pm = (int)F(6, 4), s = (int)F(3, 0);
            u16 lo, hi;
            if ((i & 0x7880) != 0)
                return bad_opcode();
            if (!pm) {
                if (s & 1)
                    return bad_opcode();
                lo = ps->w[s];
                hi = ps->w[s + 1];
            } else {
                u32 ea;
                if (!pointer(s))
                    return 1;
                u16 w = ps->w[s];
                switch (pm) {
                case 1: ea = w; break;
                case 2: ea = w; set_w(s, (u16)(w - 4)); break;
                case 3: ea = w; set_w(s, (u16)(w + 4)); break;
                case 4: set_w(s, (u16)(w - 4)); ea = ps->w[s]; break;
                case 5: set_w(s, (u16)(w + 4)); ea = ps->w[s]; break;
                default: return bad_opcode();
                }
                if (s == 15 && stack_check(ea))
                    return 1;
                if (dread(ea, 2, &lo) || dread((u16)(ea + 2), 2, &hi))
                    return 1;
            }
            set_w(d, lo);
            set_w(d + 1, hi);
            return 1;
        }
        /* MOV.D Wns, Wd: 1 0qqq dddd 0000 sss0 */
        int qm = (int)F(13, 11), d = (int)F(10, 7), s = (int)F(3, 1) * 2;
        if (F(14, 14) || (i & 0x71) != 0)
            return bad_opcode();
        u16 lo = ps->w[s], hi = ps->w[s + 1];
        if (!qm) {
            if (d & 1)
                return bad_opcode();
            set_w(d, lo);
            set_w(d + 1, hi);
            return 1;
        }
        u32 ea;
        if (!pointer(d))
            return 1;
        u16 w = ps->w[d];
        switch (qm) {
        case 1: ea = w; break;
        case 2: ea = w; set_w(d, (u16)(w - 4)); break;
        case 3: ea = w; set_w(d, (u16)(w + 4)); break;
        case 4: set_w(d, (u16)(w - 4)); ea = ps->w[d]; break;
        case 5: set_w(d, (u16)(w + 4)); ea = ps->w[d]; break;
        default: return bad_opcode();
        }
        if (d == 15 && (stack_check(ea) || stack_check(ea + 2)))
            return 1;
        if (!dwrite(ea, 2, lo))
            dwrite((u16)(ea + 2), 2, hi);
        return 1;
    }
    case 0xbf:                                  /* MOV f {,WREG} */
        if (!F(15, 15))
            return bad_opcode();
        n = F(14, 14) ? 1 : 2;
        if (dread(F(12, 0), n, &v))
            return 1;
        flags(SR_N | SR_Z, nz(v, n));
        if (F(13, 13))
            dwrite(F(12, 0), n, v);
        else
            dst(0, 0, -1, n, v);
        return 1;
    case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5:
    case 0xc6: case 0xc7: case 0xc8: case 0xc9: case 0xca: case 0xcb:
    case 0xcc: case 0xcd: case 0xce: case 0xf0: case 0xf1: case 0xf2:
    case 0xf3:
        return ps->dsp ? pic24_dsp_exec(i) : bad_opcode();
    case 0xd9:                                  /* DIVF */
        return divf(i);
    case 0xcf: {                                /* FF1R, FF1L */
        if ((i & 0x7800) != 0)
            return bad_opcode();
        if (src((int)F(6, 4), (int)F(3, 0), -1, 2, &v))
            return 1;
        int r = 0;
        if (v) {
            if (F(15, 15))
                for (r = 1; !(v & 0x8000); r++)
                    v = (u16)(v << 1);
            else
                for (r = 1; !(v & 1); r++)
                    v = (u16)(v >> 1);
        }
        flags(SR_C, r ? 0 : SR_C);
        set_w((int)F(10, 7), (u16)r);
        return 1;
    }
    case 0xd0:
        return F(15, 15) ? bad_opcode() : unary_w(U_SL, i);
    case 0xd1:
        return unary_w(F(15, 15) ? U_ASR : U_LSR, i);
    case 0xd2:
        return unary_w(F(15, 15) ? U_RLC : U_RLNC, i);
    case 0xd3:
        return unary_w(F(15, 15) ? U_RRC : U_RRNC, i);
    case 0xd4:
        return F(15, 15) ? bad_opcode() : unary_f(U_SL, i);
    case 0xd5:
        return unary_f(F(15, 15) ? U_ASR : U_LSR, i);
    case 0xd6:
        return unary_f(F(15, 15) ? U_RLC : U_RLNC, i);
    case 0xd7:
        return unary_f(F(15, 15) ? U_RRC : U_RRNC, i);
    case 0xd8:
        if ((i & 0x30) != 0)
            return bad_opcode();
        return divide(i);
    case 0xdd:                                  /* SL Wb, #lit4 or Wns */
        if (F(15, 15) || (i & 0x30) != 0)
            return bad_opcode();
        return shift_by(U_SL, i);
    case 0xde:                                  /* ASR, LSR Wb, #lit4 or Wns */
        if ((i & 0x30) != 0)
            return bad_opcode();
        return shift_by(F(15, 15) ? U_ASR : U_LSR, i);
    case 0xdf: {                                /* FBCL */
        if ((i & 0xf800) != 0)
            return bad_opcode();
        if (src((int)F(6, 4), (int)F(3, 0), -1, 2, &v))
            return 1;
        u16 sign = v & 0x8000, t = (u16)(v << 1);
        int sh = 0;
        while (sh < 15 && (t & 0x8000) == sign) {
            t = (u16)(t << 1);
            sh++;
        }
        flags(SR_C, sh == 15 ? SR_C : 0);
        set_w((int)F(10, 7), (u16)-sh);
        return 1;
    }
    case 0xe0:                                  /* CP0 Ws */
        if ((i & 0xfb80) != 0)
            return bad_opcode();
        n = F(10, 10) ? 1 : 2;
        if (!src((int)F(6, 4), (int)F(3, 0), -1, n, &v))
            sub(v, 0, 1, n, 0);
        return 1;
    case 0xe1: {                                /* CP, CPB Wb, Ws or #lit5 */
        int pm = (int)F(6, 4), cb = (int)F(15, 15);
        n = F(10, 10) ? 1 : 2;
        /* a PIC24E's literal is 8 bits: its top three at 9:7 */
        if ((i & 0x0380) != 0 && !(ps->e && (pm & 6) == 6))
            return bad_opcode();
        u16 a = n == 2 ? ps->w[F(14, 11)] : (u16)(ps->w[F(14, 11)] & 0xff), b;
        if ((pm & 6) == 6)
            b = (u16)(F(9, 7) << 5 | F(4, 0));
        else if (src(pm, (int)F(3, 0), -1, n, &b))
            return 1;
        sub(a, b, cb ? (u32)carry() : 1, n, cb);
        return 1;
    }
    case 0xe2:                                  /* CP0 f */
        if (F(15, 15) || F(13, 13))
            return bad_opcode();
        n = F(14, 14) ? 1 : 2;
        if (!dread(F(12, 0), n, &v))
            sub(v, 0, 1, n, 0);
        return 1;
    case 0xe3: {                                /* CP, CPB f */
        if (F(13, 13))
            return bad_opcode();
        n = F(14, 14) ? 1 : 2;
        int cb = (int)F(15, 15);
        u16 wreg = n == 2 ? ps->w[0] : (u16)(ps->w[0] & 0xff);
        if (!dread(F(12, 0), n, &v))
            sub(v, wreg, cb ? (u32)carry() : 1, n, cb);
        return 1;
    }
    case 0xe6: case 0xe7: {                     /* CPSGT, CPSLT, CPSNE, CPSEQ */
        /* a PIC24E's CPBGT, CPBLT, CPBNE, CPBEQ: the offset at 9:4, 1
         * the compare and skip */
        s32 off = (s32)sext(F(9, 4), 6);
        if ((i & 0x03f0) != 0 && !ps->e)
            return bad_opcode();
        n = F(10, 10) ? 1 : 2;
        u32 m = n == 2 ? 0xffff : 0xff;
        u32 a = ps->w[F(14, 11)] & m, b = ps->w[F(3, 0)] & m;
        s32 sa = (s32)sext(a, 8 * n), sb = (s32)sext(b, 8 * n);
        int t;
        if (op8 == 0xe6)
            t = F(15, 15) ? sa < sb : sa > sb;
        else
            t = F(15, 15) ? a == b : a != b;
        if (t && ps->e && off != 1) {
            jump(ps->pc + 2 + 2 * (u32)off);
            ps->extra++;                        /* five cycles taken */
        } else if (t)
            skip();
        return 1;
    }
    case 0xe8:
        return unary_w(F(15, 15) ? U_INC2 : U_INC, i);
    case 0xe9:
        return unary_w(F(15, 15) ? U_DEC2 : U_DEC, i);
    case 0xea:
        return unary_w(F(15, 15) ? U_COM : U_NEG, i);
    case 0xeb:                                  /* CLR, SETM Wd */
        if ((i & 0x7f) != 0)
            return bad_opcode();
        n = F(14, 14) ? 1 : 2;
        dst((int)F(13, 11), (int)F(10, 7), -1, n, F(15, 15) ? 0xffff : 0);
        return 1;
    case 0xec:
        return unary_f(F(15, 15) ? U_INC2 : U_INC, i);
    case 0xed:
        return unary_f(F(15, 15) ? U_DEC2 : U_DEC, i);
    case 0xee:
        return unary_f(F(15, 15) ? U_COM : U_NEG, i);
    case 0xef: {                                /* CLR, SETM f {,WREG} */
        n = F(14, 14) ? 1 : 2;
        u16 x = F(15, 15) ? 0xffff : 0;
        if (F(13, 13))
            dwrite(F(12, 0), n, x);
        else
            dst(0, 0, -1, n, x);
        return 1;
    }
    case 0xf8:                                  /* PUSH f */
        if (!dread(i & 0xfffe, 2, &v))
            push(v);
        return 1;
    case 0xf9:                                  /* POP f */
        if (!pop(&v))
            dwrite(i & 0xfffe, 2, v);
        return 1;
    case 0xfa:
        if (!F(15, 15)) {                       /* LNK #lit14 */
            if (F(14, 14) || (i & 1))
                return bad_opcode();
            if (push(ps->w[14]))
                return 1;
            set_w(14, ps->w[15]);
            set_w(15, (u16)(ps->w[15] + (i & 0x3ffe)));
            if (ps->e)
                ps->corcon |= CORCON_SFA;       /* a stack frame active */
            return 1;
        }
        if (i == 0xfa8000) {                    /* ULNK */
            set_w(15, ps->w[14]);
            if (!pop(&v))
                set_w(14, v);
            if (ps->e)
                ps->corcon &= (u16)~CORCON_SFA;
            return 1;
        }
        return bad_opcode();
    case 0xfb:                                  /* SE, ZE */
        if ((i & 0x7800) != 0)
            return bad_opcode();
        if (src((int)F(6, 4), (int)F(3, 0), -1, 1, &v))
            return 1;
        if (F(15, 15)) {
            flags(SR_N | SR_Z | SR_C, (u16)(SR_C | (v ? 0 : SR_Z)));
            set_w((int)F(10, 7), v);
        } else {
            u16 x = (v & 0x80) ? (u16)(v | 0xff00) : v;
            flags(SR_N | SR_Z | SR_C, (u16)(((x & 0x8000) ? SR_N : SR_C) |
                                            (x ? 0 : SR_Z)));
            set_w((int)F(10, 7), x);
        }
        return 1;
    case 0xfc:                                  /* DISI #lit14 */
        if (F(15, 14))
            return bad_opcode();
        ps->disicnt = (u16)(i & 0x3fff);
        ps->disi_at = ps->sim->cycles + 1;
        return 1;
    case 0xfd:
        if ((i & 0xf870) == 0) {                /* EXCH Wns, Wnd */
            int a = (int)F(3, 0), b = (int)F(10, 7);
            u16 t = ps->w[a];
            set_w(a, ps->w[b]);
            set_w(b, t);
            return 1;
        }
        if ((i & 0xfff0) == 0x4000) {           /* DAW.B */
            int r = (int)(i & 15);
            u32 x = ps->w[r] & 0xff, c = (u32)carry();
            if ((x & 15) > 9 || (ps->sr & SR_DC))
                x += 6;
            if ((x >> 4) > 9 || c)
                x += 0x60;
            flags(SR_C, (x > 0xff || c) ? SR_C : 0);
            set_w(r, (u16)((ps->w[r] & 0xff00) | (x & 0xff)));
            return 1;
        }
        if ((i & 0xbff0) == 0x8000) {           /* SWAP, SWAP.B */
            int r = (int)(i & 15);
            u16 x = ps->w[r];
            if (F(14, 14))
                x = (u16)((x & 0xff00) | (x << 4 & 0xf0) | (x >> 4 & 0x0f));
            else
                x = (u16)(x << 8 | x >> 8);
            set_w(r, x);
            return 1;
        }
        return bad_opcode();
    case 0xfe:
        if (ps->e && (i & 0xe000) == 0xc000) {  /* MOVPAG #lit or Wn */
            int pp = (int)F(11, 10);
            u32 k = F(9, 0);
            if (F(12, 12)) {
                if (i & 0x03f0)
                    return bad_opcode();
                k = ps->w[i & 15];
            }
            switch (pp) {
            case 0: ps->dsrpag = (u16)(k & 0x3ff); break;
            case 1: ps->dswpag = (u16)(k & 0x1ff); break;
            case 2: ps->tblpag = (u16)(k & 0xff); break;
            default: return bad_opcode();
            }
            ps->changed = 1;
            return 1;
        }
        switch (i & 0xffff) {
        case 0x0000:                            /* RESET */
            if (ps->sim->reset_runs_on)
                return software_reset();
            sim_end(ps->sim, END_EXIT, "RESET at 0x%06x", (unsigned)ps->pc);
            return 0;
        case 0x4000: case 0x4001:               /* PWRSAV #0, #1 */
            ps->npc = (ps->pc + 2) & 0x7ffffe;
            return pwrsav();
        case 0x6000:                            /* CLRWDT */
            return 1;
        case 0x8000:                            /* POP.S */
            for (int k = 0; k < 4; k++)
                set_w(k, ps->sw[k]);
            flags(SR_DC | SR_N | SR_OV | SR_Z | SR_C, ps->ssr);
            return 1;
        case 0xa000:                            /* PUSH.S */
            for (int k = 0; k < 4; k++)
                ps->sw[k] = ps->w[k];
            ps->ssr = ps->sr;
            return 1;
        }
        return bad_opcode();
    case 0xff:
        if (i == 0xffffff)                      /* NOPR */
            return 1;
        return bad_opcode();
    }
    if (op8 >= 0x20 && op8 <= 0x2f) {           /* MOV #lit16, Wnd */
        set_w((int)(i & 15), (u16)(i >> 4));
        return 1;
    }
    if (op8 >= 0x30 && op8 <= 0x3e) {           /* BRA cc */
        if (cond((int)(op8 & 15)))
            jump(ps->pc + 2 + 2 * sext(i & 0xffff, 16));
        return 1;
    }
    if (op8 >= 0x0c && op8 <= 0x0f && ps->dsp) {    /* BRA OA, OB, SA, SB */
        static const u16 bit[4] = { SR_OA, SR_OB, SR_SA, SR_SB };
        if (ps->sr & bit[op8 - 0x0c])
            jump(ps->pc + 2 + 2 * sext(i & 0xffff, 16));
        return 1;
    }
    return bad_opcode();
}

/* ---- exceptions -------------------------------------------------------------------- */

static void irq_changed(void)
{
    /* the next step looks; nothing is cached */
}

static int cpu_ipl(void)
{
    return (ps->corcon & CORCON_IPL3) ? 8 + (ps->sr >> 5 & 7) : (ps->sr >> 5 & 7);
}

/* the traps' priorities, by vector: a PIC24F's, and a PIC24E's and
 * dsPIC33E's (the DMAC error trap at 10) */
static const int trap_prio[8] = { 14, 13, 13, 12, 11, 8, 9, 8 };
static const int trap_prio_e[8] = { 15, 14, 13, 12, 11, 10, 9, 8 };

/* the exception's entry: PC and SR's low byte with IPL3 pushed, the
 * priority raised, and the vector's address */
static void enter(u32 vector, int ipl, int irq)
{
    struct sim *s = ps->sim;
    u32 ret = ps->pc;
    u16 hi = (u16)((ps->sr & 0xff) << 8 | ((ps->corcon & CORCON_IPL3) ? 0x80 : 0) |
                   (ret >> 16 & 0x7f));
    u16 sfa = ps->e && (ps->corcon & CORCON_SFA) ? 1 : 0;
    ps->trap = 0;
    if (push((u16)(ret | sfa)) || push(hi)) {
        ps->trap = 0;                           /* a stack that cannot take it */
    }
    if (ps->e)
        ps->corcon &= (u16)~CORCON_SFA;
    if (ipl < 8 && ps->intc && ps->intc->nstdis && ps->intc->nstdis(ps->intc->ctx))
        ipl = 7;
    ps->sr = (u16)((ps->sr & ~(SR_IPL | SR_RA)) | (u32)(ipl & 7) << 5);
    if (ipl >= 8)
        ps->corcon |= CORCON_IPL3;
    ps->pc = pic24_prog(s, vector) & 0x7ffffe;
    ps->sleeping = 0;
    s->cycles += 5;
    if (s->counting)
        sim_advance(s, 5);
    if (s->an)
        an_exc_entry(s, (u32)(irq < 0 ? (u32)(vector - 4) / 2 : (u32)irq + 8), ret);
}

static int take_exception(void)
{
    if (ps->traps) {
        int t = 0, best = -1;
        const int *prio = ps->e ? trap_prio_e : trap_prio;
        for (int k = 0; k < 8; k++)
            if ((ps->traps >> k & 1) && prio[k] > best) {
                best = prio[k];
                t = k;
            }
        ps->traps &= (u8)~(1u << t);
        if (best > cpu_ipl()) {
            if (ps->intc && ps->intc->trap)
                ps->intc->trap(ps->intc->ctx, t);
            enter(0x000004 + 2 * (u32)t, best, -1);
            return 1;
        }
        /* a trap at or below the priority running: the part resets */
        sim_end(ps->sim, END_LOCKUP, "lockup: %s trap at priority %d while the "
                "CPU runs at %d (the part resets)", trap_name[t], best, cpu_ipl());
        return 1;
    }
    if (!ps->intc)
        return 0;
    int irq, pr = ps->intc->request(ps->intc->ctx, &irq);
    if (!pr || pr <= cpu_ipl())
        return 0;
    if (pr < 7 && disicnt())
        return 0;
    ps->intc->taken(ps->intc->ctx, irq, pr);
    enter(0x000014 + 2 * (u32)irq, pr, irq);
    return 1;
}

/* ---- the run ------------------------------------------------------------------------ */

static void step(struct cpu *c)
{
    ps = (struct pic24_state *)c;
    struct sim *s = ps->sim;
    if (take_exception())
        return;
    u32 i = pic24_prog(s, ps->pc);
    if (ps->pc >= ps->prog_end) {
        char m[80];
        snprintf(m, sizeof m, "an instruction fetched at 0x%06x, past program "
                 "memory", (unsigned)ps->pc);
        trap(TRAP_ADDR, m, ~0u);
        ps->traps |= 1u << TRAP_ADDR;
        ps->trap = 0;
        return;
    }
    ps->insn = i;
    ps->size = two_word(i) ? 2 : 1;
    if (s->trace) {
        u32 units[2] = { i, pic24_prog(s, ps->pc + 2) };
        trace_insn(s, ps->pc, units, ps->size, 3);
    }
    int repeating = (ps->sr & SR_RA) != 0;
    ps->npc = (ps->pc + 2 * (u32)ps->size) & 0x7ffffe;
    ps->changed = 0;
    ps->extra = 0;
    ps->trap = 0;
    u64 c0 = s->cycles;
    s->insns++;
    if (!exec(i))
        return;
    s->cycles += 1 + (u32)ps->extra;
    if (s->counting)
        sim_advance(s, (u32)(s->cycles - c0));
    if (ps->trap) {
        ps->traps |= (u8)(1u << (ps->trap - 1));
        ps->trap = 0;
        if (repeating)
            ps->sr &= (u16)~SR_RA;
        ps->pc = ps->npc;
        return;
    }
    if (repeating && (ps->sr & SR_RA)) {
        if (ps->rcount) {
            ps->rcount--;
            return;                             /* again: the PC stays */
        }
        ps->sr &= (u16)~SR_RA;
    }
    if ((ps->sr & SR_DA) && ps->pc == ps->doend && ps->dsp)
        do_end();
    if (ps->npc == ps->pc && !ps->changed && s->state == RUN) {
        /* a branch to itself: the end, unless an interrupt can come */
        int irq, pr = ps->intc ? ps->intc->request(ps->intc->ctx, &irq) : 0;
        if ((!pr || pr <= cpu_ipl()) && !sim_event_coming(s)) {
            sim_end(s, END_IDLE, "a loop at 0x%06x that nothing can interrupt",
                    (unsigned)ps->pc);
            return;
        }
    }
    ps->pc = ps->npc;
}

/* ---- the CPU interface ----------------------------------------------------------------- */

/* Power-on: W0-W14 0 and not yet written, W15 0x0800 (the RAM's start),
 * SR 0, CORCON's PSV set, the pages 0, and the program counter at 0 (the
 * GOTO there). */
static void cpu_reset(struct cpu *c)
{
    struct pic24_state *k = (struct pic24_state *)c;
    memset(k->w, 0, sizeof k->w);
    k->w[15] = k->ram_base;
    k->winit = 1u << 15;
    k->splim = 0;
    k->splim_set = 0;
    k->sr = 0;
    k->corcon = k->dsp ? CORCON_SATDW : k->e ? 0 : 0x0004;
    k->dsrpag = k->dswpag = k->rcount = k->tblpag = k->disicnt = 0;
    if (k->e)
        k->dsrpag = k->dswpag = 1;              /* the RAM above 0x8000 */
    k->acc[0] = k->acc[1] = 0;
    k->dcount = 0;
    k->dostart = k->doend = 0;
    k->edt = 0;
    k->modcon = k->xmodsrt = k->ymodsrt = k->xbrev = k->mstrpr = 0;
    k->xmodend = k->ymodend = 1;
    k->y_lo = k->y_hi = 0;
    if (k->dsp && k->sim->svd)
        rf_y_space(k->sim->svd, &k->y_lo, &k->y_hi);
    k->disi_at = 0;
    k->pc = 0;
    k->traps = 0;
    k->trap = 0;
    k->key_step = 0;
    k->sleeping = 0;
    memset(k->sw, 0, sizeof k->sw);
    k->ssr = 0;
    ps = k;
    pic24_osc_power_on(k->sim);
}

static u32 cpu_pc(struct cpu *c)
{
    return ((struct pic24_state *)c)->pc;
}

static void cpu_interrupt(struct cpu *c, int n)
{
    (void)c;
    (void)n;
}

/* The registers for a debugger: W0-W15 (0-15), SPLIM 16, the PC 17
 * (four bytes), SR 18, CORCON 19, DSRPAG 20, DSWPAG 21, RCOUNT 22,
 * TBLPAG 23 -- no gdb has a PIC24 target; this is EmbSim's own order,
 * which the target description (cpu_gdb_xml) names for a client. */
static int cpu_reg_read(struct cpu *c, int n, u8 *buf)
{
    struct pic24_state *k = (struct pic24_state *)c;
    u32 v;
    int len = 2;
    if (n < 16)
        v = k->w[n];
    else
        switch (n) {
        case 16: v = k->splim; break;
        case 17: v = k->pc; len = 4; break;
        case 18: v = k->sr; break;
        case 19: v = k->corcon; break;
        case 20: v = k->dsrpag; break;
        case 21: v = k->dswpag; break;
        case 22: v = k->rcount; break;
        case 23: v = k->tblpag; break;
        default: return 0;
        }
    for (int b = 0; b < len; b++)
        buf[b] = (u8)(v >> (8 * b));
    return len;
}

static int cpu_reg_write(struct cpu *c, int n, const u8 *buf)
{
    struct pic24_state *k = (struct pic24_state *)c;
    u32 v = (u32)buf[0] | (u32)buf[1] << 8;
    if (n < 16) {
        k->w[n] = n == 15 ? (u16)(v & ~1u) : (u16)v;
        k->winit |= (u16)(1u << n);
        return 2;
    }
    switch (n) {
    case 16: k->splim = (u16)(v & ~1u); k->splim_set = 1; return 2;
    case 17:
        v |= (u32)buf[2] << 16 | (u32)buf[3] << 24;
        k->pc = v & 0x7ffffe;
        return 4;
    case 18: k->sr = (u16)(v & 0x1ff); return 2;
    case 19: k->corcon = (u16)(v & 0xc); return 2;
    case 20: k->dsrpag = (u16)(v & 0x3ff); return 2;
    case 21: k->dswpag = (u16)(v & 0x1ff); return 2;
    case 22: k->rcount = (u16)v; return 2;
    case 23: k->tblpag = (u16)(v & 0xff); return 2;
    }
    return 0;
}

static const int g_regs[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, -1
};

static const int *cpu_gdb_g_regs(struct cpu *c)
{
    (void)c;
    return g_regs;
}

/* the registers by name and size, in the g packet's order: a feature of
 * EmbSim's own, as gdb has none for the PIC24 (and so no architecture) */
static const char target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "<feature name=\"org.embcc.embsim.pic24.core\">\n"
    "<reg name=\"w0\" bitsize=\"16\" type=\"uint16\" regnum=\"0\"/>\n"
    "<reg name=\"w1\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w2\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w3\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w4\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w5\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w6\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w7\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w8\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w9\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w10\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w11\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w12\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w13\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"w14\" bitsize=\"16\" type=\"data_ptr\"/>\n"
    "<reg name=\"w15\" bitsize=\"16\" type=\"data_ptr\"/>\n"
    "<reg name=\"splim\" bitsize=\"16\" type=\"data_ptr\"/>\n"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>\n"
    "<reg name=\"sr\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"corcon\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"dsrpag\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"dswpag\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"rcount\" bitsize=\"16\" type=\"uint16\"/>\n"
    "<reg name=\"tblpag\" bitsize=\"16\" type=\"uint16\"/>\n"
    "</feature>\n"
    "</target>\n";

static const char *cpu_gdb_xml(struct cpu *c, const char *annex)
{
    (void)c;
    return !strcmp(annex, "target.xml") ? target_xml : 0;
}

/* an image's program address on the bus: twice it (a word's four bytes) */
static u32 cpu_phys(struct cpu *c, u32 a)
{
    (void)c;
    return 2 * a;
}

static const struct cpu_ops pic24_ops = {
    "pic24", 118, "a PIC24 or dsPIC image (XC16's)",
    cpu_reset, step, cpu_pc, cpu_reg_read, cpu_reg_write,
    cpu_gdb_g_regs, cpu_gdb_xml, 4, cpu_interrupt, 17, 1, cpu_phys,
};

int pic24_is(const struct cpu *c)
{
    return c && c->ops == &pic24_ops;
}

void pic24_intc_attach(struct sim *s, struct pic24_intc *ic)
{
    if (pic24_is(s->cpu))
        ((struct pic24_state *)s->cpu)->intc = ic;
}

void pic24_nvm_attach(struct sim *s, struct pic24_nvm *n)
{
    if (pic24_is(s->cpu))
        ((struct pic24_state *)s->cpu)->nvm = n;
}

void pic24_nvmkey(struct sim *s, u16 v)
{
    struct pic24_state *k = (struct pic24_state *)s->cpu;
    if (!pic24_is(s->cpu))
        return;
    if ((v & 0xff) == 0x55) {
        k->key_step = 1;
    } else if ((v & 0xff) == 0xaa && k->key_step == 1) {
        k->key_step = 2;
        k->key_at = s->insns;
    } else {
        k->key_step = 0;
    }
}

int pic24_unlocked(struct sim *s)
{
    struct pic24_state *k = (struct pic24_state *)s->cpu;
    if (!pic24_is(s->cpu) || k->key_step != 2 || s->insns - k->key_at > 2)
        return 0;
    k->key_step = 0;                /* one use */
    return 1;
}

/* A dsPIC33E's clock: the 7.37 MHz FRC (COSC 000), with the PLL (001),
 * the primary oscillator (the board's 8 MHz, 010) and with the PLL (011),
 * the SOSC (100), the LPRC (101), the FRC divided by 16 (110) or by
 * CLKDIV.FRCDIV (111); the PLL's output Fin * M / (N1 * N2): M PLLFBD + 2,
 * N1 CLKDIV.PLLPRE + 2, N2 2 (CLKDIV.PLLPOST + 1). The CPU runs at half. */
static u32 fcy_e(struct rf_periph *osc, u32 cosc, u32 clkdiv)
{
    u32 frc = 7370000u, fin = cosc <= 1 ? frc : 8000000u, fosc;
    struct rf_reg *r = osc ? rf_reg(osc, "PLLFBD") : 0;
    u64 m = (r ? (u32)r->value & 0x1ff : 0x30) + 2;
    switch (cosc) {
    case 1: case 3:
        fosc = (u32)((u64)fin * m / ((clkdiv & 0x1f) + 2) / (2 * ((clkdiv >> 6 & 3) + 1)));
        break;
    case 2: fosc = 8000000u; break;
    case 4: fosc = 32768; break;
    case 5: fosc = 32000; break;
    case 6: fosc = frc / 16; break;
    case 7: fosc = frc >> (clkdiv >> 8 & 7); break;
    default: fosc = frc; break;
    }
    return fosc / 2;
}

/* The core's clock. FNOSC (FOSCSEL<2:0>) chooses the oscillator at reset
 * and OSCCON.COSC the one running: the 8 MHz FRC, the FRC with the 96 MHz
 * PLL (CLKDIV.CPDIV divides it to 32, 16, 8 or 4 MHz), the FRC divided by
 * CLKDIV.RCDIV (OSCFDIV); the primary oscillator is the board's (8 MHz on
 * the Explorer 16/32), the LPRC 31 kHz, the SOSC 32.768 kHz. The CPU
 * runs at half of it. */
u32 pic24_fcy(struct sim *s)
{
    u32 cosc = 0, clkdiv = 0x3100;
    struct rf_periph *osc = s->svd ? rf_periph(s->svd, "OSC") : 0;
    struct rf_reg *r = osc ? rf_reg(osc, "OSCCON") : 0;
    if (r)
        cosc = (u32)(r->value >> 12 & 7);
    r = osc ? rf_reg(osc, "CLKDIV") : 0;
    if (r)
        clkdiv = (u32)r->value;
    if (pic24_is(s->cpu) && ((struct pic24_state *)s->cpu)->e)
        return fcy_e(osc, cosc, clkdiv);
    u32 fosc;
    switch (cosc) {
    case 1: fosc = 32000000u >> (clkdiv >> 6 & 3); break;
    case 2: fosc = 8000000u; break;
    case 3: fosc = 32000000u >> (clkdiv >> 6 & 3); break;
    case 4: fosc = 32768; break;
    case 5: fosc = 31000; break;
    case 7: {
        static const u32 div[8] = { 1, 2, 4, 8, 16, 32, 64, 256 };
        fosc = 8000000u / div[clkdiv >> 8 & 7];
        break;
    }
    default: fosc = 8000000u; break;
    }
    return fosc / 2;
}

/* the core: a PIC24FJ's (the PIC24FJ1024GB610 or a part of its family)
 * or a dsPIC33EP's, its program memory the board's first region (to
 * program address 0xABFFF on the PIC24FJ1024GB610, the configuration
 * words at the end), its RAM the third */
struct cpu *pic24_create(struct sim *s, const char *model,
                         const struct board_desc *bd)
{
    int ds = !strncmp(model, "dspic33ep", 9), e = ds || !strncmp(model, "pic24ep", 7);
    if (strncmp(model, "pic24fj", 7) && !e)
        return 0;
    struct pic24_state *k = calloc(1, sizeof *k);
    if (!k)
        die("out of memory");
    k->cpu.ops = &pic24_ops;
    k->sim = s;
    k->e = e;
    k->dsp = ds;
    k->ram_base = bd && bd->mem[2].base >= PIC24_DATA ?
                  (u16)(bd->mem[2].base - PIC24_DATA) : e ? 0x1000 : 0x0800;
    k->prog_end = bd && bd->mem[0].size ? bd->mem[0].size / 2 : 0xac000;
    s->cpu = &k->cpu;
    cpu_reset(&k->cpu);
    return &k->cpu;
}

/* ---- the core's own, for pic24-dsp.c -------------------------------------------- */

int pic24_core_read(u32 ea, int n, u16 *v)
{
    return dread(ea, n, v);
}

int pic24_core_write(u32 ea, int n, u16 v)
{
    return dwrite(ea, n, v);
}

void pic24_core_set_w(int n, u16 v)
{
    set_w(n, v);
}

int pic24_core_pointer(int n)
{
    return pointer(n);
}

int pic24_core_src(int mode, int r, int wb, int n, u16 *v)
{
    return src(mode, r, wb, n, v);
}

int pic24_core_dst(int mode, int r, int wb, int n, u16 v)
{
    return dst(mode, r, wb, n, v);
}

int pic24_core_bad_opcode(void)
{
    return bad_opcode();
}

int pic24_core_trap(int n, const char *why)
{
    return trap(n, why, ~0u);
}

void pic24_core_math(int why)
{
    static const char *const what[] = {
        "a divide by zero", "an overflow of ACCA", "an overflow of ACCB",
        "a catastrophic overflow of ACCA", "a catastrophic overflow of ACCB",
        "a shift of an accumulator past 16 bits",
    };
    int on = why == PIC24_MATH_DIV0 || why == PIC24_MATH_SFTAC;
    if (ps->intc && ps->intc->math)
        on = ps->intc->math(ps->intc->ctx, why);
    if (on)
        trap(TRAP_MATH, what[why], ~0u);
}
