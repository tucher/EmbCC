/* sim.h -- EmbSim's shared types and the interfaces between its modules.
 *
 * EmbSim is a machine put together from parts, each behind an interface
 * this file declares (docs/internals/embsim.md is the contributor's
 * guide):
 *
 *   - a CPU (struct cpu_ops): reset, step one instruction, the registers
 *     by GDB number, the target description, and interrupt entry. The
 *     Cortex-M is cortexm.c (with cortexm-thumb.c and cortexm-fpu.c).
 *   - the bus (bus.c): memory regions (RAM, flash that ignores stores),
 *     bit-band aliases, and memory-mapped devices by address range, each
 *     with read and write callbacks and a hook for the time that passes
 *     (struct dev_ops). The UARTs, SysTick, the system control space and
 *     DWT are devices.
 *   - boards as data (boards.c): a core, its options, the memory regions
 *     and the devices at their addresses. A new board is a table entry.
 *   - an SVD file's peripherals as a register file on the bus, and the
 *     behavioural models over it by the SVD's names (svd-map.h; the
 *     STM32's are stm32-*.c).
 *   - the run (run.c): the loop, time, the end-of-run rules and the
 *     console; semihost.c, trace.c (tracing and the counts), loader.c
 *     (the ELF image), gdb.c (the GDB remote server, over net.h's
 *     connection), and main.c (the command line).
 *
 * The C is ISO C99 with no dependencies beyond libm; net-posix.c is the
 * one file that uses an operating system's API, behind net.h. */
#ifndef EMBSIM_SIM_H
#define EMBSIM_SIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

struct sim;
struct cpu;

/* print "embsim: MESSAGE" and exit with status 2 */
/* reports and exits: GCC then knows that a NULL tested before it cannot
 * reach what follows (-Wformat-truncation at boards.c's snprintf) */
void die(const char *fmt, ...) __attribute__((noreturn));

/* ---- the bus (bus.c) -------------------------------------------------- */

/* MEM_ALIAS: another name for the region at a board's `target` (the
 * STM32's flash, seen at 0 as well as at 0x08000000) */
enum { MEM_RAM, MEM_FLASH, MEM_ALIAS };

struct region {
    u32 base, size;
    u8 *mem;
    int rom;                        /* stores are ignored */
    int kind;                       /* MEM_RAM or MEM_FLASH */
    u32 erased;                     /* each word of it before the image (bus_erase) */
};

/* A memory-mapped device. `off` is the offset of the access from the
 * device's base, `n` its size in bytes (1, 2 or 4); a read's value is
 * masked to n bytes by the bus. Any hook may be 0. */
struct dev_ops {
    const char *type;
    u32 (*read)(void *ctx, u32 off, int n);
    void (*write)(void *ctx, u32 off, int n, u32 v);
    void (*reset)(void *ctx);
    /* `cycles` of time have passed. Called after every instruction, but
     * only while the device has said it is counting (sim_clock): a
     * stopped timer costs nothing. */
    void (*tick)(void *ctx, u32 cycles);
    /* 1 and the cycles until the device next raises an interrupt, or 0
     * when it will not without the program doing something */
    int (*next_event)(void *ctx, u32 *cycles);
};

struct device {
    const struct dev_ops *ops;
    void *ctx;
    u32 base, size;
};

/* a bit-band alias: one word at base + 32 * byte + 4 * bit for each bit
 * of the `size / 32` bytes at target */
struct alias {
    u32 base, size, target;
};

/* a GDB watchpoint: Z2, Z3 or Z4 */
enum { WATCH_WRITE = 2, WATCH_READ = 3, WATCH_ACCESS = 4 };
struct watch {
    u32 addr, len;
    int kind;
};

#define BUS_REGIONS 12
#define BUS_DEVICES 24
#define BUS_ALIASES 4
#define BUS_WATCHES 32

struct bus {
    struct region rg[BUS_REGIONS];
    int nrg;
    struct device dev[BUS_DEVICES];
    int ndev;
    struct alias bb[BUS_ALIASES];
    int nbb;
    u32 fail_addr;                  /* the address of a failed access */
    /* every store the core makes, as the exclusive monitor sees it */
    void (*snoop)(void *ctx, u32 a);
    void *snoop_ctx;
    struct watch watch[BUS_WATCHES];
    int nwatch;
    int watch_hit, watch_kind;      /* the first watchpoint the core hit */
    u32 watch_addr;
    int debug;                      /* the access is a debugger's */
    int dev_fault;                  /* the device refused it: bus_fault */
    /* a device was read or written since sim_event_coming last looked:
     * when its next interrupt comes may have changed */
    int touched;
};

