/* svd-map.c -- a CMSIS-SVD file's peripherals on the bus, as a register
 * file (svd-map.h says what that is, and how the models sit on it).
 *
 * The file is read by tools/embsvd/svd.c, embsvd's reader, so EmbSim
 * places every register where embsvd's header does: clusters, arrays
 * (dim and dimIncrement) and derivedFrom resolved, each array element a
 * register of its own. One device on the bus covers the peripherals'
 * span, from the lowest register to the highest; an access is sent to
 * the peripheral of the register it touches, that is, to its model's
 * hooks or the plain register file's.
 *
 * What the register file does, by the SVD's words:
 *   - a register starts at its resetValue, and again at each reset;
 *   - read-only fields ignore writes; write-only (and writeOnce) fields
 *     read as 0; a writeOnce or read-writeOnce field takes the first
 *     write after reset and ignores the rest;
 *   - modifiedWriteValues: oneToClear, oneToSet, oneToToggle, their
 *     zeroTo- forms, clear and set; modify is a plain write;
 *   - readAction: clear and set change the field after the read; modify
 *     and modifyExternal are the model's business;
 *   - bits that no field covers are reserved: they read as their reset
 *     value and ignore writes. A register without fields is one field;
 *   - a byte or halfword access reads or writes those bytes alone.
 * An access where no register is -- a hole in a peripheral, or between
 * peripherals -- is a bus fault, reported on stderr with the peripheral
 * and the offset, since a firmware's HardFault rarely says which store it
 * was. Two registers at one address (an alternateRegister, or two
 * peripherals that share their space, as Nordic's SPI0, SPIM0 and TWI0
 * do) are one: the first in the file names it. The core's own
 * peripherals (0xE0000000 up) are the core's devices, not the file's.
 *
 * --trace-periph prints each access to the peripherals it names, by the
 * SVD's names: the program's reads (R) and writes (W) field by field, and
 * what the hardware changed (H). */
#include <stdlib.h>
#include <string.h>

#include "../embsvd/svd.h"
#include "svd-map.h"

#define MAX_TICKERS 32
#define NO_EFFECT (~0ull)
/* map_tick's ticks: the models that may lag waiting, or the step's every model;
 * or those models catching up */
#define STEP_WAITING 1
#define STEP_ALL 2
#define STEP_CATCHING_UP 3

struct svdmap {
    struct sim *s;
    struct svd_device *d;
    const char *path;
    struct rf_periph *p;            /* the peripherals mapped, as in the file */
    int np;
    struct rf_reg **reg;            /* their registers, by address */
    int nreg;
    u32 lo, hi;                     /* the span the device covers */
    /* the peripherals whose model's clock runs: their hooks are looked up
     * at each tick, so a model may start its clock while it is created */
    struct rf_periph *tk[MAX_TICKERS];
    int ntk;
    /* the models that may lag (rf_lag) are ticked up to synced_at; due_at is
     * the cycle the first of them next acts at, when due_known */
    u64 synced_at, due_at;
    int due_known;
    int stepping;                   /* the ticks under way: STEP_* */
    int faults;                     /* bus faults reported */
    int rules;                      /* the board's RF_* */
};

static const struct dev_ops rf_plain_ops;

/* ---- names and codes ---------------------------------------------------- */

static int acc_code(const char *a)
{
    if (!a)
        return ACC_RW;
    return !strcmp(a, "read-only") ? ACC_RO
         : !strcmp(a, "write-only") ? ACC_WO
         : !strcmp(a, "writeOnce") ? ACC_W1
         : !strcmp(a, "read-writeOnce") ? ACC_RW1 : ACC_RW;
}

static const char *const mwv_names[] = {
    "modify", "oneToClear", "oneToSet", "oneToToggle", "zeroToClear",
    "zeroToSet", "zeroToToggle", "clear", "set",
};

static int mwv_code(const char *v, const char *p, const char *r)
{
    if (!v)
        return MWV_MODIFY;
    for (int i = 0; i < (int)(sizeof mwv_names / sizeof mwv_names[0]); i++)
        if (!strcmp(v, mwv_names[i]))
            return i;
    die("%s.%s: modifiedWriteValues '%s' is not one of the SVD's", p, r, v);
    return 0;
}

static int ract_code(const char *v, const char *p, const char *r)
{
    if (!v)
        return RA_NONE;
    if (!strcmp(v, "clear"))
        return RA_CLEAR;
    if (!strcmp(v, "set"))
        return RA_SET;
    if (!strcmp(v, "modify") || !strcmp(v, "modifyExternal"))
        return RA_MODIFY;
    die("%s.%s: readAction '%s' is not one of the SVD's", p, r, v);
    return 0;
}

/* `*` matches any characters */
static int glob(const char *pat, const char *s, size_t sl)
{
    if (!*pat)
        return sl == 0;
    if (*pat == '*') {
        for (size_t k = 0; k <= sl; k++)
            if (glob(pat + 1, s + k, sl - k))
                return 1;
        return 0;
    }
    return sl && *pat == *s && glob(pat + 1, s + 1, sl - 1);
}

static u64 field_mask(const struct rf_field *f)
{
    u64 m = f->width >= 64 ? ~0ULL : (1ULL << f->width) - 1;
    return m << f->lsb;
}

static u64 lanes(int at, int n)
{
    u64 m = n >= 8 ? ~0ULL : (1ULL << (8 * n)) - 1;
    return m << (8 * at);
}

/* ---- the map ---------------------------------------------------------- */

