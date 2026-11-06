/* embsim -- an instruction-set simulator: run a firmware image without a
 * board.
 *
 * It runs an ELF image the way the part does -- a Cortex-M, a RISC-V
 * core, an AVR or a MIPS32 -- on a model of one of the boards QEMU
 * models, so an image built for QEMU runs unchanged, and QEMU referees
 * the simulator: tests/golden/embsim.sh, embsim-riscv.sh, embsim-avr.sh
 * and embsim-mips.sh run the exec corpus on both, and the output (and
 * but on MIPS the instruction count) must be the same. It counts the
 * instructions it executes and estimates their cycles with the table
 * tools/bench uses (src/arch/cost.h) -- on the AVR, exactly
 * -- and the timers (SysTick, DWT's CYCCNT, the CLINT's mtime, the AVR's
 * Timer/Counter1) advance by it, so a timed run gives the same answer
 * every time.
 *
 * This file is the command line. sim.h says how the rest is put
 * together; docs/manual/tools/embsim.md is the user's reference and
 * docs/internals/embsim.md the contributor's. The C is ISO C99 with no
 * dependencies, so it builds on a machine with no QEMU (the portable-host
 * goal). */
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "disasm.h"
#include "image.h"
#include "sim.h"
#include "pic24.h"
#include "svd-map.h"

static u32 parse_size(const char *s)
{
    char *e;
    unsigned long v = strtoul(s, &e, 0);
    if (*e == 'K' || *e == 'k')
        v <<= 10, e++;
    else if (*e == 'M' || *e == 'm')
        v <<= 20, e++;
    if (*e || e == s)
        die("bad size '%s'", s);
    return (u32)v;
}

/* PORT, HOST:PORT, or QEMU's tcp::PORT: the port, and the host (empty for
 * this machine's loopback addresses) */
static int host_port(const char *opt, const char *arg, char *host, size_t size)
{
    const char *c = strrchr(arg, ':');
    char *e;
    long v = strtol(c ? c + 1 : arg, &e, 10);
    if (*e || v <= 0 || v > 65535)
        die("bad port in %s '%s'", opt, arg);
    host[0] = 0;
    if (c) {
        const char *h = !strncmp(arg, "tcp:", 4) ? arg + 4 : arg;
        size_t n = (size_t)(c - h);
        if (n >= size)
            die("bad host in %s '%s'", opt, arg);
        memcpy(host, h, n);
        host[n] = 0;
    }
    return (int)v;
}