void bus_add_region(struct bus *b, u32 base, u32 size, int kind);
/* the region as before an image: every 4 bytes its `erased` word */
void bus_erase(struct region *r);
/* the region holding all `n` bytes at `a`, or 0: memory's fast path, so
 * inline (the core fetches through it, and reads and writes memory
 * through it when no watchpoint is set) */
static inline struct region *bus_region(struct bus *b, u32 a, u32 n)
{
    for (int i = 0; i < b->nrg; i++) {
        struct region *r = &b->rg[i];
        if (a - r->base < r->size && r->size - (a - r->base) >= n)
            return r;
    }
    return 0;
}
static inline u32 mem_rd_le(const u8 *p, int n)
{
    u32 v = p[0];
    if (n > 1)
        v |= (u32)p[1] << 8;
    if (n > 2)
        v |= (u32)p[2] << 16 | (u32)p[3] << 24;
    return v;
}
static inline void mem_wr_le(u8 *p, int n, u32 v)
{
    p[0] = (u8)v;
    if (n > 1)
        p[1] = (u8)(v >> 8);
    if (n > 2) {
        p[2] = (u8)(v >> 16);
        p[3] = (u8)(v >> 24);
    }
}
void bus_add_device(struct bus *b, u32 base, u32 size,
                    const struct dev_ops *ops, void *ctx);
void bus_add_alias(struct bus *b, u32 base, u32 size, u32 target);
/* a second name for the memory region holding `target`, at `base` */
void bus_add_mirror(struct bus *b, u32 base, u32 target);
/* a device's read or write hook refuses the access in progress: the
 * core takes its bus fault, as when nothing answers */
static inline void bus_fault(struct bus *b)
{
    b->dev_fault = 1;
}
/* The core's accesses: 0, or -1 with bus->fail_addr when nothing answers
 * at the address. A store tells the snoop. */
int bus_read(struct bus *b, u32 a, int n, u32 *v);
int bus_write(struct bus *b, u32 a, int n, u32 v);
/* Whether an access of the core's touches a watchpoint (1), recording
 * the first that one did in watch_hit. The core asks before the access,
 * so it can stop before it as QEMU's stub does. */
int bus_watch_check(struct bus *b, u32 a, int n, int write);
/* A debugger's: no watchpoints, no exclusive monitor, and a write to
 * flash goes in (as a flash programmer's does). */
int bus_debug_read(struct bus *b, u32 a, int n, u32 *v);
int bus_debug_write(struct bus *b, u32 a, int n, u32 v);
int bus_watch_add(struct bus *b, int kind, u32 addr, u32 len);
int bus_watch_remove(struct bus *b, int kind, u32 addr, u32 len);

/* ---- a CPU ------------------------------------------------------------ */

struct cpu_ops {
    const char *name;               /* "cortex-m" */
    int elf_machine;                /* the e_machine of its images */
    const char *elf_name;           /* "an ARM image" */
    /* power-on reset: the core's state and its own devices, then the
     * reset sequence (the vector table, for a Cortex-M) */
    void (*reset)(struct cpu *c);
    /* one instruction, or the entry to an exception */
    void (*step)(struct cpu *c);
    u32 (*pc)(struct cpu *c);
    /* the registers by GDB number: the bytes written, 0 when there is no
     * such register */
    int (*reg_read)(struct cpu *c, int n, u8 *buf);
    int (*reg_write)(struct cpu *c, int n, const u8 *buf);
    /* the registers of a `g` packet, in order, ending with -1 */
    const int *(*gdb_g_regs)(struct cpu *c);
    /* the target description (qXfer:features:read) for `annex`, or 0 */
    const char *(*gdb_xml)(struct cpu *c, const char *annex);
    /* the size of the breakpoint instruction at an address (Z0's kind) */
    int bp_kind;
    /* make exception or interrupt `n` pending (an ARM exception number:
     * 15 is SysTick, 16 + k is external interrupt k) */
    void (*interrupt)(struct cpu *c, int n);
    /* the program counter's GDB number (`c ADDR` and `s ADDR` set it) */
    int pc_regnum;
    /* the ELF classes its images may be: 1 for ELFCLASS32, 2 for
     * ELFCLASS64, or both (RISC-V) */
    int elf_classes;
    /* the bus address of a program address, for a core whose addresses
     * are not the bus's (MIPS: kseg0 and kseg1 are physical memory's
     * windows), or 0: the loader's segments and a debugger's accesses go
     * through it */
    u32 (*phys)(struct cpu *c, u32 a);
};