static int ull_cmp(const void *a, const void *b)
{
    unsigned long long x = *(const unsigned long long *)a;
    unsigned long long y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

static int u32_cmp(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return x < y ? -1 : x > y;
}

static int reg_cmp(const void *a, const void *b)
{
    const struct rf_reg *x = *(struct rf_reg *const *)a;
    const struct rf_reg *y = *(struct rf_reg *const *)b;
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1;
    return x < y ? -1 : x > y;      /* allocated in the file's order */
}

/* the peripheral's registers as rf_regs, appended to *all */
static void read_periph(struct svdmap *m, struct rf_periph *p,
                        struct rf_reg ***all, int *nall, int *cap)
{
    struct svd_flats fl = svd_flat_regs(p->svd, 1);
    struct rf_reg *regs = calloc((size_t)fl.n + 1, sizeof *regs);
    if (!regs)
        die("out of memory");
    u32 end = 0;
    for (int i = 0; i < fl.n; i++) {
        const struct svd_node *n = fl.v[i].r;
        struct rf_reg *r = &regs[i];
        r->addr = p->base + (u32)fl.v[i].off;
        r->bytes = n->size / 8;
        r->reset = r->value = n->reset;
        r->name = fl.v[i].path;
        fl.v[i].path = 0;
        r->p = p;
        r->nf = n->nf ? n->nf : 1;
        r->f = calloc((size_t)r->nf, sizeof *r->f);
        if (!r->f)
            die("out of memory");
        if (!n->nf) {
            r->f[0].width = n->size;
            r->f[0].acc = acc_code(n->acc);
            r->f[0].mwv = mwv_code(n->mwv, p->name, r->name);
            r->f[0].ract = ract_code(n->ract, p->name, r->name);
        }
        for (int k = 0; k < n->nf; k++) {
            const struct svd_field *sf = &n->f[k];
            struct rf_field *f = &r->f[k];
            f->name = sf->name;
            f->lsb = sf->lsb;
            f->width = sf->width;
            f->acc = acc_code(sf->acc);
            f->mwv = mwv_code(sf->mwv, p->name, r->name);
            f->ract = ract_code(sf->ract, p->name, r->name);
            f->svd = sf;
        }
        if ((u32)fl.v[i].off + (u32)r->bytes > end)
            end = (u32)fl.v[i].off + (u32)r->bytes;
        if (*nall == *cap) {
            *cap = *cap ? 2 * *cap : 1024;
            *all = realloc(*all, (size_t)*cap * sizeof **all);
            if (!*all)
                die("out of memory");
        }
        (*all)[(*nall)++] = r;
    }
    const struct svd_periph *sp = p->svd;
    for (int i = 0; i < sp->nab; i++)
        if (sp->ab[i].off + sp->ab[i].size > end)
            end = (u32)(sp->ab[i].off + sp->ab[i].size);
    p->end = p->base + end;
    svd_free_flats(&fl);
    (void)m;
}

/* A PIC32's CLR, SET and INV registers: at +4, +8 and +0xC from the
 * 32-bit registers the pack's processor module gives them (atdf.c), or
 * without it, from each 16-byte aligned one whose next three words no
 * register of the file holds -- which gives a few the part does not
 * have (CFGCON, DEVID). Each is a register of its own, named as the
 * device header names it (T2CONSET). */
static void add_clr_set_inv(struct svd_device *d, struct rf_reg ***all,
                            int *nall, int *cap)
{
    static const char *const suf[3] = { "CLR", "SET", "INV" };
    int n0 = *nall;
    u32 *at = malloc(sizeof *at * ((size_t)n0 + 1));
    if (!at)
        die("out of memory");
    for (int i = 0; i < n0; i++)
        at[i] = (*all)[i]->addr;
    qsort(at, (size_t)n0, sizeof *at, u32_cmp);
    for (int i = 0; i < n0; i++) {
        struct rf_reg *r = (*all)[i];
        if (r->bytes != 4)
            continue;
        if (d->nsci) {
            unsigned long long ra = r->addr;
            if (!bsearch(&ra, d->sci_at, (size_t)d->nsci, sizeof ra, ull_cmp))
                continue;
        } else if (r->addr & 15) {
            continue;
        }
        int free_after = 1;
        for (u32 k = 4; k <= 12 && free_after; k += 4) {
            u32 a = r->addr + k;
            free_after = !bsearch(&a, at, (size_t)n0, sizeof *at, u32_cmp);
        }
        if (!free_after)
            continue;
        struct rf_reg *x = calloc(3, sizeof *x);
        struct rf_field *f = calloc(3, sizeof *f);
        if (!x || !f)
            die("out of memory");
        for (int k = 0; k < 3; k++) {
            size_t l = strlen(r->name) + 4;
            char *nm = malloc(l);
            if (!nm)
                die("out of memory");
            snprintf(nm, l, "%s%s", r->name, suf[k]);
            x[k].addr = r->addr + 4 * (u32)(k + 1);
            x[k].bytes = 4;
            x[k].name = nm;
            x[k].p = r->p;
            f[k].width = 32;
            f[k].acc = ACC_WO;
            x[k].f = &f[k];
            x[k].nf = 1;
            x[k].base = r;
            x[k].sci = k + 1;
            r->alias[k] = &x[k];
            if (*nall == *cap) {
                *cap *= 2;
                *all = realloc(*all, (size_t)*cap * sizeof **all);
                if (!*all)
                    die("out of memory");
            }
            (*all)[(*nall)++] = &x[k];
            if (x[k].addr + 4 > r->p->end)
                r->p->end = x[k].addr + 4;
        }
    }
    free(at);
}

struct svdmap *svdmap_create(struct sim *s, const char *path)
{
    svd_prog = "embsim";
    svd_status = 2;
    svd_quiet = 1;
    struct svdmap *m = calloc(1, sizeof *m);
    if (!m)
        die("out of memory");
    m->s = s;
    m->path = path;
    m->rules = s->board ? s->board->rules : 0;
    size_t pl = strlen(path);
    m->d = pl > 5 && !strcmp(path + pl - 5, ".atdf") ? atdf_load(path)
         : pl > 4 && !strcmp(path + pl - 4, ".PIC") ? edc_load(path)
         : svd_load(path);
    m->p = calloc((size_t)m->d->np + 1, sizeof *m->p);
    if (!m->p)
        die("out of memory");
    struct rf_reg **all = 0;
    int nall = 0, cap = 0;
    for (int i = 0; i < m->d->np; i++) {
        const struct svd_periph *sp = &m->d->p[i];
        if (sp->base >= 0xE0000000u)
            continue;               /* the core's own: its devices answer */
        struct rf_periph *p = &m->p[m->np++];
        p->name = sp->name;
        p->base = (u32)(sp->base + m->d->bus_offset);
        p->svd = sp;
        p->sim = s;
        p->ops = &rf_plain_ops;
        p->ctx = p;
        read_periph(m, p, &all, &nall, &cap);
    }
    if (m->d->clr_set_inv)
        add_clr_set_inv(m->d, &all, &nall, &cap);
    /* by address; where two overlap, the first in the file is the one */
    if (nall)
        qsort(all, (size_t)nall, sizeof *all, reg_cmp);
    m->reg = calloc((size_t)nall + 1, sizeof *m->reg);
    if (!m->reg)
        die("out of memory");
    u64 at = 0;
    for (int i = 0; i < nall; i++)
        if (all[i]->addr >= at) {
            m->reg[m->nreg++] = all[i];
            at = (u64)all[i]->addr + (u64)all[i]->bytes;
            all[i]->p->nreg++;
        }
    free(all);
    for (int i = 0; i < m->np; i++) {
        m->p[i].reg = calloc((size_t)m->p[i].nreg + 1, sizeof *m->p[i].reg);
        if (!m->p[i].reg)
            die("out of memory");
        m->p[i].nreg = 0;
    }
    for (int i = 0; i < m->nreg; i++) {
        struct rf_periph *p = m->reg[i]->p;
        p->reg[p->nreg++] = m->reg[i];
    }
    if (!m->nreg)
        die("%s: no peripheral of %s has a register EmbSim can map", path,
            m->d->name);
    m->lo = m->reg[0]->addr;
    struct rf_reg *last = m->reg[m->nreg - 1];
    m->hi = last->addr + (u32)last->bytes;
    for (int i = 0; i < m->np; i++)
        if (m->p[i].end > m->hi)
            m->hi = m->p[i].end;
    s->svd = m;
    return m;
}

/* the first register touching [a, a + n), or 0 */
static struct rf_reg *reg_at(struct svdmap *m, u32 a, int n)
{
    int lo = 0, hi = m->nreg;       /* the first whose end is past a */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        struct rf_reg *r = m->reg[mid];
        if ((u64)r->addr + (u64)r->bytes <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < m->nreg && m->reg[lo]->addr < (u64)a + (u64)n)
        return m->reg[lo];
    return 0;
}

struct rf_reg *rf_reg_at(struct svdmap *m, u32 addr)
{
    struct rf_reg *r = reg_at(m, addr, 1);
    return r && r->addr == addr ? r : 0;
}

static int reg_index(struct svdmap *m, const struct rf_reg *r)
{
    int lo = 0, hi = m->nreg - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (m->reg[mid]->addr < r->addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

struct rf_periph *rf_periph(struct svdmap *m, const char *name)
{
    for (int i = 0; i < m->np; i++)
        if (!strcmp(m->p[i].name, name))
            return &m->p[i];
    return 0;
}

struct rf_periph *rf_periph_at(struct svdmap *m, int i)
{
    return i >= 0 && i < m->np ? &m->p[i] : 0;
}

struct rf_reg *rf_reg(struct rf_periph *p, const char *name)
{
    for (int i = 0; p && i < p->nreg; i++)
        if (!strcmp(p->reg[i]->name, name))
            return p->reg[i];
    return 0;
}

u64 rf_mask(const struct rf_reg *r, const char *field)
{
    for (int i = 0; r && i < r->nf; i++)
        if (r->f[i].name && !strcmp(r->f[i].name, field))
            return field_mask(&r->f[i]);
    return 0;
}

void rf_set_mwv(struct rf_reg *r, u64 mask, int mwv)
{
    for (int i = 0; i < r->nf; i++)
        if (field_mask(&r->f[i]) & mask)
            r->f[i].mwv = mwv;
}

void rf_set_reset(struct rf_reg *r, u64 v)
{
    r->reset = r->value = v;
}

void rf_set_acc(struct rf_reg *r, u64 mask, int acc)
{
    for (int i = 0; i < r->nf; i++)
        if (field_mask(&r->f[i]) & mask)
            r->f[i].acc = acc;
}

u64 rf_fields(const struct rf_reg *r)
{
    u64 m = 0;
    for (int i = 0; i < r->nf; i++)
        m |= field_mask(&r->f[i]);
    return m;
}

int rf_irq(const struct rf_periph *p, const char *want)
{
    const struct svd_periph *sp = p->svd;
    for (int i = 0; want && i < sp->nirq; i++)
        if (strstr(sp->irq[i].name, want))
            return sp->irq[i].value;
    return sp->nirq ? sp->irq[0].value : -1;
}

int rf_dev_irq(struct svdmap *m, const char *name)
{
    for (int i = 0; i < m->d->nirq; i++)
        if (!strcmp(m->d->irq[i].name, name))
            return m->d->irq[i].value;
    return -1;
}

int rf_irq_vector(struct svdmap *m, int irq)
{
    return irq >= 0 && irq < m->d->nirq_vector ? m->d->irq_vector[irq] : irq;
}

const char *rf_device_name(struct svdmap *m)
{
    return m->d->name;
}

u32 rf_config_addr(struct svdmap *m, const char *name, u32 dflt)
{
    for (int i = 0; i < m->d->ncfg; i++)
        if (!strcmp(m->d->cfg[i].name, name))
            return (u32)m->d->cfg[i].addr;
    return dflt;
}

int rf_y_space(struct svdmap *m, u32 *lo, u32 *hi)
{
    if (!m->d->y_end)
        return 0;
    *lo = (u32)m->d->y_base;
    *hi = (u32)m->d->y_end;
    return 1;
}

int rf_pin(struct svdmap *m, const char *name)
{
    for (int i = 0; i < m->d->npin; i++)
        if (!strcmp(m->d->pin[i].name, name))
            return m->d->pin[i].port * 32 + m->d->pin[i].bit;
    return -1;
}

void rf_interrupt(struct rf_periph *p, int irq)
{
    if (irq >= 0)
        p->sim->cpu->ops->interrupt(p->sim->cpu, 16 + irq);
}

/* ---- the trace ------------------------------------------------------------ */

static void tr_head(struct sim *s, char kind)
{
    fflush(stdout);
    fprintf(stderr, "periph %08x %c ", s->cpu->ops->pc(s->cpu), kind);
}

/* a field's value, as the trace writes it */
static void tr_val(const struct rf_field *f, u64 v)
{
    if (f->width == 1)
        fprintf(stderr, "%u", (unsigned)v);
    else
        fprintf(stderr, "0x%llx", (unsigned long long)v);
}

/* the name of the enumerated value v of field f, or 0 */
static const char *enum_name(const struct rf_field *f, u64 v)
{
    if (!f->svd)
        return 0;
    const char *def = 0;
    for (int i = 0; i < f->svd->nev; i++) {
        const struct svd_enumval *e = &f->svd->ev[i];
        if (e->isdef)
            def = e->name;
        else if ((v & e->care) == (e->value & e->care))
            return e->name;
    }
    return def;
}

/* "P.R.F old->new (OLD->NEW)", or "P.R old->new" without fields */
static void tr_change(const struct rf_reg *r, const struct rf_field *f, u64 old,
                      u64 nv)
{
    if (!f->name) {
        fprintf(stderr, "%s.%s 0x%0*llx->0x%0*llx", r->p->name, r->name,
                2 * r->bytes, (unsigned long long)old, 2 * r->bytes,
                (unsigned long long)nv);
        return;
    }
    u64 a = (old & field_mask(f)) >> f->lsb, b = (nv & field_mask(f)) >> f->lsb;
    fprintf(stderr, "%s.%s.%s ", r->p->name, r->name, f->name);
    tr_val(f, a);
    fputs("->", stderr);
    tr_val(f, b);
    const char *ea = enum_name(f, a), *eb = enum_name(f, b);
    if (ea && eb)
        fprintf(stderr, " (%s->%s)", ea, eb);
    else if (eb)
        fprintf(stderr, " (%s)", eb);
}

/* each field that changed, one line each */
static int tr_changes(struct sim *s, char kind, const struct rf_reg *r, u64 old,
                      u64 nv, const char *why)
{
    int k = 0;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        if (!((old ^ nv) & field_mask(f)))
            continue;
        tr_head(s, kind);
        tr_change(r, f, old, nv);
        fprintf(stderr, "%s\n", why);
        k++;
    }
    return k;
}

static void tr_write(struct sim *s, const struct rf_reg *r, u64 old, u64 nv,
                     u64 wm, u64 wv)
{
    int k = 0, ro = -1;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        u64 fm = field_mask(f);
        if (!(fm & wm))
            continue;
        if (ro)
            ro = f->acc == ACC_RO;
        if (f->acc == ACC_WO || f->acc == ACC_W1) {
            /* nothing to compare with: what was written, if not 0 */
            if (!(wv & fm & wm))
                continue;
            tr_head(s, 'W');
            if (f->name)
                fprintf(stderr, "%s.%s.%s=", r->p->name, r->name, f->name);
            else
                fprintf(stderr, "%s.%s=", r->p->name, r->name);
            tr_val(f, (wv & fm) >> f->lsb);
            const char *e = f->name ? enum_name(f, (wv & fm) >> f->lsb) : 0;
            fprintf(stderr, e ? " (%s)\n" : "\n", e);
            k++;
        } else if ((old ^ nv) & fm) {
            tr_head(s, 'W');
            tr_change(r, f, old, nv);
            fputc('\n', stderr);
            k++;
        }
    }
    if (!k) {
        tr_head(s, 'W');
        fprintf(stderr, "%s.%s 0x%0*llx %s\n", r->p->name, r->name,
                2 * r->bytes, (unsigned long long)(wv & wm),
                ro == 1 ? "ignored: read-only" : "(no change)");
    }
}

static void tr_read(struct sim *s, const struct rf_reg *r, u64 v, int at, int n)
{
    tr_head(s, 'R');
    fprintf(stderr, "%s.%s 0x%0*llx", r->p->name, r->name, 2 * r->bytes,
            (unsigned long long)v);
    if (at || n != r->bytes)
        fprintf(stderr, " (%d byte%s at +%d)", n, n > 1 ? "s" : "", at);
    fputc('\n', stderr);
}

void rf_hw(struct rf_reg *r, u64 v)
{
    u64 old = r->value;
    r->value = v;
    if (r->traced && old != v && !r->p->sim->bus.debug)
        tr_changes(r->p->sim, 'H', r, old, v, "");
}

/* ---- the register file's accesses ------------------------------------- */

/* a write of the bits wm (wv) of r, as its fields take it */
static u64 write_value(struct rf_reg *r, u64 old, u64 wm, u64 wv)
{
    u64 nv = old;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        u64 fm = field_mask(f), m = fm & wm;
        if (!m || f->acc == ACC_RO)
            continue;
        if (f->acc == ACC_W1 || f->acc == ACC_RW1) {
            if (r->once & fm)
                continue;
            r->once |= fm;
        }
        switch (f->mwv) {
        case MWV_MODIFY: nv = (nv & ~m) | (wv & m); break;
        case MWV_1CLR: nv &= ~(wv & m); break;
        case MWV_1SET: nv |= wv & m; break;
        case MWV_1TOG: nv ^= wv & m; break;
        case MWV_0CLR: nv &= ~(~wv & m); break;
        case MWV_0SET: nv |= ~wv & m; break;
        case MWV_0TOG: nv ^= ~wv & m; break;
        case MWV_CLEAR: nv &= ~m; break;
        case MWV_SET: nv |= m; break;
        }
    }
    return nv;
}