static void usage(void)
{
    fputs("usage: embsim IMAGE.elf|IMAGE.hex [--board NAME] [--cpu NAME]\n"
          "              [--ram-size SIZE] [--until STRING] [--max-insns N]\n"
          "              [--stats] [--count FILE] [--trace FILE]\n"
          "              [--no-semihosting] [--gdb [HOST:]PORT [--gdb-wait]]\n"
          "              [--svd FILE.svd] [--trace-periph[=NAME,...]]\n"
          "              [--coverage FILE [--coverage-format=text|lcov]]\n"
          "              [--profile[=FILE] [--profile-format=report|collapsed|\n"
          "               collapsed-insns]]\n"
          "              [--stack-report[=FILE]] [--stack-limit ADDR|SYMBOL]\n"
          "              [--stack-su FILE.su]... [--stack-embrt FILE.json] [FILE.su...]\n"
          "              [--fault-report[=FILE]] [--disassemble[=FILE]]\n"
          "              [--input FILE|-] [--record FILE] [--i2c-bus [HOST:]PORT]\n"
          "              [--dfp DIR] [--set NAME=VALUE]...\n"
          "       embsim [IMAGE.elf] --replay FILE [--count FILE] [--stats] ...\n"
          "       embsim --svd FILE.svd --svd-map\n"
          "boards: lm3s6965evb (default), mps2-an385, mps2-an386,\n"
          "        mps2-an500, microbit, stm32f405 (its SVD: --svd or\n"
          "        EMBSIM_SVD_PATH), bluepill (STM32F103C8: ST's\n"
          "        STM32F103xx.svd), nrf52840dk (Nordic's nrf52840.svd),\n"
          "        pico (the Raspberry Pi Pico's RP2040:\n"
          "        the Pico SDK's RP2040.svd), arduino-zero (SAM D21:\n"
          "        Microchip's ATSAMD21G18A.svd), esp32c3-devkitm-1\n"
          "        (Espressif's esp32c3.svd), virt (RISC-V), uno (AVR),\n"
          "        malta (MIPS),\n"
          "        pic32mz2048efh100, explorer16-32, curiosity-pic32mz-ef-2\n"
          "        (PIC32MZ: its ATDF, from --dfp DIR, --svd FILE.atdf or\n"
          "        EMBSIM_SVD_PATH), pic24fj1024gb610, explorer16-32-pic24\n"
          "        (PIC24: its EDC, from --dfp DIR or --svd FILE.PIC),\n"
          "        explorer16-32-pic32mx (PIC32MX: its EDC, from --dfp DIR),\n"
          "        explorer16-32-dspic33 (dsPIC33EP: its EDC, from --dfp DIR);\n"
          "        and any PIC32MZ EF, PIC32MX, PIC24FJ GA6/GB6, dsPIC33EP or\n"
          "        PIC24EP part by its name (pic32mz1024efg064, pic32mx795f512l,\n"
          "        pic24fj256ga610, dspic33ep512mu810), from --dfp DIR\n"
          "cpus:   cortex-m0, cortex-m0plus, cortex-m3, cortex-m4, cortex-m7;\n"
          "        rv32, rv64 (virt's default: the image's width), esp32c3;\n"
          "        atmega328p;\n"
          "        24kc, pic32mz, pic32mx; pic24fj1024gb610, dspic33ep512mu810\n"
          "        (or the part's name)\n"
          "--set:  explorer16-32: temperature=DEGREES, pot=VOLTS or PERCENT%,\n"
          "        S4, S5, S6=pressed or released; explorer16-32-pic24,\n"
          "        explorer16-32-pic32mx and explorer16-32-dspic33: the same,\n"
          "        and S3;\n"
          "        curiosity-pic32mz-ef-2: SW1, SW2, SW3, SW4=pressed or released;\n"
          "        pico: BOOTSEL=pressed or released;\n"
          "        nrf52840dk: BUTTON1-4=pressed or released;\n"
          "        esp32c3-devkitm-1: BOOT=pressed or released\n",
          stderr);
    exit(2);
}

static struct sim sim;

/* ---- a PIC24 or dsPIC33E part by its name: the pins --set drives -------------- */

/* A part by its name has no board around it, so nothing outside drives its
 * pins and an input reads its pull-up (CNPUx), else 0. --set RD5=low drives
 * one for the whole run as a board's input would: R, the port's letter (no
 * I), the bit, and low, high, 0 or 1. */
#define PART_PINS 16
static struct { int port, bit, level; } part_pin[PART_PINS];
static int part_npin;

static int part_pin_in(void *ctx, int port, int bit, int *level)
{
    (void)ctx;
    for (int i = 0; i < part_npin; i++)
        if (part_pin[i].port == port && part_pin[i].bit == bit) {
            *level = part_pin[i].level;
            return 1;
        }
    return 0;
}

