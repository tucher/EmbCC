/* record.c -- --input, --record and --replay: what the outside world
 * gives a run, and the run again from a record of it.
 *
 * Everything EmbSim computes is a function of the image and the options
 * but for what comes from outside the machine:
 *   - bytes arriving at the console UART's receiver (--input FILE, or -
 *     for stdin): a byte arrives when the host has one, which is when a
 *     person types it or a pipe delivers it -- a time the run cannot
 *     know in advance. The receiver is offered a byte every RX_POLL
 *     steps while it has room, and when the core would otherwise sleep
 *     for ever (the run waits for the host instead);
 *   - semihosting's SYS_READC, a byte of the host's stdin;
 *   - a debugger's writes to registers and memory, its `monitor reset`,
 *     the core it wakes from a sleep nothing else would end, and its
 *     `kill`;
 *   - the I2C bus endpoint's answers (--i2c-bus, i2c-bus.c): each kept
 *     with what it answered, which a replay checks it is asked again.
 * Time is not among them: every counter and timer runs on the core's
 * estimated cycles, never on the host's clock.
 *
 * --record FILE writes each of these with the step it came after (or
 * during), and the run's machine (the image and the options that shape
 * it: the board's inputs --set gives, --dfp's pack, too) and its end: the steps, the counts, how it ended, a hash of the
 * registers and memory, and the output's length and hash. A step is one call of the core's step that
 * did something (a debugger's watchpoint makes one that does nothing,
 * which a run without the debugger never takes). The record also holds
 * every exception and interrupt the core entered, with its step and its
 * instruction count, which replay checks rather than makes. A run of
 * the same exception at a constant stride -- a timer's tick waking an
 * idle loop, while the run waits for the host -- is one line: "exc N x
 * COUNT +STEPS +INSNS", so a record grows with what the program does,
 * not with how long a person takes to type.
 *
 * --replay FILE runs the recorded machine and gives it the recorded
 * inputs at the recorded steps, with no host input and no debugger; the
 * exceptions must come at the same steps, and the end must be the
 * recorded end, or it says where the runs diverged (status 5). */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "net.h"

#define RX_POLL 4096

enum { R_RX, R_EXC, R_READC, R_WAKE, R_REG, R_MEM, R_RESET, R_KILL, R_I2C };
static const char *const kinds[] = { "rx", "exc", "readc", "wake", "reg", "mem",
                                     "reset", "kill", "i2c" };
/* an i2c event's ask and answer, on its line */
#define I2C_SEPARATOR " | "

struct rev {
    u64 step, insns;
    u64 count, dstep, dinsns;       /* exc: a run of COUNT, at these strides */
    int kind;
    long v;                         /* rx's byte, exc's number, readc's value... */
    u32 addr;                       /* mem's address */
    u8 *data;                       /* reg's and mem's bytes */
    int len;
    char *ask, *answer;             /* i2c's exchange */
};

struct rec {
    struct sim *s;
    FILE *out;                      /* --record */
    FILE *in;                       /* --input */
    int in_eof;
    u64 last_poll;
    struct rev *ev;                 /* --replay's events */
    int nev, at;
    char end[256];                  /* --replay's recorded end */
    int replay;
    u64 out_hash, out_n;            /* the program's output, so far */
    /* --record: the run of one exception not yet written */
    int run_n;
    u64 run_step, run_insns, run_count, run_ds, run_di, last_step, last_insns;
};

static void *xalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static u64 steps(struct sim *s)
{
    return s->an ? s->an->steps : 0;
}