/* what a read of the bits rm of r gives */
static u64 read_value(const struct rf_reg *r, u64 rm)
{
    u64 v = r->value;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        if (f->acc == ACC_WO || f->acc == ACC_W1)
            v &= ~field_mask(f);
    }
    return v & rm;
}

/* the readAction of the fields a read of the bits rm touched */
static void read_action(struct rf_reg *r, u64 rm)
{
    u64 nv = r->value;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        u64 fm = field_mask(f);
        if (!(fm & rm))
            continue;
        if (f->ract == RA_CLEAR)
            nv &= ~fm;
        else if (f->ract == RA_SET)
            nv |= fm;
    }
    if (nv != r->value) {
        u64 old = r->value;
        r->value = nv;
        if (r->traced)
            tr_changes(r->p->sim, 'H', r, old, nv, " (by the read)");
    }
}

/* the registers [a, a + n) touches, read or written: 0, or -1 when it
 * touches none */
static int access(struct svdmap *m, u32 a, int n, u32 *v, int write, int side)
{
    struct rf_reg *r = reg_at(m, a, n);
    if (!r)
        return -1;
    u32 out = 0;
    for (int i = reg_index(m, r); i < m->nreg; i++) {
        r = m->reg[i];
        if ((u64)r->addr >= (u64)a + (u64)n)
            break;
        if (r->base)
            continue;               /* CLR, SET, INV: map_write's */
        u32 lo = a > r->addr ? a : r->addr;
        u64 e1 = (u64)a + (u64)n, e2 = (u64)r->addr + (u64)r->bytes;
        int cnt = (int)((e1 < e2 ? e1 : e2) - lo);
        int at = (int)(lo - r->addr);   /* in the register */
        int sh = (int)(lo - a);         /* in the access */
        u64 rm = lanes(at, cnt);
        if (write) {
            u64 wv = ((u64)(*v >> (8 * sh))) << (8 * at);
            u64 old = r->value;
            r->value = write_value(r, old, rm, wv);
            if (side && r->traced)
                tr_write(m->s, r, old, r->value, rm, wv & rm);
        } else {
            u64 x = read_value(r, rm) >> (8 * at);
            if (side && r->traced)
                tr_read(m->s, r, x << (8 * at), at, cnt);
            if (side)
                read_action(r, rm);
            out |= (u32)(x << (8 * sh));
        }
    }
    if (!write)
        *v = out;
    return 0;
}

