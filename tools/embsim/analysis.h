/* analysis.h -- what EmbSim measures of a run beyond its counts: the
 * code it covered (coverage.c), where its time went (profile.c), how
 * deep its stacks went (stack.c), the faults it took (fault.c), and the
 * record of its inputs that replays it (record.c).
 *
 * All of them watch the run one step at a time through an_step, which
 * the run loop calls in place of the core's step when any is on, so a
 * run with none of them is the loop it always was. an_step notes the pc
 * and the counts before the step and, after it, gives each analysis the
 * instruction that ran and what it cost; and it follows the program's
 * calls and returns, as the core executes them, on a shadow stack of
 * frames (the profile's call paths, a function's frame size, the
 * backtrace when there is no CFI). The cores tell it of exception entry
 * and return (an_exc_entry, an_exc_return), which no instruction shows. */
#ifndef EMBSIM_ANALYSIS_H
#define EMBSIM_ANALYSIS_H

#include "image.h"
#include "sim.h"

enum { AN_ARM, AN_RV, AN_AVR, AN_MIPS, AN_PIC24 };

/* a frame of the shadow stack: a call or an exception's entry */
struct an_frame {
    u32 ret;                        /* where it returns to */
    u32 sp;                         /* sp at its first instruction */
    int node;                       /* its call path (struct an_node) */
    int exc;                        /* an exception's (its number + 1) */
    u32 vec;                        /* an exception's: the vector's jump */
    u32 min;                        /* the lowest sp while it was on the stack */
};

/* a call path: a function, called along its parent's path. The counts
 * are the instructions and cycles run in the function itself along this
 * path; each step's go to exactly one path. */
struct an_node {
    int parent, fn;                 /* fn: m->fn's index, or -1: no symbol */
    int child, sib;                 /* the paths it calls, as a list */
    u64 insns, cycles, calls;
};

/* what a step did besides running its instruction */
enum { EV_ENTRY = 1, EV_RETURN };

struct analysis {
    struct sim *s;
    struct image *img;
    int arch;
    u32 sp0;                        /* PIC24: the stack's base (__SP_init), or 0 */

    /* ---- the step ---- */
    u32 pc0;                        /* the instruction's address */
    int kind, size;                 /* its kind (a call, a return), its bytes */
    int slot_kind;                  /* MIPS: the branch before this delay
                                     * slot was a call or a return */
    struct { int what; u32 n, ret; } ev[4];   /* the step's exceptions */
    int nev;

    /* ---- the shadow stack (calls on) ---- */
    int calls;
    struct an_frame *fr;
    int nfr, capfr;
    struct an_node *node;
    int nnode, capnode;
    int cur_base, cur_fn, cur_node;   /* the path the pc is on, cached */
    u32 fn_lo, fn_hi;               /* the cached function's range */
    int fn_idx;

    /* ---- coverage (coverage.c) ---- */
    const char *cov_path;
    int cov_lcov;
    u64 *cov[IMG_CODE];             /* a count per halfword of code */

    /* ---- the profile (profile.c) ---- */
    int prof;
    const char *prof_path;          /* 0: stderr */
    int prof_fmt;                   /* 0 the report; collapsed by 1 cycles, 2 insns */

    /* ---- the stacks (stack.c) ---- */
    int stk;                        /* measured: --stack-report or --stack-limit */
    int stk_report;                 /* the report wanted */
    const char *stk_path;           /* 0: stderr */
    const char *stk_limit_arg;      /* --stack-limit's ADDR or SYMBOL */
    u32 stk_limit;                  /* --stack-limit or the link's */
    const char *stk_limit_why;      /* where it came from, or 0: none */
    u32 stk_check, stk_checked_top; /* the overflow's limit, for the top */
    const char *stk_check_why;
    int stk_check_data;             /* the limit is the end of .data/.bss */
    u32 msp_top, msp_min, psp_top, psp_min;
    int psp_used;
    u32 *fn_minsp, *fn_frame, *fn_incl;   /* by function, nfn + 1 */
    int tail_fn;                    /* a function entered by a jump, not a call */
    u32 tail_sp;                    /* and sp at its first instruction */
    int ovf, ovf_entry;             /* the overflow, when there was one */
    u32 ovf_pc, ovf_sp;
    const char **su_path;           /* --stack-su files */
    int nsu_path;
    const char *embrt_path;         /* --stack-embrt */
    struct su { char *name; long bytes; int dynamic; } *su;
    int nsu;
    struct rt { char *name; long bytes; } *rt;
    int nrt;