/* FNV-1a, 64 bits */
static u64 fnv(u64 h, const u8 *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static u64 file_hash(const char *path)
{
    FILE *f = fopen(path, "rb");
    u8 buf[4096];
    size_t n;
    u64 h = 0xcbf29ce484222325ull;
    if (!f)
        die("cannot open %s", path);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        h = fnv(h, buf, n);
    fclose(f);
    return h;
}

/* the machine's state at the end: the registers of a `g` packet, and
 * every memory region */
static u64 state_hash(struct sim *s)
{
    u64 h = 0xcbf29ce484222325ull;
    struct cpu *c = s->cpu;
    for (const int *r = c->ops->gdb_g_regs(c); *r >= 0; r++) {
        u8 b[16];
        int n = c->ops->reg_read(c, *r, b);
        h = fnv(h, b, (size_t)n);
    }
    for (int i = 0; i < s->bus.nrg; i++)
        h = fnv(h, s->bus.rg[i].mem, s->bus.rg[i].size);
    return h;
}

/* the runs went apart: where, and why; status 5 */
static void diverged(struct sim *s, const char *fmt, ...)
{
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "embsim: replay diverged at step %llu (%llu instructions): ",
            (unsigned long long)steps(s), (unsigned long long)s->insns);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(5);
}

/* the run of exceptions not yet written */
static void flush_run(struct rec *r)
{
    if (!r->out || !r->run_count)
        return;
    fprintf(r->out, "@%llu %llu exc %d", (unsigned long long)r->run_step,
            (unsigned long long)r->run_insns, r->run_n);
    if (r->run_count > 1)
        fprintf(r->out, " x %llu +%llu +%llu", (unsigned long long)r->run_count,
                (unsigned long long)r->run_ds, (unsigned long long)r->run_di);
    fputc('\n', r->out);
    r->run_count = 0;
}

/* an event's line: "@STEP INSNS KIND", then what fmt says */
static void put_ev(struct rec *r, u64 step, int kind, const char *fmt, ...)
{
    va_list ap;
    if (!r->out)
        return;
    flush_run(r);
    fprintf(r->out, "@%llu %llu %s", (unsigned long long)step,
            (unsigned long long)r->s->insns, kinds[kind]);
    if (fmt) {
        fputc(' ', r->out);
        va_start(ap, fmt);
        vfprintf(r->out, fmt, ap);
        va_end(ap);
    }
    fputc('\n', r->out);
}

static void put_hex(FILE *f, const u8 *b, int n)
{
    for (int i = 0; i < n; i++)
        fprintf(f, "%02x", b[i]);
    fputc('\n', f);
}

/* ---- the input -------------------------------------------------------- */

void rec_input(struct sim *s, const char *path)
{
    struct rec *r = s->rec;
    r->in = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!r->in)
        die("cannot read %s", path);
    setvbuf(r->in, 0, _IONBF, 0);       /* readiness is the descriptor's */
    /* a console waits on what the UART answers: each byte reaches a pipe as
     * it goes, not when stdio's buffer fills */
    if (r->in == stdin)
        setvbuf(stdout, 0, _IONBF, 0);
    s->rx_on = 1;
}

/* a byte to the receiver, recorded */
static void deliver(struct rec *r, int c)
{
    struct sim *s = r->s;
    s->rx.put(s->rx.ctx, c);
    put_ev(r, steps(s), R_RX, "%02x", c & 0xff);
}

static void poll_input(struct rec *r)
{
    struct sim *s = r->s;
    if (!r->in || r->in_eof || !s->rx.room || !s->rx.room(s->rx.ctx))
        return;
    int idle = s->state == END_IDLE;
    if (!idle && (s->state != RUN || steps(s) - r->last_poll < RX_POLL))
        return;
    r->last_poll = steps(s);
    if (!idle && !net_file_ready(r->in))
        return;
    /* a byte, or (idle) the wait for one: the machine has nothing else
     * to do */
    int c = getc(r->in);
    if (c == EOF) {
        r->in_eof = 1;
        return;
    }
    deliver(r, c);
    if (idle) {
        s->state = RUN;
        s->end_why[0] = 0;
        put_ev(r, steps(s), R_WAKE, 0);
    }
}

/* ---- the events ----------------------------------------------------------- */