u32 rf_read(struct rf_periph *p, u32 off, int n)
{
    struct svdmap *m = p->sim->svd;
    u32 v = 0;
    if (access(m, p->base + off, n, &v, 0, !m->s->bus.debug))
        bus_fault(&m->s->bus);
    return v;
}

void rf_write(struct rf_periph *p, u32 off, int n, u32 v)
{
    struct svdmap *m = p->sim->svd;
    if (access(m, p->base + off, n, &v, 1, 1))
        bus_fault(&m->s->bus);
}

/* ---- the device on the bus ------------------------------------------------ */

static const char *access_kind(int n, int write)
{
    static const char *const k[2][5] = {
        { "", "a byte read", "a halfword read", "", "a word read" },
        { "", "a byte write", "a halfword write", "", "a word write" },
    };
    return n >= 1 && n <= 4 ? k[write != 0][n] : "an access";
}

/* An access no register answers: a bus fault, and why on stderr -- but
 * on a PIC32, whose SFR space reads as 0 and ignores writes where nothing
 * is (its startups write whole tables over the holes: XC32's OFFn table
 * covers the unimplemented vectors' addresses), as the part does. */
static void hole(struct svdmap *m, u32 a, int n, int write)
{
    struct sim *s = m->s;
    if (m->d->holes_zero)
        return;
    bus_fault(&s->bus);
    if (s->bus.debug)
        return;
    if (++m->faults > 10) {
        if (m->faults == 11)
            fprintf(stderr, "embsim: (further bus faults in the peripherals "
                    "are not reported)\n");
        return;
    }
    const struct rf_periph *in = 0;
    for (int i = 0; i < m->np; i++)
        if (a - m->p[i].base < m->p[i].end - m->p[i].base &&
            (!in || m->p[i].end - m->p[i].base < in->end - in->base))
            in = &m->p[i];
    fflush(stdout);
    fprintf(stderr, "embsim: bus fault: %s at 0x%08x (pc 0x%08x): ",
            access_kind(n, write), a, s->cpu->ops->pc(s->cpu));
    if (in)
        fprintf(stderr, "%s has no register at +0x%03x\n", in->name, a - in->base);
    else
        fprintf(stderr, "no peripheral of %s is there\n", m->d->name);
}