struct cpu {
    const struct cpu_ops *ops;
};

/* ---- boards (boards.c) ------------------------------------------------ */

struct mem_desc {
    u32 base, size;
    int kind;                       /* MEM_RAM, MEM_FLASH or MEM_ALIAS */
    int main_ram;                   /* the one --ram-size resizes */
    u32 target;                     /* MEM_ALIAS: the region it names */
    /* what each word of erased flash reads, little-endian: a PIC32's
     * 0xffffffff, a PIC24's 0x00ffffff (three bytes and the phantom); 0
     * where the board's reference (QEMU) has zeros */
    u32 erased;
};

struct dev_desc {
    const char *type;               /* a name in boards.c's device list */
    u32 base, size;
};

#define BOARD_MEMS 8
#define BOARD_DEVS 8

struct board_desc {
    const char *name;
    const char *core;               /* a name in boards.c's core list */
    const char *cpu;                /* the core's model */
    int prio_bits;                  /* NVIC priority bits implemented */
    struct mem_desc mem[BOARD_MEMS];   /* ends with a size of 0 */
    struct dev_desc dev[BOARD_DEVS];   /* ends with a type of 0 */
    struct alias bitband[2];        /* ends with a size of 0 */
    /* the part's CMSIS-SVD file, by its name: its peripherals' registers
     * are on the bus (svd-map.c), found with --svd or EMBSIM_SVD_PATH */
    const char *svd;
    /* the behavioural models layered over those registers, by the SVD's
     * peripheral names; ends with a name of 0 */
    const struct model_desc *models;
    /* how the part's bus treats those registers, beyond the SVD: RF_*
     * (svd-map.h), 0 for the plain rules */
    int rules;
};

/* a model for the SVD's peripherals whose names match `periph` (a `*`
 * matches any characters) */
struct model_desc {
    const char *periph;
    const char *model;              /* a name in boards.c's model list */
};

struct core_type {
    const char *name;
    /* the model's core, or 0 when the model is not one of this core's */
    struct cpu *(*create)(struct sim *s, const char *model,
                          const struct board_desc *bd);
};

struct dev_type {
    const char *type;
    const struct dev_ops *ops;
    void *(*create)(struct sim *s, const struct dev_desc *d);
};

extern const struct board_desc boards[];
extern const int nboards;
extern const struct core_type cores[];
extern const int ncores;
const struct board_desc *board_find(const char *name);
/* a part of a family EmbSim models, by its name, from its device pack
 * (pic32mz1024efg064, pic24fj256ga610), or 0 */
const struct board_desc *board_part(const char *name);
void sim_add_device(struct sim *s, const struct dev_desc *d);
/* a device on the bus, keeping time if it has a clock (a tick or a
 * next_event hook) */
void sim_add_dev(struct sim *s, u32 base, u32 size, const struct dev_ops *ops,
                 void *ctx);

/* ---- the run (run.c) -------------------------------------------------- */

enum { RUN, END_EXIT, END_LOCKUP, END_IDLE, END_UNTIL, END_BUDGET };

#define SIM_TICKERS 8

/* the console UART's receiver (sim_rx_port): whether it can take a byte
 * now, and a byte arriving. Connected only for a run with an input
 * (--input, or --replay of one): s->rx_on. */
struct rx_port {
    int (*room)(void *ctx);
    void (*put)(void *ctx, int c);
    void *ctx;
};

/* What a board wires to its part's pins, beyond the part: LEDs and
 * buttons on the GPIO, voltages on the analog inputs, a display on a
 * parallel bus. The part's peripheral models call these (each may be
 * 0); the board's device fills them in (pic-boards.c). */
struct board_io {
    void *ctx;
    /* a pin's driven level changed (port 0 is A): 1 high, 0 low, -1 let
     * go (an input, an open drain at 1) */
    void (*pin_out)(void *ctx, int port, int bit, int level);
    /* 1 and the level when something outside drives the pin, else 0 */
    int (*pin_in)(void *ctx, int port, int bit, int *level);
    /* the voltage on analog input n, or a negative one for none */
    double (*analog)(void *ctx, int n);
    /* a parallel master port's write and read cycles */
    void (*bus_write)(void *ctx, u32 addr, u32 data);
    u32 (*bus_read)(void *ctx, u32 addr);
    /* a byte SPI unit n (1 up) exchanges as master: what the slave on its
     * lines sends back, or -1 when none answers */
    int (*spi)(void *ctx, int unit, int out);
    /* --set NAME=VALUE: 0, or -1 when the board has no such input */
    int (*set)(void *ctx, const char *name, const char *value);
    /* the run ended: what the board shows (its display) */
    void (*end)(void *ctx);
};