void rec_exc(struct sim *s, u32 n)
{
    struct rec *r = s->rec;
    u64 st = steps(s) + 1;              /* during the step in progress */
    if (!r->replay) {
        if (!r->out)
            return;
        /* the same exception again, at the run's strides: one more of it */
        if (r->run_count && (int)n == r->run_n &&
            (r->run_count == 1 || (st - r->last_step == r->run_ds &&
                                   s->insns - r->last_insns == r->run_di))) {
            if (r->run_count == 1) {
                r->run_ds = st - r->last_step;
                r->run_di = s->insns - r->last_insns;
            }
            r->run_count++;
        } else {
            flush_run(r);
            r->run_n = (int)n;
            r->run_step = st;
            r->run_insns = s->insns;
            r->run_count = 1;
        }
        r->last_step = st;
        r->last_insns = s->insns;
        return;
    }
    if (r->at >= r->nev)
        diverged(s, "exception %u came, after the recording's last event", n);
    struct rev *e = &r->ev[r->at];
    if (e->kind != R_EXC || e->step != st)
        diverged(s, "exception %u came; the recording has %s at step %llu next", n,
                 kinds[e->kind], (unsigned long long)e->step);
    if ((u32)e->v != n || e->insns != s->insns)
        diverged(s, "exception %u came; the recording has exception %ld here, at "
                 "%llu instructions", n, e->v, (unsigned long long)e->insns);
    /* the next of a run, or the next event */
    if (--e->count) {
        e->step += e->dstep;
        e->insns += e->dinsns;
    } else
        r->at++;
}

int rec_replaying(struct sim *s)
{
    return s->rec && s->rec->replay;
}

void rec_i2c(struct sim *s, const char *ask, const char *answer)
{
    struct rec *r = s->rec;
    if (r)
        put_ev(r, steps(s) + 1, R_I2C, "%s%s%s", ask, I2C_SEPARATOR, answer);
}

void rec_i2c_replay(struct sim *s, const char *ask, char *answer, size_t n)
{
    struct rec *r = s->rec;
    u64 st = steps(s) + 1;
    if (r->at >= r->nev || r->ev[r->at].kind != R_I2C || r->ev[r->at].step != st)
        diverged(s, "the I2C bus was asked '%s'; the recording has no exchange here", ask);
    struct rev *e = &r->ev[r->at++];
    if (strcmp(e->ask, ask))
        diverged(s, "the I2C bus was asked '%s'; the recording asked '%s'", ask, e->ask);
    snprintf(answer, n, "%s", e->answer);
}

int sim_readc(struct sim *s)
{
    struct rec *r = s->rec;
    if (!r)
        return getchar();
    u64 st = steps(s) + 1;
    if (!r->replay) {
        int c = getchar();
        put_ev(r, st, R_READC, "%d", c);
        return c;
    }
    if (r->at >= r->nev || r->ev[r->at].kind != R_READC || r->ev[r->at].step != st)
        diverged(s, "SYS_READC was called; the recording has no read here");
    return (int)r->ev[r->at++].v;
}

void rec_reg(struct sim *s, int n, const u8 *b, int len)
{
    struct rec *r = s->rec;
    if (!r || !r->out)
        return;
    flush_run(r);
    fprintf(r->out, "@%llu %llu reg %d ", (unsigned long long)steps(s),
            (unsigned long long)s->insns, n);
    put_hex(r->out, b, len);
}

void rec_mem(struct sim *s, u32 a, const u8 *b, int len)
{
    struct rec *r = s->rec;
    if (!r || !r->out || len <= 0)
        return;
    flush_run(r);
    fprintf(r->out, "@%llu %llu mem %08x ", (unsigned long long)steps(s),
            (unsigned long long)s->insns, a);
    put_hex(r->out, b, len);
}

/* a debugger's memory write, as gdb.c makes it: the widest aligned
 * accesses, so a device register gets the access it got */
static void mem_write(struct sim *s, u32 a, const u8 *b, int len)
{
    for (int i = 0; i < len;) {
        u32 x = a + (u32)i, v = 0;
        int n = (len - i >= 4 && !(x & 3)) ? 4 : (len - i >= 2 && !(x & 1)) ? 2 : 1;
        for (int k = 0; k < n; k++)
            v |= (u32)b[i + k] << (8 * k);
        bus_debug_write(&s->bus, x, n, v);
        i += n;
    }
}