static int part_pin_set(void *ctx, const char *name, const char *value)
{
    (void)ctx;
    char *end;
    if (name[0] != 'R' || name[1] < 'A' || name[1] > 'K' || name[1] == 'I' ||
        part_npin == PART_PINS)
        return -1;
    long bit = strtol(name + 2, &end, 10);
    int level = !strcmp(value, "high") || !strcmp(value, "1") ? 1 :
                !strcmp(value, "low") || !strcmp(value, "0") ? 0 : -1;
    if (end == name + 2 || *end || bit < 0 || bit > 31 || level < 0)
        return -1;
    part_pin[part_npin].port = name[1] - 'A' - (name[1] > 'I');
    part_pin[part_npin].bit = (int)bit;
    part_pin[part_npin++].level = level;
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = 0, *cpu = 0, *count_path = 0, *trace_path = 0;
    const char *until = 0, *gdb = 0, *svd = 0, *trace_periph = 0, *i2c_bus = 0;
    const char *coverage = 0, *profile = 0;
    int svd_map = 0, tracing_periph = 0, board_given = 0, cov_lcov = 0;
    int profiling = 0, prof_fmt = 0, stk_report = 0;
    const char *stk_path = 0, *stk_limit = 0, *stk_embrt = 0, *fault_path = 0;
    int fault_report = 0, semi_given = 0, disassemble = 0;
    const char *dis_path = 0;
    const char *input = 0, *record = 0, *replay = 0;
    const char **su = calloc((size_t)argc, sizeof *su);
    const char **sets = calloc((size_t)argc, sizeof *sets);
    int nsu = 0, nset = 0;
    u32 ram_size = 0;
    u64 max_insns = 0;
    int stats = 0, verbose = 0, semihosting = 1, gdb_wait = 0;
    const struct board_desc *bd = &boards[0];
    const char *board_name = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--board") && more) {
            board_name = argv[++i];
            board_given = 1;
        } else if (!strcmp(a, "--cpu") && more)
            cpu = argv[++i];
        else if (!strcmp(a, "--ram-size") && more)
            ram_size = parse_size(argv[++i]);
        else if (!strcmp(a, "--until") && more) {
            until = argv[++i];
            if (!*until)
                until = 0;
        } else if (!strcmp(a, "--max-insns") && more)
            max_insns = strtoull(argv[++i], 0, 0);
        else if (!strcmp(a, "--count") && more)
            count_path = argv[++i];
        else if (!strcmp(a, "--trace") && more)
            trace_path = argv[++i];
        else if (!strcmp(a, "--stats"))
            stats = 1;
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = 1;
        else if (!strcmp(a, "--no-semihosting"))
            semihosting = 0, semi_given = 1;
        else if (!strcmp(a, "--gdb") && more)
            gdb = argv[++i];
        else if (!strcmp(a, "--i2c-bus") && more)
            i2c_bus = argv[++i];
        else if (!strcmp(a, "--gdb-wait"))
            gdb_wait = 1;
        else if (!strcmp(a, "--set") && more)
            sets[nset++] = argv[++i];
        else if (!strncmp(a, "--set=", 6))
            sets[nset++] = a + 6;
        else if (!strcmp(a, "--dfp") && more)
            svd_dfp = argv[++i];
        else if (!strcmp(a, "--svd") && more)
            svd = argv[++i];
        else if (!strcmp(a, "--svd-map"))
            svd_map = 1;
        else if (!strcmp(a, "--trace-periph"))
            tracing_periph = 1, trace_periph = 0;
        else if (!strncmp(a, "--trace-periph=", 15))
            tracing_periph = 1, trace_periph = a + 15;
        else if (!strcmp(a, "--coverage") && more)
            coverage = argv[++i];
        else if (!strncmp(a, "--coverage-format=", 18)) {
            if (!strcmp(a + 18, "lcov"))
                cov_lcov = 1;
            else if (!strcmp(a + 18, "text"))
                cov_lcov = 0;
            else
                die("--coverage-format is text or lcov, not '%s'", a + 18);
        } else if (!strcmp(a, "--profile"))
            profiling = 1, profile = 0;
        else if (!strncmp(a, "--profile=", 10))
            profiling = 1, profile = a + 10;
        else if (!strncmp(a, "--profile-format=", 17)) {
            const char *f = a + 17;
            if (!strcmp(f, "report"))
                prof_fmt = 0;
            else if (!strcmp(f, "collapsed"))
                prof_fmt = 1;
            else if (!strcmp(f, "collapsed-insns"))
                prof_fmt = 2;
            else
                die("--profile-format is report, collapsed or collapsed-insns, "
                    "not '%s'", f);
        } else if (!strcmp(a, "--input") && more)
            input = argv[++i];
        else if (!strcmp(a, "--record") && more)
            record = argv[++i];
        else if (!strcmp(a, "--replay") && more)
            replay = argv[++i];
        else if (!strcmp(a, "--fault-report"))
            fault_report = 1, fault_path = 0;
        else if (!strncmp(a, "--fault-report=", 15))
            fault_report = 1, fault_path = a + 15;
        else if (!strcmp(a, "--disassemble"))
            disassemble = 1, dis_path = 0;
        else if (!strncmp(a, "--disassemble=", 14))
            disassemble = 1, dis_path = a + 14;
        else if (!strcmp(a, "--stack-report"))
            stk_report = 1, stk_path = 0;
        else if (!strncmp(a, "--stack-report=", 15))
            stk_report = 1, stk_path = a + 15;
        else if (!strcmp(a, "--stack-limit") && more)
            stk_limit = argv[++i];
        else if (!strcmp(a, "--stack-su") && more)
            su[nsu++] = argv[++i];
        else if (!strcmp(a, "--stack-embrt") && more)
            stk_embrt = argv[++i];
        else if (a[0] != '-' && strlen(a) > 3 && !strcmp(a + strlen(a) - 3, ".su"))
            su[nsu++] = a;              /* embsim fw.elf --stack-report *.su */
        else if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            usage();
        else if (a[0] == '-')
            die("unknown option '%s'", a);
        else if (!image)
            image = a;
        else
            die("one image at a time ('%s' and '%s')", image, a);
    }
    /* a board by its name, or a part by its own (from its pack: --dfp's) */
    if (board_name && !(bd = board_find(board_name)) && !(bd = board_part(board_name)))
        die("unknown board '%s'", board_name);
    /* --replay: the machine is the recording's; an option that shapes it
     * must say what the recording says, or be left out */
    struct rec *rp = 0;
    struct rec_machine rm;
    if (replay) {
        if (input || record || gdb || i2c_bus)
            die("--replay takes the run's inputs from the recording: not with "
                "--input, --record, --gdb or --i2c-bus");
        rp = rec_load(replay, &rm);
        if (!image)
            image = rm.image;
        if (board_given && strcmp(bd->name, rm.board))
            die("--board %s: the recording ran on %s", bd->name, rm.board);
        board_given = 1;
        if (cpu && (!rm.cpu || strcmp(cpu, rm.cpu)))
            die("--cpu %s: the recording ran %s", cpu, rm.cpu ? rm.cpu : "the board's");
        cpu = rm.cpu;
        if (ram_size && ram_size != rm.ram_size)
            die("--ram-size: the recording ran with %lu", (unsigned long)rm.ram_size);
        ram_size = rm.ram_size;
        if (svd && (!rm.svd || strcmp(svd, rm.svd)))
            die("--svd %s: the recording ran with %s", svd, rm.svd ? rm.svd : "none");
        svd = rm.svd;
        if (semi_given && semihosting != rm.semihosting)
            die("--no-semihosting: the recording ran with semihosting");
        semihosting = rm.semihosting;
        if (max_insns && max_insns != rm.max_insns)
            die("--max-insns: the recording ran with %llu", (unsigned long long)rm.max_insns);
        max_insns = rm.max_insns;
        if (until && (!rm.until || strcmp(until, rm.until)))
            die("--until: the recording ran with %s", rm.until ? rm.until : "none");
        until = rm.until;
        if (svd_dfp && (!rm.dfp || strcmp(svd_dfp, rm.dfp)))
            die("--dfp %s: the recording ran with %s", svd_dfp,
                rm.dfp ? rm.dfp : "none");
        svd_dfp = rm.dfp;
        if (!(bd = board_find(rm.board)) && !(bd = board_part(rm.board)))
            die("the recording's board '%s' is not one of EmbSim's", rm.board);
        if (nset)
            die("--set: the recording's inputs are the run's (%d of them)",
                rm.nset);
        if (rm.nset) {
            sets = rm.sets;
            nset = rm.nset;
        }
        rec_check_image(image, &rm);
    }
    if (svd && !board_given) {
        /* --svd STM32F405.svd is the stm32f405's */
        const char *b = strrchr(svd, '/');
        b = b ? b + 1 : svd;
        for (int i = 0; i < nboards; i++) {
            const char *n = boards[i].svd;
            size_t k = 0;
            while (n && n[k] && b[k] &&
                   (n[k] | 0x20) == (b[k] | 0x20))
                k++;
            if (n && !n[k] && !b[k])
                bd = &boards[i];
        }
    }
    if (svd_map) {
        if (!svd && !bd->svd)
            die("--svd-map needs --svd FILE, or a board with an SVD");
        sim_init(&sim, bd, cpu, svd);
        svdmap_print(sim.svd, stdout);
        return 0;
    }
    if (!image)
        usage();
    char host[256], i2c_host[256];
    int port = 0, i2c_port = 0;
    if (gdb)
        port = host_port("--gdb", gdb, host, sizeof host);
    else if (gdb_wait)
        die("--gdb-wait needs --gdb PORT");
    if (i2c_bus)
        i2c_port = host_port("--i2c-bus", i2c_bus, i2c_host, sizeof i2c_host);

    struct sim *s = &sim;
    sim_init(s, bd, cpu, svd);
    if (!s->io.set && !s->io.pin_in && !strcmp(bd->core, "pic24")) {
        s->io.set = part_pin_set;
        s->io.pin_in = part_pin_in;
    }
    for (int i = 0; i < nset; i++) {
        /* the board's inputs: --set temperature=30 */
        char name[64];
        const char *eq = strchr(sets[i], '=');
        size_t l = eq ? (size_t)(eq - sets[i]) : 0;
        if (!eq || !l || l >= sizeof name)
            die("--set %s: give NAME=VALUE", sets[i]);
        memcpy(name, sets[i], l);
        name[l] = 0;
        if (!s->io.set || s->io.set(s->io.ctx, name, eq + 1))
            die("--set %s: the %s board has no input named %s", sets[i],
                bd->name, name);
    }
    if (tracing_periph) {
        if (!s->svd)
            die("--trace-periph needs --svd FILE: the names are the SVD's");
        svdmap_trace(s->svd, trace_periph);
    }
    s->until = until;
    s->until_len = until ? strlen(until) : 0;
    s->max_insns = max_insns;
    s->semihosting = semihosting;
    if (trace_path)
        trace_open(s, trace_path);
    sim_load(s, ram_size, image);
    if ((nsu || stk_embrt) && !stk_report)
        die("--stack-su and --stack-embrt are for --stack-report");
    if (disassemble) {
        /* the image's code as text, and no run */
        FILE *o = dis_path && *dis_path && strcmp(dis_path, "-") ? fopen(dis_path, "w")
                                                                 : stdout;
        if (!o)
            die("cannot write %s", dis_path);
        dis_listing(s, image_open(image), o);
        if (o != stdout)
            fclose(o);
        return 0;
    }
    if ((coverage || profiling || stk_report || stk_limit || fault_report) &&
        image_is_hex(image))
        die("--coverage, --profile, --stack-report, --stack-limit and "
            "--fault-report read the image's symbols and lines: give its ELF "
            "file, not its HEX one");
    if (coverage || profiling || stk_report || stk_limit || fault_report ||
        input || record || rp || i2c_bus) {
        struct analysis *an = an_create(s, profiling || stk_report || stk_limit ||
                                               fault_report);
        if (coverage) {
            an->cov_path = coverage;
            an->cov_lcov = cov_lcov;
            cov_init(an);
        }
        an->prof = profiling;
        an->prof_path = profile && *profile && strcmp(profile, "-") ? profile : 0;
        an->prof_fmt = prof_fmt;
        if (fault_report) {
            an->fault = 1;
            if (fault_path && *fault_path && strcmp(fault_path, "-")) {
                an->fault_out = fopen(fault_path, "w");
                if (!an->fault_out)
                    die("cannot write %s", fault_path);
            }
        }
        if (stk_report || stk_limit) {
            an->stk = 1;
            an->stk_report = stk_report;
            an->stk_path = stk_path && *stk_path && strcmp(stk_path, "-") ? stk_path : 0;
            an->stk_limit_arg = stk_limit;
            an->su_path = su;
            an->nsu_path = nsu;
            an->embrt_path = stk_embrt;
            stk_init(an);
            stk_read_static(an);
        }
        if (rp)
            rec_attach(s, rp, &rm);
        else if (input || record || i2c_bus)
            rec_create(s);
        if (input)
            rec_input(s, input);
        if (record) {
            struct rec_machine w;
            memset(&w, 0, sizeof w);
            w.image = image;
            w.board = bd->name;
            w.cpu = cpu;
            w.ram_size = ram_size;
            w.svd = svd;
            w.semihosting = semihosting;
            w.max_insns = max_insns;
            w.until = until;
            w.dfp = svd_dfp;
            w.sets = sets;
            w.nset = nset;
            w.i2c_bus = i2c_bus != 0;
            rec_record(s, record, &w);
        }
        if (s->rec)
            rec_start(s);
    }

    if (i2c_bus)
        i2cbus_open(s, i2c_host[0] ? i2c_host : 0, i2c_port);
    else if (rp && rm.i2c_bus)
        i2cbus_open(s, 0, 0);
    if (gdb)
        gdb_serve(s, host[0] ? host : 0, port, gdb_wait);
    else
        sim_run(s);
    if (s->io.end)
        s->io.end(s->io.ctx);
    trace_report(s, count_path, stats || verbose);
    if (s->rec)
        rec_finish(s);
    if (s->an)
        an_finish(s);
    switch (s->state) {
    case END_EXIT: return s->exit_status;
    case END_LOCKUP: return 3;
    case END_BUDGET: return 4;
    default: return 0;
    }
}