struct sim {
    struct bus bus;
    struct cpu *cpu;
    const struct board_desc *board;
    const char *image;
    u64 insns, cycles;
    int state, exit_status;
    char end_why[160];
    const char *until;              /* stop once the output holds this */
    size_t until_len, until_at;     /* how much of it has matched */
    u64 max_insns;
    int semihosting;
    int reset_runs_on;              /* --reset-runs-on: a part's RESET resets it */
    FILE *trace;                    /* --trace, or 0 */
    /* the devices that keep time: their tick hooks, flat, since they
     * are called after every instruction; and the devices themselves */
    void (*tick_fn[SIM_TICKERS])(void *ctx, u32 cycles);
    void *tick_ctx[SIM_TICKERS];
    int ntick_fn;
    struct device *tick[SIM_TICKERS];
    int ntick;
    int counting;                   /* devices whose clock is running */
    /* the image, as the loader found it: its entry point, whether it is
     * ELFCLASS64, and its e_flags (a core with more than one width or
     * ABI reads them at reset) */
    u64 entry;
    int elf64;
    u32 elf_flags;
    /* instructions a skip passed over (AVR's CPSE, SBRC, SBRS, SBIC and
     * SBIS): not run, so not in `insns`; with count_skips, --count's
     * file has them as a third line */
    u64 skipped;
    int count_skips;
    /* the SVD's register file (svd-map.c), or 0 */
    struct svdmap *svd;
    /* sim_event_coming's answer, and the cycle its event comes at */
    int ev_known, ev_any;
    u64 ev_at;
    /* the analyses watching the run (analysis.h), or 0: when set, the
     * run steps through an_step */
    struct analysis *an;
    /* the UART's receiver, and whether an input is connected to it */
    struct rx_port rx;
    int rx_on;
    /* the record of the run's inputs, or its replay (record.c), or 0 */
    struct rec *rec;
    /* what the board has outside the part (pic-boards.c's), or zeros */
    struct board_io io;
    /* the time since reset in ns, from a part that models its clocks (the
     * RP2040's: its core runs at the clk_sys the program sets), or 0 */
    double (*now_ns)(struct sim *s);
    /* the I2C bus endpoint (i2c-bus.c), or 0 */
    struct i2cbus *i2c;
};

/* build the board with its core (`model`, or 0 for the board's own),
 * and with the peripherals of an SVD file (`svd`, or 0 for the board's
 * own, if it has one) */
void sim_init(struct sim *s, const struct board_desc *bd, const char *model,
              const char *svd);
/* the memory (`ram_size`, or 0 for the board's), the image in it, and
 * the core out of reset */
void sim_load(struct sim *s, u32 ram_size, const char *image);
/* a reset, as the reset pin gives one: the core and the devices, and
 * the counts back to zero; memory as it is, or (reload) the image loaded
 * into it again */
void sim_reset(struct sim *s, int reload);
/* a part's own reset (a RESET instruction): its devices and core go to their
 * reset state while time goes on, and the memories keep what they hold */
void sim_part_reset(struct sim *s);
/* one instruction, and the budget (--max-insns) */
void sim_step(struct sim *s);
/* run until the end */
void sim_run(struct sim *s);
/* end the run: state, and the reason --stats prints */
void sim_end(struct sim *s, int state, const char *fmt, ...);
/* a byte of the program's output */
void sim_out(struct sim *s, int c);
/* `cycles` of time pass for the devices */
void sim_advance(struct sim *s, u32 cycles);
/* a device's clock starts (on 1) or stops (0): whether its tick hook
 * needs calling; `*running` is the device's own record of it */
void sim_clock(struct sim *s, int *running, int on);
/* 1 and the cycles to the next device interrupt, or 0 when none is due */
int sim_next_event(struct sim *s, u32 *cycles);
/* whether a device will raise an interrupt, as sim_next_event says, but
 * asked again only when it may have changed: a device accessed (by the
 * core or a debugger: bus->touched), a clock started or stopped, a
 * reset, a RISC-V CSR written, or the event's time come. For the cores'
 * test of a loop that branches to itself, which asks it every turn. */
int sim_event_coming(struct sim *s);

/* ---- the analyses (analysis.c) ---------------------------------------- */

/* one step with the analyses watching, and the budget: sim_step's and
 * sim_run's when s->an is set */
void an_step(struct sim *s);
/* a core, while s->an is set: exception `n` (an ARM exception number, a
 * RISC-V mcause, an AVR vector) was entered and returns to `ret`; the
 * handler returned */