void rec_out(struct sim *s, int c)
{
    struct rec *r = s->rec;
    u8 b = (u8)c;
    if (!r->out_n)
        r->out_hash = 0xcbf29ce484222325ull;
    r->out_hash = fnv(r->out_hash, &b, 1);
    r->out_n++;
}

void rec_wake(struct sim *s)
{
    if (s->rec)
        put_ev(s->rec, steps(s), R_WAKE, 0);
}

void rec_reset(struct sim *s, int reload)
{
    if (s->rec)
        put_ev(s->rec, steps(s), R_RESET, "%d", reload);
}

void rec_kill(struct sim *s)
{
    if (s->rec)
        put_ev(s->rec, steps(s), R_KILL, 0);
}

/* after each step: the input, or the recorded events of this step */
void rec_step(struct sim *s)
{
    struct rec *r = s->rec;
    if (!r->replay) {
        poll_input(r);
        return;
    }
    u64 st = steps(s);
    int woke = 0;
    while (r->at < r->nev && r->ev[r->at].step <= st) {
        struct rev *e = &r->ev[r->at];
        if (e->kind == R_EXC || e->kind == R_READC || e->kind == R_I2C || e->step < st)
            diverged(s, "the recording has %s at step %llu, which did not come",
                     kinds[e->kind], (unsigned long long)e->step);
        if (e->insns != s->insns)
            diverged(s, "%s: the recording is at %llu instructions here",
                     kinds[e->kind], (unsigned long long)e->insns);
        switch (e->kind) {
        case R_RX:
            if (!s->rx.room || !s->rx.room(s->rx.ctx))
                diverged(s, "the receiver has no room for the recorded byte");
            s->rx.put(s->rx.ctx, (int)e->v);
            break;
        case R_WAKE:
            woke = 1;
            break;
        case R_REG:
            s->cpu->ops->reg_write(s->cpu, (int)e->v, e->data);
            break;
        case R_MEM:
            mem_write(s, e->addr, e->data, e->len);
            break;
        case R_RESET:
            sim_reset(s, (int)e->v);
            break;
        case R_KILL:
            s->exit_status = 0;
            sim_end(s, END_EXIT, "the debugger killed it");
            break;
        }
        r->at++;
    }
    if (woke && s->state == END_IDLE) {
        s->state = RUN;
        s->end_why[0] = 0;
    }
}

/* ---- the record's header and end -------------------------------------- */

static void hexstr(FILE *f, const char *s)
{
    if (!s) {
        fprintf(f, "-\n");
        return;
    }
    put_hex(f, (const u8 *)s, (int)strlen(s));
}

void rec_record(struct sim *s, const char *path, const struct rec_machine *m)
{
    struct rec *r = s->rec;
    r->out = fopen(path, "w");
    if (!r->out)
        die("cannot write %s", path);
    fprintf(r->out, "embsim-record 1\n");
    fprintf(r->out, "image %s\n", m->image);
    fprintf(r->out, "image-hash %016llx\n", (unsigned long long)file_hash(m->image));
    fprintf(r->out, "board %s\n", m->board);
    fprintf(r->out, "cpu %s\n", m->cpu ? m->cpu : "-");
    fprintf(r->out, "ram-size %lu\n", (unsigned long)m->ram_size);
    fprintf(r->out, "svd %s\n", m->svd ? m->svd : "-");
    fprintf(r->out, "semihosting %d\n", m->semihosting);
    fprintf(r->out, "max-insns %llu\n", (unsigned long long)m->max_insns);
    fprintf(r->out, "until ");
    hexstr(r->out, m->until);
    fprintf(r->out, "input %d\n", s->rx_on);
    fprintf(r->out, "dfp %s\n", m->dfp ? m->dfp : "-");
    for (int i = 0; i < m->nset; i++)
        fprintf(r->out, "set %s\n", m->sets[i]);
    fprintf(r->out, "i2c-bus %d\n", m->i2c_bus);
    fprintf(r->out, "reset-runs-on %d\n", m->reset_runs_on);
    fprintf(r->out, "events\n");
}

static char *dup(const char *s)
{
    char *d = xalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

static char *unhex(const char *h)
{
    size_t n = strlen(h) / 2;
    char *d = xalloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1)
            return 0;
        d[i] = (char)v;
    }
    return d;
}

