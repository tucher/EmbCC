/* run.c -- building the machine, the run loop, time and the run's end.
 *
 * Time is the core's estimate of the cycles it has run (tools/bench's
 * cost table), never the host's clock: the devices that keep time are
 * told how many cycles each instruction took (sim_advance), so a run
 * gives the same answer every time, whatever it is debugged or traced
 * with. A run ends when:
 *   - the image exits through semihosting, or requests a reset;
 *   - the core locks up (a fault with nowhere to go);
 *   - it waits in a loop or a WFI that nothing can interrupt;
 *   - the output contains --until's string;
 *   - --max-insns runs out. */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"
#include "svd-map.h"

void die(const char *fmt, ...)
{
    va_list ap;
    fputs("embsim: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void sim_end(struct sim *s, int state, const char *fmt, ...)
{
    va_list ap;
    if (state == END_IDLE && s->never_idle)
        return;
    s->state = state;
    va_start(ap, fmt);
    vsnprintf(s->end_why, sizeof s->end_why, fmt, ap);
    va_end(ap);
}

void sim_out(struct sim *s, int c)
{
    putc(c, s->out ? s->out : stdout);
    if (s->rec)
        rec_out(s, c);
    if (s->until) {
        /* a plain prefix automaton is enough for the sentinels harnesses
         * print: restart the match at this byte when it breaks */
        if ((u8)s->until[s->until_at] == (u8)c)
            s->until_at++;
        else
            s->until_at = (u8)s->until[0] == (u8)c;
        if (s->until_at == s->until_len && s->state == RUN)
            sim_end(s, END_UNTIL, "the output reached \"%s\"", s->until);
    }
}

void sim_advance(struct sim *s, u32 cycles)
{
    for (int i = 0; i < s->ntick_fn; i++)
        s->tick_fn[i](s->tick_ctx[i], cycles);
}

void sim_rx_port(struct sim *s, int (*room)(void *ctx),
                 void (*put)(void *ctx, int c), void *ctx)
{
    if (s->rx.room)
        return;
    s->rx.room = room;
    s->rx.put = put;
    s->rx.ctx = ctx;
}

void sim_clock(struct sim *s, int *running, int on)
{
    on = on != 0;
    if (*running != on) {
        s->counting += on ? 1 : -1;
        s->bus.touched = 1;
    }
    *running = on;
}

int sim_event_coming(struct sim *s)
{
    if (!s->ev_known || s->bus.touched || (s->ev_any && s->cycles >= s->ev_at)) {
        u32 left;
        s->ev_any = sim_next_event(s, &left);
        s->ev_at = s->cycles + (s->ev_any ? left : 0);
        s->ev_known = 1;
        s->bus.touched = 0;
    }
    return s->ev_any;
}

int sim_next_event(struct sim *s, u32 *cycles)
{
    int any = 0;
    for (int i = 0; i < s->ntick; i++) {
        u32 c;
        struct device *d = s->tick[i];
        if (d->ops->next_event && d->ops->next_event(d->ctx, &c) &&
            (!any || c < *cycles)) {
            *cycles = c;
            any = 1;
        }
    }
    return any;
}

void sim_init(struct sim *s, const struct board_desc *bd, const char *model,
              const char *svd)
{
    memset(s, 0, sizeof *s);
    s->board = bd;
    s->semihosting = 1;
    s->state = RUN;
    const struct core_type *ct = 0;
    for (int i = 0; i < ncores; i++)
        if (!strcmp(cores[i].name, bd->core))
            ct = &cores[i];
    if (!model)
        model = bd->cpu;
    if (!ct || !ct->create(s, model, bd))
        die("unknown cpu '%s'", model);
    /* the SVD's peripherals go before the board's space that reads as
     * zero, so where they are, they answer; and at the end with no such
     * space */
    if (!svd && bd->svd) {
        svd = svd_search(bd->svd);
        if (!svd && (strstr(bd->svd, ".atdf") || strstr(bd->svd, ".PIC")))
            die("the %s board's peripherals are %s's, from its device pack: "
                "give the pack's directory with --dfp DIR (it holds "
                "%s/%s), the file with --svd FILE, or a directory holding "
                "it in EMBSIM_SVD_PATH", bd->name, bd->svd,
                strstr(bd->svd, ".PIC") ? "edc" : "atdf", bd->svd);
        if (!svd)
            die("the %s board's peripherals are %s's: give its path with "
                "--svd FILE, or a directory holding it in EMBSIM_SVD_PATH",
                bd->name, bd->svd);
    }
    struct svdmap *m = svd ? svdmap_create(s, svd) : 0;
    for (int i = 0; i < BOARD_DEVS && bd->dev[i].type; i++) {
        if (m && !strcmp(bd->dev[i].type, "zero")) {
            svdmap_add(s, m);
            m = 0;
        }
        sim_add_device(s, &bd->dev[i]);
    }
    if (m)
        svdmap_add(s, m);
    if (s->svd)
        svdmap_models(s, s->svd, bd->models);
    for (int i = 0; i < 2 && bd->bitband[i].size; i++)
        bus_add_alias(&s->bus, bd->bitband[i].base, bd->bitband[i].size,
                      bd->bitband[i].target);
}

/* the image goes into the board's memory -- flash too, which stores from
 * the core then leave alone -- and the core comes out of reset */
static void load(struct sim *s)
{
    for (int i = 0; i < s->bus.nrg; i++)
        s->bus.rg[i].rom = 0;
    load_elf(s, s->image);
    for (int i = 0; i < s->bus.nrg; i++)
        s->bus.rg[i].rom = s->bus.rg[i].kind == MEM_FLASH;
    s->cpu->ops->reset(s->cpu);
}

void sim_load(struct sim *s, u32 ram_size, const char *image)
{
    const struct board_desc *bd = s->board;
    for (int i = 0; i < BOARD_MEMS && bd->mem[i].size; i++)
        if (bd->mem[i].kind == MEM_ALIAS)
            bus_add_mirror(&s->bus, bd->mem[i].base, bd->mem[i].target);
        else {
            bus_add_region(&s->bus, bd->mem[i].base,
                           bd->mem[i].main_ram && ram_size ? ram_size
                                                           : bd->mem[i].size,
                           bd->mem[i].kind);
            s->bus.rg[s->bus.nrg - 1].erased = bd->mem[i].erased;
            bus_erase(&s->bus.rg[s->bus.nrg - 1]);
        }
    s->image = image;
    load(s);
}

void sim_reset(struct sim *s, int reload)
{
    if (reload)
        for (int i = 0; i < s->bus.nrg; i++)
            bus_erase(&s->bus.rg[i]);
    for (int i = 0; i < s->bus.ndev; i++)
        if (s->bus.dev[i].ops->reset)
            s->bus.dev[i].ops->reset(s->bus.dev[i].ctx);
    s->insns = s->cycles = 0;
    s->counting = 0;
    s->ev_known = 0;
    s->state = RUN;
    s->exit_status = 0;
    s->end_why[0] = 0;
    s->until_at = 0;
    s->bus.watch_hit = 0;
    if (reload)
        load(s);
    else
        s->cpu->ops->reset(s->cpu);
}

/* The counts go on, and so do the clock's holders outside the part (an I2C
 * endpoint's): the devices drop their own as they reset. */
void sim_part_reset(struct sim *s)
{
    for (int i = 0; i < s->bus.ndev; i++)
        if (s->bus.dev[i].ops->reset)
            s->bus.dev[i].ops->reset(s->bus.dev[i].ctx);
    s->ev_known = 0;
    s->cpu->ops->reset(s->cpu);
}

/* one step and the budget: the loop's body, for the GDB server's loop
 * too */
void sim_step(struct sim *s)
{
    if (s->an) {
        an_step(s);
        return;
    }
    s->cpu->ops->step(s->cpu);
    if (s->max_insns && s->insns >= s->max_insns && s->state == RUN)
        sim_end(s, END_BUDGET, "--max-insns: %llu instructions run",
                (unsigned long long)s->insns);
}

/* Several parts in one run: each step goes to the part furthest behind in
 * cycles, so the parts stay within an instruction of each other; the run
 * ends when one of them ends. */
struct sim *sim_run_parts(struct sim *const *part, int n)
{
    for (;;) {
        struct sim *next = 0;
        for (int i = 0; i < n; i++) {
            if (part[i]->state != RUN)
                return part[i];
            if (!next || part[i]->cycles < next->cycles)
                next = part[i];
        }
        sim_step(next);
    }
}

void sim_run(struct sim *s)
{
    struct cpu *c = s->cpu;
    void (*step)(struct cpu *) = c->ops->step;
    if (s->an) {
        while (s->state == RUN)
            an_step(s);
        return;
    }
    while (s->state == RUN) {
        step(c);
        if (s->max_insns && s->insns >= s->max_insns && s->state == RUN)
            sim_end(s, END_BUDGET, "--max-insns: %llu instructions run",
                    (unsigned long long)s->insns);
    }
}