int rf_gated(const struct rf_periph *p)
{
    return p->gate && (!(p->gate->value & p->gate_mask)) != p->gate_held;
}

void rf_periph_reset(struct rf_periph *p)
{
    for (int i = 0; i < p->nreg; i++) {
        p->reg[i]->value = p->reg[i]->reset;
        p->reg[i]->once = 0;
    }
    if (p->ops->reset)
        p->ops->reset(p->ctx);
}

/* An access to a peripheral whose clock is off, or that is held in
 * reset: the trace says so, and the first one is a warning. A read gives
 * 0 with the clock off, the registers' reset values in reset (`v`). */
static void gated_access(struct svdmap *m, struct rf_periph *p, u32 a, int n,
                         int write, u32 v)
{
    struct sim *s = m->s;
    if (s->bus.debug)
        return;
    struct rf_reg *r = reg_at(m, a, n);
    const char *why = p->gate_held ? "it is held in reset" : "its clock is off";
    if (r->traced) {
        tr_head(s, write ? 'W' : 'R');
        if (write)
            fprintf(stderr, "%s.%s 0x%08x ignored: %s (%s)\n", p->name,
                    r->name, v, why, p->gate_name);
        else
            fprintf(stderr, "%s.%s 0x%08x: %s (%s)\n", p->name, r->name, v,
                    why, p->gate_name);
    }
    if (!p->gate_warned) {
        p->gate_warned = 1;
        fflush(stdout);
        fprintf(stderr, "embsim: warning: %s at pc 0x%08x: %s.%s %s while "
                "%s (%s is %d)\n", write ? "a write" : "a read",
                s->cpu->ops->pc(s->cpu), p->name, r->name,
                write ? "is ignored" : p->gate_held ? "reads its reset value"
                                                    : "reads as 0",
                why, p->gate_name, p->gate_held);
    }
}