    /* ---- the fault report (fault.c) ---- */
    int fault;
    FILE *fault_out;                /* 0: stderr */
    int faults;                     /* reported */

    /* ---- record and replay (record.c) ---- */
    u64 steps;                      /* the steps that did something */
};

/* the machine a recording ran: the image and the options that shape a
 * run */
struct rec_machine {
    const char *image;
    u64 image_hash;
    const char *board, *cpu, *svd, *until;
    u32 ram_size;
    int semihosting, input;
    u64 max_insns;
    const char *dfp;                /* --dfp's directory, or 0 */
    const char **sets;              /* the board's inputs: --set NAME=VALUE */
    int nset;
    int i2c_bus;                    /* the run had an I2C bus endpoint */
    int reset_runs_on;              /* --reset-runs-on */
};

/* the analyses on for this run (from main.c's options), after sim_load;
 * 0 when none is on */
struct analysis *an_create(struct sim *s, int calls);
/* the run is over: each analysis writes its report (an_step and the
 * cores' an_exc_entry and an_exc_return are sim.h's) */
void an_finish(struct sim *s);

/* the function holding pc (an index into img->fn, or -1), cached */
int an_fn(struct analysis *a, u32 pc);
/* the stack pointer, as an address on the bus; 0 before the program has
 * one. A PIC24's stack grows up: its sp is mirrored (an_stack_real), so
 * deeper is lower on every core */
u32 an_sp(struct analysis *a);
/* a mirrored PIC24 stack address back to W15's value (a data address);
 * anything else as it is */
u32 an_stack_real(struct analysis *a, u32 v);
/* and a data address to the analyses' sp */
u32 an_stack_view(struct analysis *a, u32 real);
/* the main and the process stack pointers (a Cortex-M's MSP and PSP; sp
 * and 0 elsewhere), and whether the PSP is the one in use */
int an_sps(struct analysis *a, u32 *msp, u32 *psp);
/* a symbol of the data space, as an address on the bus (the AVR's data
 * space is at AVR_DATA there) */
int an_data_sym(struct analysis *a, const char *name, u32 *v);

/* coverage.c */
void cov_init(struct analysis *a);
void cov_report(struct analysis *a);

/* profile.c */
void prof_report(struct analysis *a);

/* stack.c */
void stk_init(struct analysis *a);
void stk_read_static(struct analysis *a);
/* after a step: whether an instruction ran, whether an exception was
 * entered, whether the step may count toward the function's own frame */
void stk_step(struct analysis *a, int ran, int entered, int sample_frame);
void stk_entered(struct analysis *a, u32 pc1, int called);
/* frame i of the shadow stack is over */
void stk_pop(struct analysis *a, int i);
void stk_report(struct analysis *a);

/* fault.c: exception n entered (an_exc_entry's), and the run's end */
void fault_entry(struct analysis *a, u32 n, u32 ret);
void fault_end(struct analysis *a);

/* record.c: a recorder (--input, --record) on s, and its input */
struct rec *rec_create(struct sim *s);
void rec_input(struct sim *s, const char *path);
void rec_record(struct sim *s, const char *path, const struct rec_machine *m);
/* --replay: a recording's machine and events, read before the machine
 * is built; then put on it */
struct rec *rec_load(const char *path, struct rec_machine *m);
void rec_attach(struct sim *s, struct rec *r, const struct rec_machine *m);
void rec_check_image(const char *path, const struct rec_machine *m);
/* the events before the first step; after each step; an exception's
 * entry; and the end */
void rec_start(struct sim *s);
void rec_input_fd(struct sim *s, int fd);
void rec_step(struct sim *s);
void rec_exc(struct sim *s, u32 n);
void rec_finish(struct sim *s);

#endif
