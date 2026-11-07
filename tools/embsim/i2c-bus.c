/* i2c-bus.c -- the I2C bus endpoint (--i2c-bus): what happens on a part's
 * I2C buses outside the part. The part's I2C units ask it and wait for its
 * answer, by i2c-bus.def's protocol, over a TCP connection the endpoint
 * makes; a replay takes the answers from the recording and needs none.
 *
 *   - Polled after every instruction once the core reaches the cycle the
 *     endpoint's last "idle" named: nothing outside happens before it, so
 *     the run asks again only then, when its own master has started
 *     (another master may start with it), or when it has stopped (a slave
 *     may answer what it was sent).
 *   - A write another master starts goes to the unit that drives that bus
 *     (i2cbus_attach), which clocks it into its slave and says how far the
 *     slave acknowledged it (i2cbus_delivered). A bus no unit drives ends
 *     the run: nothing in the part could take the write.
 *   - An input the endpoint sets goes to the board as --set's would; one the
 *     board has not got ends the run. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "net.h"

#define I2C_VERSION(v) enum { I2C_PROTOCOL_VERSION = (v) };
#define I2C_ASK(n, words, answers)
#define I2C_ANSWER(n, words)
#include "i2c-bus.def"
#undef I2C_VERSION
#undef I2C_ASK
#undef I2C_ANSWER

#define I2C_VERSION(v)
#define I2C_ASK(n, words, answers) ASK_##n,
#define I2C_ANSWER(n, words)
enum {
#include "i2c-bus.def"
    ASKS
};
#undef I2C_ASK
#define I2C_ASK(n, words, answers)
#undef I2C_ANSWER
#define I2C_ANSWER(n, words) ANSWER_##n,
enum {
#include "i2c-bus.def"
    ANSWERS
};
#undef I2C_ANSWER
#define I2C_ANSWER(n, words)
#undef I2C_ASK
#define I2C_ASK(n, words, answers) words,
static const char *const ask_words[] = {
#include "i2c-bus.def"
};
#undef I2C_ASK
#define I2C_ASK(n, words, answers) answers,
static const char *const ask_answers[] = {
#include "i2c-bus.def"
};
#undef I2C_ASK
#define I2C_ASK(n, words, answers)
#undef I2C_ANSWER
#define I2C_ANSWER(n, words) words,
static const char *const answer_words[] = {
#include "i2c-bus.def"
};
#undef I2C_ANSWER
#define I2C_ANSWER(n, words) #n,
static const char *const answer_names[] = {
#include "i2c-bus.def"
};
#undef I2C_VERSION
#undef I2C_ASK
#undef I2C_ANSWER

#define I2C_UNITS 4
/* an ask or an answer: a master's write of up to 255 bytes and its words */
#define LINE_MAX 1024
#define NO_POLL UINT64_MAX

struct unit {
    void *ctx;
    void (*inject)(void *ctx, u64 at, const u8 *b, int n);
};

struct i2cbus {
    struct sim *s;
    int on;                         /* an endpoint is connected, or replayed */
    int counting;                   /* its poll keeps the core's clock counting */
    int fd;                         /* the endpoint, or -1 while replaying */
    u64 next_poll;
    struct unit unit[I2C_UNITS + 1];
    char in[LINE_MAX];              /* what came of the next answer */
    int in_n;
};

/* the first word of a line's words: its verb */
static size_t verb_len(const char *words)
{
    return strcspn(words, " ");
}

static int answer_kind(const char *line)
{
    for (int k = 0; k < ANSWERS; k++) {
        size_t n = verb_len(answer_words[k]);
        if (!strncmp(line, answer_words[k], n) && (line[n] == ' ' || !line[n]))
            return k;
    }
    return -1;
}

/* whether `kind` is among the answers ASK `ask` takes */
static int allowed(int ask, int kind)
{
    const char *list = ask_answers[ask], *name = answer_names[kind];
    size_t n = strlen(name);
    for (const char *p = list; *p; p += strcspn(p, " "), p += *p == ' ')
        if (!strncmp(p, name, n) && (p[n] == ' ' || !p[n]))
            return 1;
    return 0;
}

static void read_line(struct i2cbus *b, char *line)
{
    for (;;) {
        char *nl = memchr(b->in, '\n', (size_t)b->in_n);
        if (nl) {
            int n = (int)(nl - b->in);
            memcpy(line, b->in, (size_t)n);
            line[n] = 0;
            b->in_n -= n + 1;
            memmove(b->in, nl + 1, (size_t)b->in_n);
            return;
        }
        if (b->in_n == LINE_MAX)
            die("--i2c-bus: an answer longer than %d bytes", LINE_MAX);
        net_ready(b->fd, -1);
        int got = net_read(b->fd, b->in + b->in_n, LINE_MAX - b->in_n);
        if (got <= 0)
            die("--i2c-bus: the endpoint closed the connection");
        b->in_n += got;
    }
}