/* RF_ATOMIC_ALIASES: the register an address at +0x1000, +0x2000 or
 * +0x3000 from it names, and which alias (1 XOR, 2 set, 3 clear); 0 for
 * an address that is not an alias -- one of the file's own registers, or
 * the SIO's space (0xD0000000 up), which has none */
static struct rf_reg *atomic_alias(struct svdmap *m, u32 a, int n, int *kind)
{
    if (!(m->rules & RF_ATOMIC_ALIASES) || !(a & 0x3000u) || a >= 0xd0000000u ||
        reg_at(m, a, n))
        return 0;
    struct rf_reg *r = reg_at(m, a & ~0x3000u, n);
    if (!r || r->base || (r->p->base & ~0x3fffu) != (a & ~0x3fffu))
        return 0;
    *kind = (int)(a >> 12 & 3);
    return r;
}

/* A write of `v` through an atomic alias of r: the bits set in v are
 * XORed, set or cleared, and the others are not written at all -- the
 * model is given a whole word in which each of them is what a write
 * leaves alone: a plain field's value as it is, 0 for oneToClear and its
 * like, 1 for zeroToClear and its. (The datasheet's bus interposer in
 * front of the UARTs, SPI, I2C and SSI reads the register and writes the
 * result whole, which differs only for a register a read changes, a
 * FIFO's: EmbSim does not tell them apart.) */
static void alias_write(struct svdmap *m, struct rf_reg *r, int kind, u32 v)
{
    struct rf_periph *p = r->p;
    u32 off = r->addr - p->base;
    u64 old = r->value;
    u64 hit = v, w = 0;
    for (int i = 0; i < r->nf; i++) {
        const struct rf_field *f = &r->f[i];
        u64 fm = field_mask(f), keep;
        if (f->mwv == MWV_MODIFY)
            keep = f->acc == ACC_WO || f->acc == ACC_W1 ? 0 : old & fm;
        else
            keep = f->mwv >= MWV_0CLR && f->mwv <= MWV_0TOG ? fm : 0;
        u64 to = kind == 1 ? ~old : kind == 2 ? ~0ULL : 0;
        w |= (keep & ~hit) | (to & hit & fm);
    }
    (void)m;
    if (p->ops->write)
        p->ops->write(p->ctx, off, 4, (u32)w);
}

static int lags(const struct rf_periph *p)
{
    return p->next_effect != 0;
}

/* the running models that may lag ticked up to cycle `to`: none of them acts
 * before its due cycle, so the order they tick in is not seen */
static void sync_to(struct svdmap *m, u64 to)
{
    int was = m->stepping;
    m->stepping = STEP_CATCHING_UP;
    while (m->synced_at < to) {
        u64 left = to - m->synced_at;
        u32 c = left > 0xffffffffu ? 0xffffffffu : (u32)left;
        struct rf_periph *late[MAX_TICKERS];
        int n = 0;
        m->synced_at += c;
        for (int i = 0; i < m->ntk; i++)
            if (lags(m->tk[i]) && m->tk[i]->ops->tick)
                late[n++] = m->tk[i];
        for (int i = 0; i < n; i++)
            late[i]->ops->tick(late[i]->ctx, c);
    }
    m->stepping = was;
}

/* the models that may lag, current before the program or a question sees them */
static void catch_up(struct svdmap *m)
{
    sync_to(m, m->s->cycles);
}

static void find_due(struct svdmap *m)
{
    u64 due = NO_EFFECT;
    for (int i = 0; i < m->ntk; i++) {
        u32 c;
        if (lags(m->tk[i]) && m->tk[i]->next_effect(m->tk[i]->ctx, &c) && m->synced_at + c < due)
            due = m->synced_at + c;
    }
    m->due_at = due;
    m->due_known = 1;
}

void rf_lag(struct rf_periph *p, int (*next_effect)(void *ctx, u32 *cycles))
{
    p->next_effect = next_effect;
}