struct rec *rec_create(struct sim *s)
{
    struct rec *r = xalloc(sizeof *r);
    r->s = s;
    s->rec = r;
    return r;
}

struct rec *rec_load(const char *path, struct rec_machine *m)
{
    struct rec *r = xalloc(sizeof *r);
    FILE *f = fopen(path, "r");
    static char line[1 << 16];
    char *big = 0;
    size_t bigcap = 0;
    if (!f)
        die("cannot read %s", path);
    r->replay = 1;
    memset(m, 0, sizeof *m);
    if (!fgets(line, sizeof line, f) || strcmp(line, "embsim-record 1\n"))
        die("%s is not an EmbSim recording", path);
    for (;;) {
        if (!fgets(line, sizeof line, f))
            die("%s: the recording is cut short", path);
        line[strcspn(line, "\n")] = 0;
        char *v = strchr(line, ' ');
        if (!strcmp(line, "events"))
            break;
        if (!v)
            die("%s: a bad header line '%s'", path, line);
        *v++ = 0;
        if (!strcmp(line, "image"))
            m->image = dup(v);
        else if (!strcmp(line, "image-hash"))
            m->image_hash = strtoull(v, 0, 16);
        else if (!strcmp(line, "board"))
            m->board = dup(v);
        else if (!strcmp(line, "cpu"))
            m->cpu = strcmp(v, "-") ? dup(v) : 0;
        else if (!strcmp(line, "ram-size"))
            m->ram_size = (u32)strtoul(v, 0, 10);
        else if (!strcmp(line, "svd"))
            m->svd = strcmp(v, "-") ? dup(v) : 0;
        else if (!strcmp(line, "semihosting"))
            m->semihosting = atoi(v);
        else if (!strcmp(line, "max-insns"))
            m->max_insns = strtoull(v, 0, 10);
        else if (!strcmp(line, "until"))
            m->until = strcmp(v, "-") ? unhex(v) : 0;
        else if (!strcmp(line, "input"))
            m->input = atoi(v);
        else if (!strcmp(line, "dfp"))
            m->dfp = strcmp(v, "-") ? dup(v) : 0;
        else if (!strcmp(line, "i2c-bus"))
            m->i2c_bus = atoi(v);
        else if (!strcmp(line, "reset-runs-on"))
            m->reset_runs_on = atoi(v);
        else if (!strcmp(line, "set")) {
            const char **ns = realloc((void *)m->sets,
                                      (size_t)(m->nset + 1) * sizeof *ns);
            if (!ns)
                die("out of memory");
            ns[m->nset++] = dup(v);
            m->sets = ns;
        }
    }
    /* the events: lines of any length (a debugger's `load` is long) */
    for (;;) {
        size_t n = 0;
        int c;
        while ((c = getc(f)) != EOF && c != '\n') {
            if (n + 1 >= bigcap) {
                bigcap = bigcap ? 2 * bigcap : 4096;
                big = realloc(big, bigcap);
                if (!big)
                    die("out of memory");
            }
            big[n++] = (char)c;
        }
        if (c == EOF && !n)
            break;
        if (!big)
            continue;
        big[n] = 0;
        if (!strncmp(big, "end ", 4)) {
            snprintf(r->end, sizeof r->end, "%s", big + 4);
            continue;
        }
        unsigned long long st, in;
        char kind[16];
        int used = 0;
        if (sscanf(big, "@%llu %llu %15s %n", &st, &in, kind, &used) < 3)
            die("%s: a bad event '%s'", path, big);
        struct rev e;
        memset(&e, 0, sizeof e);
        e.step = st;
        e.insns = in;
        e.kind = -1;
        for (int k = 0; k < (int)(sizeof kinds / sizeof kinds[0]); k++)
            if (!strcmp(kind, kinds[k]))
                e.kind = k;
        const char *p = big + used;
        switch (e.kind) {
        case R_RX: e.v = strtol(p, 0, 16); break;
        case R_EXC: {
            unsigned long long k = 1, ds = 0, di = 0;
            e.v = (long)strtoul(p, 0, 10);
            const char *x = strstr(p, " x ");
            if (x && sscanf(x, " x %llu +%llu +%llu", &k, &ds, &di) != 3)
                die("%s: a bad event '%s'", path, big);
            if (!k)
                die("%s: a bad event '%s'", path, big);
            e.count = k;
            e.dstep = ds;
            e.dinsns = di;
            break;
        }
        case R_READC: case R_RESET: e.v = strtol(p, 0, 10); break;
        case R_I2C: {
            const char *bar = strstr(p, I2C_SEPARATOR);
            if (!bar)
                die("%s: a bad event '%s'", path, big);
            e.ask = xalloc((size_t)(bar - p) + 1);
            memcpy(e.ask, p, (size_t)(bar - p));
            e.answer = dup(bar + strlen(I2C_SEPARATOR));
            break;
        }
        case R_WAKE: case R_KILL: break;
        case R_REG: case R_MEM: {
            char *q;
            unsigned long x = strtoul(p, &q, e.kind == R_MEM ? 16 : 10);
            const char *h = q + (*q == ' ');
            e.len = (int)(strlen(h) / 2);
            e.data = xalloc((size_t)e.len + 1);
            for (int i = 0; i < e.len; i++) {
                unsigned b;
                if (sscanf(h + 2 * i, "%2x", &b) != 1)
                    die("%s: a bad event '%.60s'", path, big);
                e.data[i] = (u8)b;
            }
            if (e.kind == R_MEM)
                e.addr = (u32)x;
            else
                e.v = (long)x;
            break;
        }
        default:
            die("%s: an unknown event '%s'", path, kind);
        }
        r->ev = realloc(r->ev, (size_t)(r->nev + 1) * sizeof *r->ev);
        if (!r->ev)
            die("out of memory");
        r->ev[r->nev++] = e;
    }
    free(big);
    fclose(f);
    if (!r->end[0])
        die("%s: the recording has no end (was the recorded run stopped?)", path);
    return r;
}