/* one exchange: ASK `ask`'s verb, then what fmt says; the answer's kind,
 * its words in `answer` */
static int ask(struct i2cbus *b, int ask, char *answer, const char *fmt, ...)
{
    char line[LINE_MAX];
    size_t n = verb_len(ask_words[ask]);
    memcpy(line, ask_words[ask], n);
    line[n] = ' ';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n + 1, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    if (b->fd < 0) {
        rec_i2c_replay(b->s, line, answer, LINE_MAX);
    } else {
        size_t len = strlen(line);
        line[len] = '\n';
        if (net_write(b->fd, line, (int)len + 1))
            die("--i2c-bus: the endpoint closed the connection");
        line[len] = 0;
        read_line(b, answer);
        rec_i2c(b->s, line, answer);
    }
    int kind = answer_kind(answer);
    if (kind < 0 || !allowed(ask, kind))
        die("--i2c-bus: the endpoint answered '%s' to '%s'", answer, line);
    return kind;
}

static void expect_ok(struct i2cbus *b, int what, const char *fmt, ...)
{
    char args[LINE_MAX], answer[LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);
    ask(b, what, answer, "%s", args);
}

static u64 parse_u64(const char *p, const char *line)
{
    char *e;
    unsigned long long v = strtoull(p, &e, 10);
    if (e == p || (*e && *e != ' '))
        die("--i2c-bus: a bad number in '%s'", line);
    return v;
}

/* ---- polling: what the outside does next ------------------------------- */

static void poll_tick(void *ctx, u32 cycles)
{
    struct i2cbus *b = ctx;
    struct sim *s = b->s;
    (void)cycles;
    if (s->cycles < b->next_poll)
        return;
    char answer[LINE_MAX];
    int kind = ask(b, ASK_POLL, answer, "%llu", (unsigned long long)s->cycles);
    const char *p = answer + verb_len(answer_words[kind]) + 1;
    if (kind == ANSWER_IDLE) {
        u64 until = parse_u64(p, answer);
        if (until <= s->cycles)
            die("--i2c-bus: idle until %llu, which the run has reached", (unsigned long long)until);
        b->next_poll = until;
        return;
    }
    if (kind == ANSWER_PIN) {
        char name[64];
        const char *value = strchr(p, ' ');
        size_t n = value ? (size_t)(value - p) : 0;
        if (!n || n >= sizeof name || !value[1] || strchr(value + 1, ' '))
            die("--i2c-bus: a bad pin '%s'", answer);
        memcpy(name, p, n);
        name[n] = 0;
        if (!s->io.set || s->io.set(s->io.ctx, name, value + 1))
            die("--i2c-bus: '%s': the board has no input %s that takes %s", answer, name, value + 1);
        b->next_poll = s->cycles;
        return;
    }
    char *e;
    u64 at = strtoull(p, &e, 10);
    long bus = strtol(e, &e, 10);
    u8 bytes[LINE_MAX / 3];
    int n = 0;
    while (*e == ' ') {
        unsigned v;
        if (sscanf(e, " %2x", &v) != 1 || n == (int)sizeof bytes)
            die("--i2c-bus: a bad master's write '%s'", answer);
        bytes[n++] = (u8)v;
        e += 3;
    }
    if (*e || n == 0 || at < s->cycles)
        die("--i2c-bus: a bad master's write '%s' at cycle %llu", answer,
            (unsigned long long)s->cycles);
    if (bus < 1 || bus > I2C_UNITS || !b->unit[bus].inject)
        die("--i2c-bus: another master writes on bus %ld, which no I2C unit of "
            "the part drives", bus);
    b->next_poll = NO_POLL;
    b->unit[bus].inject(b->unit[bus].ctx, at, bytes, n);
}

/* ---- the part's side ------------------------------------------------------ */

/* the units attach when the board is built, before the endpoint opens */
static struct i2cbus *get(struct sim *s)
{
    if (!s->i2c) {
        s->i2c = calloc(1, sizeof *s->i2c);
        if (!s->i2c)
            die("out of memory");
        s->i2c->s = s;
        s->i2c->fd = -1;
    }
    return s->i2c;
}