static u32 map_read(void *ctx, u32 off, int n)
{
    struct svdmap *m = ctx;
    u32 a = m->lo + off, v = 0;
    int kind;
    catch_up(m);
    if (atomic_alias(m, a, n, &kind))
        a &= ~0x3000u;              /* a read there is the register's */
    struct rf_reg *r = reg_at(m, a, n);
    if (!r) {
        hole(m, a, n, 0);
        return 0;
    }
    struct rf_periph *p = r->p;
    if (lags(p))
        m->due_known = 0;
    if (rf_gated(p)) {
        if (p->gate_held)
            access(m, a, n, &v, 0, 0);
        gated_access(m, p, a, n, 0, v);
        return v;
    }
    if (r->base)
        return 0;                   /* CLR, SET, INV read as 0 */
    if (m->s->bus.debug) {
        access(m, a, n, &v, 0, 0);
        return v;
    }
    return p->ops->read ? p->ops->read(p->ctx, a - p->base, n) : 0;
}

/* a write to a CLR, SET or INV register: its register written with the
 * bits cleared, set or inverted, as one word */
static void sci_write(struct svdmap *m, struct rf_reg *r, u32 a, int n, u32 v)
{
    struct rf_reg *b = r->base;
    struct rf_periph *p = b->p;
    u32 sh = 8 * (a - r->addr);
    u32 bits = (n == 4 ? v : v & ((1u << (8 * n)) - 1)) << sh;
    u32 old = (u32)b->value, nv;
    switch (r->sci) {
    case 1: nv = old & ~bits; break;
    case 2: nv = old | bits; break;
    default: nv = old ^ bits; break;
    }
    (void)m;
    if (p->ops->write)
        p->ops->write(p->ctx, b->addr - p->base, 4, nv);
}

static void map_write(void *ctx, u32 off, int n, u32 v)
{
    struct svdmap *m = ctx;
    u32 a = m->lo + off;
    int kind = 0;
    catch_up(m);
    m->due_known = 0;
    struct rf_reg *r = atomic_alias(m, a, n, &kind);
    if (r)
        a &= ~0x3000u;              /* the register the alias names */
    else
        r = reg_at(m, a, n);
    if (r && n < 4 && r->bytes == 4 && (m->rules & RF_WIDE_WRITES)) {
        /* the byte or halfword across the bus's four lanes, to the
         * whole register */
        v &= n == 1 ? 0xffu : 0xffffu;
        v = n == 1 ? v * 0x01010101u : v * 0x00010001u;
        a &= ~3u;
        n = 4;
    }
    if (!r) {
        hole(m, a, n, 1);
        return;
    }
    struct rf_periph *p = r->p;
    if (rf_gated(p)) {
        gated_access(m, p, a, n, 1, v);
        return;
    }
    if (kind) {
        alias_write(m, r, kind, v);
        return;
    }
    if (r->base) {
        sci_write(m, r, a, n, v);
        return;
    }
    if (p->ops->write)
        p->ops->write(p->ctx, a - p->base, n, v);
}

void rf_clock(struct rf_periph *p, int *running, int on)
{
    struct svdmap *m = p->sim->svd;
    on = on != 0;
    if (*running == on)
        return;
    if (lags(p)) {
        if (m->stepping == STEP_WAITING || m->stepping == STEP_CATCHING_UP)
            die("%s: not modelled: a model that may lag started or stopped while the models that "
                "may lag wait or catch up", p->name);
        catch_up(m);
        m->due_known = 0;
    }
    sim_clock(p->sim, running, on);
    if (on) {
        if (m->ntk == MAX_TICKERS)
            die("too many peripheral clocks running");
        m->tk[m->ntk++] = p;
        return;
    }
    for (int i = 0; i < m->ntk; i++)
        if (m->tk[i] == p) {
            m->tk[i] = m->tk[--m->ntk];
            break;
        }
}

static void map_reset(void *ctx)
{
    struct svdmap *m = ctx;
    m->ntk = 0;                     /* the models' resets stop their clocks */
    m->synced_at = m->s->cycles;
    m->due_known = 0;
    for (int i = 0; i < m->nreg; i++) {
        m->reg[i]->value = m->reg[i]->reset;
        m->reg[i]->once = 0;
    }
    for (int i = 0; i < m->np; i++) {
        m->p[i].gate_warned = 0;
        if (m->p[i].ops->reset)
            m->p[i].ops->reset(m->p[i].ctx);
    }
    m->faults = 0;
}

/* the step's `cycles` have passed (s->cycles is past them): the models that may
 * lag wait while their due cycle is still to come; at it they are brought to
 * the step's start, and the step ticks every model in its order */
static void map_tick(void *ctx, u32 cycles)
{
    struct svdmap *m = ctx;
    u64 now = m->s->cycles;
    if (!m->due_known)
        find_due(m);
    if (now < m->due_at) {
        m->stepping = STEP_WAITING;
        for (int i = 0; i < m->ntk; i++)
            if (!lags(m->tk[i]) && m->tk[i]->ops->tick)
                m->tk[i]->ops->tick(m->tk[i]->ctx, cycles);
        m->stepping = 0;
        return;
    }
    sync_to(m, now - cycles);
    m->synced_at = now;
    m->stepping = STEP_ALL;
    for (int i = 0; i < m->ntk; i++)
        if (m->tk[i]->ops->tick)
            m->tk[i]->ops->tick(m->tk[i]->ctx, cycles);
    m->stepping = 0;
    m->due_known = 0;
}

static int map_next_event(void *ctx, u32 *cycles)
{
    struct svdmap *m = ctx;
    int any = 0;
    catch_up(m);
    for (int i = 0; i < m->ntk; i++) {
        u32 c;
        if (m->tk[i]->ops->next_event && m->tk[i]->ops->next_event(m->tk[i]->ctx, &c) &&
            (!any || c < *cycles)) {
            *cycles = c;
            any = 1;
        }
    }
    return any;
}