void an_exc_entry(struct sim *s, u32 n, u32 ret);
void an_exc_return(struct sim *s);

/* ---- the run's inputs (record.c) ------------------------------------ */

/* the board's console UART offers its receiver (the first to call it) */
void sim_rx_port(struct sim *s, int (*room)(void *ctx),
                 void (*put)(void *ctx, int c), void *ctx);
/* semihosting's SYS_READC: a byte of the host's stdin, or EOF;
 * recorded, or replayed */
int sim_readc(struct sim *s);
/* what a debugger did, for --record (each a no-op without it) */
void rec_reg(struct sim *s, int n, const u8 *b, int len);
void rec_mem(struct sim *s, u32 a, const u8 *b, int len);
void rec_wake(struct sim *s);
void rec_reset(struct sim *s, int reload);
void rec_kill(struct sim *s);
/* a byte of the program's output, which the record's end hashes */
void rec_out(struct sim *s, int c);
/* the run takes its inputs from a recording */
int rec_replaying(struct sim *s);
/* an exchange with the I2C bus endpoint: recorded; or, replaying, the
 * recorded answer to `ask` (which must be the recorded ask) */
void rec_i2c(struct sim *s, const char *ask, const char *answer);
void rec_i2c_replay(struct sim *s, const char *ask, char *answer, size_t n);

/* ---- the I2C bus endpoint (i2c-bus.c, i2c-bus.def) ---------------------- */

/* --i2c-bus [HOST:]PORT: listen there and wait for the endpoint, or, when
 * replaying, take its answers from the recording */
void i2cbus_open(struct sim *s, const char *host, int port);
/* an endpoint is on: without one, nothing outside the part is on its buses */
int i2cbus_on(struct sim *s);
/* the unit that drives bus `bus` (1 up), and how it takes a write another
 * master starts on it at cycle `at`; it says how many of the write's bytes
 * its slave acknowledged with i2cbus_delivered */
void i2cbus_attach(struct sim *s, int bus, void *ctx,
                   void (*inject)(void *ctx, u64 at, const u8 *b, int n));
void i2cbus_delivered(struct sim *s, int bus, int count);
/* the write lost arbitration to the part's master after `count` bytes */
void i2cbus_beaten(struct sim *s, int bus, int count);
/* the unit's master: its conditions, a byte it sends (1: a slave
 * acknowledged it), a byte it reads, and its acknowledge of that byte */
void i2cbus_start(struct sim *s, int bus);
void i2cbus_restart(struct sim *s, int bus);
void i2cbus_stop(struct sim *s, int bus);
int i2cbus_write(struct sim *s, int bus, u8 byte);
/* the part's master lost arbitration: the byte it sent never went out */
void i2cbus_lost(struct sim *s, int bus);
u8 i2cbus_read(struct sim *s, int bus);
void i2cbus_master_ack(struct sim *s, int bus, int ack);

/* ---- semihosting (semihost.c) ----------------------------------------- */

/* the core's view of memory, for a semihosting call: an access that
 * faults sets the core's fault, and `faulted` reports it */
struct semi_ops {
    u32 (*ld)(void *ctx, u32 a, int n);
    void (*st)(void *ctx, u32 a, int n, u32 v);
    int (*faulted)(void *ctx);
};

/* An ARM semihosting call: operation `op` (r0), parameter `arg` (r1).
 * Returns 1 with the result for r0 in *ret, or 0 when r0 is unchanged. */
int semihost(struct sim *s, const struct semi_ops *m, void *ctx, u32 op,
             u32 arg, u32 *ret);

/* ---- tracing and counting (trace.c) ----------------------------------- */

void trace_open(struct sim *s, const char *path);
/* one instruction: its address and its units (halfwords for Thumb),
 * each `unit` bytes wide */
void trace_insn(struct sim *s, u32 pc, const u32 *units, int n, int unit);
/* the end of a run: --count's file, the --stats line, the trace closed */
void trace_report(struct sim *s, const char *count_path, int stats);

/* ---- the image (loader.c) --------------------------------------------- */

void load_elf(struct sim *s, const char *path);

/* ---- the GDB server (gdb.c) ------------------------------------------- */

/* Serve the GDB remote protocol on `port` of `host` (0: loopback) while
 * the run goes on; with `wait`, halted at reset until a debugger
 * connects. Returns when the run has ended (or the debugger killed it),
 * with s->state and s->exit_status saying how. */
void gdb_serve(struct sim *s, const char *host, int port, int wait);

#endif