int i2cbus_on(struct sim *s)
{
    return s->i2c && s->i2c->on;
}

void i2cbus_open(struct sim *s, const char *host, int port)
{
    struct i2cbus *b = get(s);
    b->on = 1;
    /* a core advances time for its devices only while one counts: the poll waits on time */
    sim_clock(s, &b->counting, 1);
    if (s->ntick_fn == SIM_TICKERS)
        die("too many devices that keep time");
    s->tick_fn[s->ntick_fn] = poll_tick;
    s->tick_ctx[s->ntick_fn++] = b;
    if (s->rec && rec_replaying(s))
        return;
    int listener = net_listen(host, port);
    if (listener < 0)
        die("--i2c-bus: cannot listen on port %d", port);
    fprintf(stderr, "embsim: --i2c-bus: waiting for the endpoint on port %d\n", port);
    b->fd = net_accept(listener, -1);
    net_unlisten(listener);
    if (b->fd < 0)
        die("--i2c-bus: no endpoint connected");
    char line[LINE_MAX], answer[LINE_MAX];
    snprintf(line, sizeof line, "%.*s %d\n", (int)verb_len(ask_words[ASK_HELLO]),
             ask_words[ASK_HELLO], I2C_PROTOCOL_VERSION);
    if (net_write(b->fd, line, (int)strlen(line)))
        die("--i2c-bus: the endpoint closed the connection");
    read_line(b, answer);
    if (answer_kind(answer) != ANSWER_OK)
        die("--i2c-bus: the endpoint answered '%s' to protocol version %d", answer,
            I2C_PROTOCOL_VERSION);
}

void i2cbus_attach(struct sim *s, int bus, void *ctx,
                   void (*inject)(void *ctx, u64 at, const u8 *b, int n))
{
    struct i2cbus *b = get(s);
    if (bus < 1 || bus > I2C_UNITS)
        die("I2C bus %d: the endpoint knows buses 1 to %d", bus, I2C_UNITS);
    b->unit[bus].ctx = ctx;
    b->unit[bus].inject = inject;
}

void i2cbus_delivered(struct sim *s, int bus, int count)
{
    struct i2cbus *b = s->i2c;
    expect_ok(b, ASK_DELIVERED, "%llu %d %d", (unsigned long long)s->cycles, bus, count);
    b->next_poll = s->cycles;
}

void i2cbus_beaten(struct sim *s, int bus, int count)
{
    struct i2cbus *b = s->i2c;
    expect_ok(b, ASK_BEATEN, "%llu %d %d", (unsigned long long)s->cycles, bus, count);
    b->next_poll = s->cycles;
}

void i2cbus_lost(struct sim *s, int bus)
{
    expect_ok(s->i2c, ASK_LOST, "%llu %d", (unsigned long long)s->cycles, bus);
}

void i2cbus_start(struct sim *s, int bus)
{
    struct i2cbus *b = s->i2c;
    expect_ok(b, ASK_START, "%llu %d", (unsigned long long)s->cycles, bus);
    b->next_poll = s->cycles;
}

void i2cbus_restart(struct sim *s, int bus)
{
    struct i2cbus *b = s->i2c;
    expect_ok(b, ASK_RESTART, "%llu %d", (unsigned long long)s->cycles, bus);
    b->next_poll = s->cycles;
}

void i2cbus_stop(struct sim *s, int bus)
{
    struct i2cbus *b = s->i2c;
    expect_ok(b, ASK_STOP, "%llu %d", (unsigned long long)s->cycles, bus);
    b->next_poll = s->cycles;
}

int i2cbus_write(struct sim *s, int bus, u8 byte)
{
    char answer[LINE_MAX];
    return ask(s->i2c, ASK_WRITE, answer, "%llu %d %02x", (unsigned long long)s->cycles,
               bus, byte) == ANSWER_ACK;
}

u8 i2cbus_read(struct sim *s, int bus)
{
    char answer[LINE_MAX];
    unsigned v;
    ask(s->i2c, ASK_READ, answer, "%llu %d", (unsigned long long)s->cycles, bus);
    if (sscanf(answer + verb_len(answer_words[ANSWER_DATA]), " %2x", &v) != 1)
        die("--i2c-bus: a bad byte in '%s'", answer);
    return (u8)v;
}

void i2cbus_master_ack(struct sim *s, int bus, int ack)
{
    expect_ok(s->i2c, ASK_MASTER_ACK, "%llu %d %s", (unsigned long long)s->cycles, bus,
              ack ? answer_words[ANSWER_ACK] : answer_words[ANSWER_NACK]);
}