static const struct dev_ops svdmap_ops = {
    "svd", map_read, map_write, map_reset, map_tick, map_next_event,
};

void svdmap_add(struct sim *s, struct svdmap *m)
{
    sim_add_dev(s, m->lo, m->hi - m->lo, &svdmap_ops, m);
}

/* a peripheral with no model: the register file alone */
static u32 plain_read(void *ctx, u32 off, int n)
{
    return rf_read(ctx, off, n);
}

static void plain_write(void *ctx, u32 off, int n, u32 v)
{
    rf_write(ctx, off, n, v);
}

static const struct dev_ops rf_plain_ops = {
    "svd-registers", plain_read, plain_write, 0, 0, 0,
};

void svdmap_models(struct sim *s, struct svdmap *m,
                   const struct model_desc *models)
{
    for (int k = 0; models && models[k].periph; k++) {
        const struct model_type *t = model_find(models[k].model);
        if (!t)
            die("unknown peripheral model '%s'", models[k].model);
        for (int i = 0; i < m->np; i++) {
            struct rf_periph *p = &m->p[i];
            if (p->ops != &rf_plain_ops ||
                !glob(models[k].periph, p->name, strlen(p->name)))
                continue;
            void *ctx = t->create(s, p);
            if (!ctx)
                continue;
            p->ops = t->ops;
            p->ctx = ctx;
        }
    }
}

void svdmap_trace(struct svdmap *m, const char *which)
{
    for (int i = 0; i < m->nreg; i++) {
        struct rf_reg *r = m->reg[i];
        const char *pn = r->p->name;
        if (!which) {
            r->traced = 1;
            continue;
        }
        size_t pl = strlen(pn), rl = strlen(r->name);
        char *full = malloc(pl + rl + 2);
        if (!full)
            die("out of memory");
        memcpy(full, pn, pl);
        full[pl] = '.';
        memcpy(full + pl + 1, r->name, rl + 1);
        for (const char *q = which; *q; ) {
            size_t l = strcspn(q, ",");
            char pat[128];
            if (l >= sizeof pat)
                die("--trace-periph: '%.*s' is too long", (int)l, q);
            memcpy(pat, q, l);
            pat[l] = 0;
            if (strchr(pat, '.') ? glob(pat, full, strlen(full))
                                 : glob(pat, pn, pl))
                r->traced = 1;
            q += l;
            if (*q == ',')
                q++;
        }
        free(full);
    }
}

void svdmap_print(struct svdmap *m, FILE *f)
{
    const struct svd_device *d = m->d;
    fprintf(f, "# %s: %d peripherals, %d registers on the bus at "
            "0x%08x-0x%08x\n", d->name, m->np, m->nreg, m->lo, m->hi - 1);
    if (d->bus_offset)
        fprintf(f, "# (the file's addresses: each is on the bus 0x%llx above)\n",
                d->bus_offset);
    for (int i = 0; i < d->np; i++) {
        const struct svd_periph *sp = &d->p[i];
        struct svd_flats fl = svd_flat_regs(sp, 1);
        for (int k = 0; k < fl.n; k++) {
            const struct svd_node *n = fl.v[k].r;
            u32 a = (u32)(sp->base + fl.v[k].off + d->bus_offset);
            fprintf(f, "%s 0x%08x %d %s %s.%s 0x%0*llx %s", sp->name,
                    a - (u32)d->bus_offset,
                    n->size, fl.v[k].name, sp->name, fl.v[k].path,
                    n->size / 4, (unsigned long long)n->reset, n->acc);
            struct rf_reg *r = sp->base >= 0xE0000000u ? 0 : reg_at(m, a, 1);
            if (sp->base >= 0xE0000000u)
                fputs(" -- the core's", f);
            else if (!r || r->addr != a || strcmp(r->p->name, sp->name) ||
                     strcmp(r->name, fl.v[k].path))
                fprintf(f, " -- is %s.%s", r ? r->p->name : "?",
                        r ? r->name : "?");
            fputc('\n', f);
            for (int q = 0; r && r->addr == a && q < 3 && r->alias[q]; q++)
                fprintf(f, "%s 0x%08x 32 %s %s.%s 0x00000000 write-only -- "
                        "%s of %s\n", sp->name, r->alias[q]->addr,
                        r->alias[q]->name, sp->name, r->alias[q]->name,
                        q == 0 ? "CLR" : q == 1 ? "SET" : "INV", r->name);
        }
        svd_free_flats(&fl);
    }
}

const char *svd_dfp;

static int readable(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != 0;
}

const char *svd_search(const char *name)
{
    const char *path = getenv("EMBSIM_SVD_PATH");
    static char buf[1024];
    if (svd_dfp) {
        snprintf(buf, sizeof buf, "%s/atdf/%s", svd_dfp, name);
        if (readable(buf))
            return buf;
        snprintf(buf, sizeof buf, "%s/edc/%s", svd_dfp, name);
        if (readable(buf))
            return buf;
        snprintf(buf, sizeof buf, "%s/%s", svd_dfp, name);
        if (readable(buf))
            return buf;
    }
    for (const char *q = path; q && *q; ) {
        size_t l = strcspn(q, ":");
        if (l && l + strlen(name) + 2 < sizeof buf) {
            memcpy(buf, q, l);
            buf[l] = '/';
            strcpy(buf + l + 1, name);
            FILE *f = fopen(buf, "rb");
            if (f) {
                fclose(f);
                return buf;
            }
        }
        q += l;
        if (*q == ':')
            q++;
    }
    return 0;
}