void rec_attach(struct sim *s, struct rec *r, const struct rec_machine *m)
{
    r->s = s;
    s->rec = r;
    s->rx_on = m->input;
}

void rec_check_image(const char *path, const struct rec_machine *m)
{
    u64 h = file_hash(path);
    if (h != m->image_hash)
        die("%s is not the image the recording ran (its hash is %016llx, the "
            "recording's %016llx)", path, (unsigned long long)h,
            (unsigned long long)m->image_hash);
}

/* the events before the first step: a debugger's, at reset */
void rec_start(struct sim *s)
{
    if (s->rec->replay)
        rec_step(s);
}

void rec_finish(struct sim *s)
{
    struct rec *r = s->rec;
    char end[256];
    snprintf(end, sizeof end, "%llu %llu %llu %d %d %016llx %llu %016llx",
             (unsigned long long)steps(s), (unsigned long long)s->insns,
             (unsigned long long)s->cycles, s->state, s->exit_status,
             (unsigned long long)state_hash(s), (unsigned long long)r->out_n,
             (unsigned long long)r->out_hash);
    if (r->out) {
        flush_run(r);
        fprintf(r->out, "end %s\n", end);
        fclose(r->out);
        r->out = 0;
    }
    if (!r->replay)
        return;
    if (r->at < r->nev)
        diverged(s, "the run ended before the recording's %s at step %llu",
                 kinds[r->ev[r->at].kind], (unsigned long long)r->ev[r->at].step);
    if (strcmp(end, r->end)) {
        fflush(stdout);
        fprintf(stderr, "embsim: replay diverged: the run ended at\n"
                        "  steps, instructions, cycles, state, status, the state's "
                        "hash, the output's bytes and hash:\n"
                        "  this run's:      %s\n"
                        "  the recording's: %s\n", end, r->end);
        exit(5);
    }
    fflush(stdout);
    fprintf(stderr, "embsim: replay: the run is the recording's (%llu steps, "
                    "%llu instructions, %d events)\n",
            (unsigned long long)steps(s), (unsigned long long)s->insns, r->nev);
}
