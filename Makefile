# EmbCC — host build (ROADMAP M0: EmbCC is a host program at this stage).
#
# Plain make on purpose: the eventual on-OS build goes through EmbBuild
# (ROADMAP M4), so nothing here may grow host-only cleverness the manifest
# could not express.
#
# -std=c99: self-hosting constrains the source to the subset EmbCC will
# implement (ARCHITECTURE.md §7). Keep it buildable by a strict C99 compiler.

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Werror -g
BUILD   := build

# What ./configure wrote, if it was run: TARGETS, DEFAULT_TARGET, PREFIX.
# A variable on the command line still wins.
-include config.mk

# ---- which targets: make TARGETS= ------------------------------------------
#
# A compiler for the targets you use, as GCC is configured for one target:
#
#     make TARGETS="arm riscv"         (or TARGETS=arm,riscv)
#     ./configure --targets=arm,riscv  (the same, remembered in config.mk)
#
# builds embcc with the ARM and RISC-V backends and no other -- the other
# families' sources are not compiled, let alone linked -- and the runtimes
# (`make runtimes`, `make install`) for their triples only. A family is one
# file of the target database, src/targets/<name>.def:
TARGET_FAMILIES := x86 aarch64 arm riscv avr mips loongarch tricore xtensa \
                   ppc rx sparc m68k
# and the default, `all`, is every one, as before. The backend directories'
# names are accepted for their family too (thumb, coldfire, x86_64, ...).
# The compiler refuses a target it was built without by name and says how
# to add it; `embcc --version` lists what it has. Built without x86, its
# default target is its first family's (src/targets/targets.def, FAMILY),
# unless DEFAULT_TARGET says otherwise.
#
# A subset's objects go to a directory of their own, $(BUILD)/targets-<names>,
# because the shared sources are compiled differently for it (each family's
# EMBCC_FAMILY_<name>=0, src/arch/target.h): switching between selections
# rebuilds nothing that is up to date and reuses nothing that is not.
TARGETS ?= all
TARGET_DEFS := $(filter-out targets,$(basename $(notdir $(wildcard src/targets/*.def))))
ifneq ($(sort $(TARGET_FAMILIES)),$(sort $(TARGET_DEFS)))
$(error TARGET_FAMILIES does not name the families src/targets/ holds)
endif
empty :=
space := $(empty) $(empty)
comma := ,
TARGET_ALIASES := thumb=arm armv7a=arm coldfire=m68k x86_64=x86 x86-64=x86 \
                  arm64=aarch64 powerpc=ppc ppc32=ppc riscv32=riscv \
                  riscv64=riscv mips32=mips mips64=mips loongarch64=loongarch \
                  sparc32=sparc
family_of = $(or $(patsubst $(1)=%,%,$(filter $(1)=%,$(TARGET_ALIASES))),$(1))
TARGETS_ASKED := $(foreach t,$(subst $(comma),$(space),$(TARGETS)),$(call family_of,$(t)))
ifeq ($(strip $(TARGETS_ASKED)),)
$(error TARGETS is empty: name the families to build ($(TARGET_FAMILIES)), or all)
endif
ifneq ($(filter all,$(TARGETS_ASKED)),)
TARGETS_BUILT := $(TARGET_FAMILIES)
else
ifneq ($(filter-out $(TARGET_FAMILIES),$(TARGETS_ASKED)),)
$(error TARGETS: no target family '$(filter-out $(TARGET_FAMILIES),$(TARGETS_ASKED))'; the families are $(TARGET_FAMILIES), or all)
endif
TARGETS_BUILT := $(filter $(TARGETS_ASKED),$(TARGET_FAMILIES))
endif
TARGETS_OMITTED := $(filter-out $(TARGETS_BUILT),$(TARGET_FAMILIES))
TARGET_CFLAGS := $(foreach f,$(TARGETS_OMITTED),-DEMBCC_FAMILY_$(f)=0)
ifeq ($(TARGETS_OMITTED),)
OBJDIR := $(BUILD)
else
OBJDIR := $(BUILD)/targets-$(subst $(space),-,$(TARGETS_BUILT))
endif
# Is family $(1) built? Non-empty when it is.
built = $(filter $(1),$(TARGETS_BUILT))

# The target `embcc` compiles for when the command line names none.
# Empty means x86_64-elf -- a compiler built here targets here. A person
# whose work is one board builds the compiler for it once:
#
#     make DEFAULT_TARGET=riscv32-unknown-elf
#
# and then types `embcc main.c` rather than repeating --target= all day.
# This is GCC's `./configure --target=`, which is why it is a build-time
# knob and not only an environment variable: an installed cross compiler
# should behave the same for everyone who runs it, including a Makefile
# that inherited no environment. EMBCC_DEFAULT_TARGET overrides it for
# one shell, and --target= overrides both.
#
# Only the string is compiled in. The compiler still contains every
# backend TARGETS names (all, by default), so --target= reaches each of
# them -- unlike GCC, where a cross build is a different binary.
DEFAULT_TARGET ?=
ifneq ($(DEFAULT_TARGET),)
CFLAGS += -DEMBCC_DEFAULT_TARGET='"$(DEFAULT_TARGET)"'
endif

# The HOST layer (src/platform): what the compiler asks the machine it runs
# on. Files and the environment are ISO C and every host shares them
# (platform_common.c); the console and the program's own path are the
# host's. `posix` is macOS, Linux and EmbLinkOS; `iso` needs nothing but a
# hosted C library, for a hobby or non-POSIX OS (docs/internals/porting.md).
PLATFORM ?= posix
# Only these two: `?=` takes a PLATFORM from the environment too, and a CI
# job's PLATFORM=linux-x86_64 (a package's name) asked for
# src/platform/platform_linux-x86_64.c, which make could only report as a
# missing file.
ifeq ($(filter posix iso,$(PLATFORM)),)
$(error PLATFORM=$(PLATFORM) is not a host layer: EmbCC builds with PLATFORM=posix (the default) or PLATFORM=iso; if it came from the environment, unset it or say make PLATFORM=posix)
endif
# The POSIX layer runs the compiler on a thread with a big stack
# (plat_run_on_stack): libpthread for a glibc older than 2.34, where the
# threads are not yet in libc; macOS and newer glibc take -lpthread too.
ifeq ($(PLATFORM),posix)
PLATFORM_LIBS ?= -lpthread
endif
# Whether the host can run a program (src/platform/platform.h): `spawn` is
# POSIX's posix_spawn, macOS and Linux; `none` is EmbLinkOS (no fork/exec)
# and any host with only a C library, and the default with PLATFORM=iso.
# Only the driver's several-sources-in-one-command uses it.
ifeq ($(PLATFORM),iso)
PROCESS ?= none
else
PROCESS ?= spawn
endif
PLATFORM_SRCS := src/platform/platform_common.c src/platform/platform_$(PLATFORM).c \
	src/platform/process_$(PROCESS).c

# The prefix an installed compiler looks under when the host cannot say
# where the compiler is (no /proc, no _NSGetExecutablePath, argv[0] not a
# path) and has no EMBCC_PREFIX in its environment -- as on a host with no
# environment at all.
DEFAULT_PREFIX ?=
ifneq ($(DEFAULT_PREFIX),)
CFLAGS += -DEMBCC_DEFAULT_PREFIX='"$(DEFAULT_PREFIX)"'
endif

# The target-neutral compiler, then src/arch: what every target shares
# (selection, the backend contract, the code buffer), then one directory per
# architecture — everything x86-64-only under x86_64/, aarch64-only under
# aarch64/ (src/arch/README.md; docs/manual/targets.md for what each supports).
# The optimizer: the pass manager (opt.c) and one file per pass.
OPT_SRCS := src/opt/opt.c \
	src/opt/coldsink.c \
	src/opt/wcet.c \
	src/opt/alias.c \
	src/opt/attrs.c \
	src/opt/cfg.c \
	src/opt/cfgclean.c \
	src/opt/copyprop.c \
	src/opt/dce.c \
	src/opt/divmagic.c \
	src/opt/dse.c \
	src/opt/fold.c \
	src/opt/gcse.c \
	src/opt/guardjump.c \
	src/opt/idiom.c \
	src/opt/ifconv.c \
	src/opt/immfold.c \
	src/opt/inline.c \
	src/opt/ipcp.c \
	src/opt/ivsr.c \
	src/opt/joincopies.c \
	src/opt/latch.c \
	src/opt/licm.c \
	src/opt/loadcse.c \
	src/opt/lvn.c \
	src/opt/mem2reg.c \
	src/opt/memfwd.c \
	src/opt/pre.c \
	src/opt/rangecheck.c \
	src/opt/reassoc.c \
	src/opt/rotate.c \
	src/opt/sccp.c \
	src/opt/sink.c \
	src/opt/splitloops.c \
	src/opt/sroa.c \
	src/opt/swthread.c \
	src/opt/tailmerge.c \
	src/opt/tailrec.c \
	src/opt/unroll.c \
	src/opt/unswitch.c \
	src/opt/util.c \
	src/opt/vectorize.c \
	src/opt/verify.c \
	src/opt/x86loadop.c

# Each target family's backend: the sources `make TARGETS=` builds for it
# (the families are src/targets/<name>.def, above). A file two families
# need is in both lists and compiled once: x86_64/topasm.c is the hosted
# targets' file-scope asm, AArch64's too, and riscv/irgen.c holds the
# variadic lowering LoongArch shares (the rest of it is RISC-V's alone).
FAMILY_SRCS_x86 := \
	src/arch/x86_64/irgen.c \
	src/arch/x86_64/codegen.c \
	src/arch/x86_64/emit.c \
	src/arch/x86_64/topasm.c \
	src/arch/x86_64/as.c \
	src/arch/x86_64/att.c \
	src/arch/x86_64/disasm.c \
	src/arch/x86_64/predef.c \
	src/arch/x86_64/predef_cxx.c
FAMILY_SRCS_aarch64 := \
	src/arch/aarch64/irgen.c \
	src/arch/aarch64/codegen.c \
	src/arch/aarch64/emit.c \
	src/arch/aarch64/asm.c \
	src/arch/aarch64/predef.c \
	src/arch/aarch64/predef_cxx.c \
	src/arch/x86_64/topasm.c
FAMILY_SRCS_arm := \
	src/arch/thumb/options.c \
	src/arch/thumb/emit.c \
	src/arch/thumb/a32.c \
	src/arch/thumb/attrs.c \
	src/arch/thumb/irgen.c \
	src/arch/thumb/codegen.c \
	src/arch/thumb/v6m.c \
	src/arch/thumb/asm.c \
	src/arch/thumb/predef.c \
	src/arch/thumb/predef_cxx.c \
	src/arch/thumbv8m/predef.c \
	src/arch/thumbv8m/predef_cxx.c \
	src/arch/thumbv6m/predef.c \
	src/arch/thumbv6m/predef_cxx.c \
	src/arch/thumbv8mbase/predef.c \
	src/arch/thumbv8mbase/predef_cxx.c \
	src/arch/armv7a/predef.c \
	src/arch/armv7a/predef_cxx.c
FAMILY_SRCS_riscv := \
	src/arch/riscv/options.c \
	src/arch/riscv/emit.c \
	src/arch/riscv/codegen.c \
	src/arch/riscv/attrs.c \
	src/arch/riscv/irgen.c \
	src/arch/riscv/asm.c \
	src/arch/riscv32/predef.c \
	src/arch/riscv32/predef_cxx.c \
	src/arch/riscv64/predef.c \
	src/arch/riscv64/predef_cxx.c
FAMILY_SRCS_avr := \
	src/arch/avr/options.c \
	src/arch/avr/emit.c \
	src/arch/avr/codegen.c \
	src/arch/avr/asm.c \
	src/arch/avr/irgen.c \
	src/arch/avr/predef.c \
	src/arch/avr/predef_cxx.c
FAMILY_SRCS_mips := \
	src/arch/mips/options.c \
	src/arch/mips/emit.c \
	src/arch/mips/codegen.c \
	src/arch/mips/irgen.c \
	src/arch/mips/asm.c \
	src/arch/mips32/predef.c \
	src/arch/mips32/predef_cxx.c \
	src/arch/mips32eb/predef.c \
	src/arch/mips32eb/predef_cxx.c \
	src/arch/mips64/predef.c \
	src/arch/mips64/predef_cxx.c \
	src/arch/mips64eb/predef.c \
	src/arch/mips64eb/predef_cxx.c
FAMILY_SRCS_loongarch := \
	src/arch/loongarch/options.c \
	src/arch/loongarch/emit.c \
	src/arch/loongarch/codegen.c \
	src/arch/loongarch/irgen.c \
	src/arch/loongarch/asm.c \
	src/arch/loongarch64/predef.c \
	src/arch/loongarch64/predef_cxx.c \
	src/arch/riscv/irgen.c
FAMILY_SRCS_tricore := \
	src/arch/tricore/options.c \
	src/arch/tricore/emit.c \
	src/arch/tricore/codegen.c \
	src/arch/tricore/irgen.c \
	src/arch/tricore/asm.c \
	src/arch/tricore/predef.c \
	src/arch/tricore/predef_cxx.c
FAMILY_SRCS_xtensa := \
	src/arch/xtensa/options.c \
	src/arch/xtensa/emit.c \
	src/arch/xtensa/codegen.c \
	src/arch/xtensa/irgen.c \
	src/arch/xtensa/asm.c \
	src/arch/xtensa/predef.c \
	src/arch/xtensa/predef_cxx.c
FAMILY_SRCS_ppc := \
	src/arch/ppc/options.c \
	src/arch/ppc/emit.c \
	src/arch/ppc/codegen.c \
	src/arch/ppc/irgen.c \
	src/arch/ppc/asm.c \
	src/arch/ppc32/predef.c \
	src/arch/ppc32/predef_cxx.c
FAMILY_SRCS_rx := \
	src/arch/rx/options.c \
	src/arch/rx/emit.c \
	src/arch/rx/codegen.c \
	src/arch/rx/irgen.c \
	src/arch/rx/asm.c \
	src/arch/rx/predef.c
FAMILY_SRCS_sparc := \
	src/arch/sparc/options.c \
	src/arch/sparc/emit.c \
	src/arch/sparc/codegen.c \
	src/arch/sparc/irgen.c \
	src/arch/sparc/asm.c \
	src/arch/sparc32/predef.c \
	src/arch/sparc32/predef_cxx.c
FAMILY_SRCS_m68k := \
	src/arch/coldfire/options.c \
	src/arch/coldfire/emit.c \
	src/arch/coldfire/codegen.c \
	src/arch/coldfire/irgen.c \
	src/arch/coldfire/asm.c \
	src/arch/coldfire/predef.c \
	src/arch/coldfire/predef_cxx.c
$(foreach f,$(TARGET_FAMILIES),$(if $(FAMILY_SRCS_$(f)),,$(error FAMILY_SRCS_$(f): family $(f) has no sources)))
# A list with each word once, in the order first seen.
uniq = $(if $(1),$(firstword $(1)) $(call uniq,$(filter-out $(firstword $(1)),$(1))))

SRCS := \
	$(PLATFORM_SRCS) \
	src/driver/main.c \
	src/driver/util.c \
	src/driver/diag.c \
	src/driver/remark.c \
	src/driver/inspect.c \
	src/driver/asmout.c \
	src/driver/iface.c \
	src/driver/explain.c \
	src/driver/options.c \
	src/driver/stackinfo.c \
	src/driver/sbom.c \
	src/driver/paths.c \
	src/link/link.c \
	src/as/gas.c \
	src/embx/embx.c \
	src/lex/lex.c \
	src/cpp/cpp.c \
	src/parse/parse.c \
	src/cxx/cxx.c \
	src/cxx/tok.c \
	src/cxx/type.c \
	src/cxx/scope.c \
	src/cxx/parse.c \
	src/cxx/expr.c \
	src/cxx/class.c \
	src/cxx/vtable.c \
	src/cxx/template.c \
	src/cxx/consteval.c \
	src/cxx/traits.c \
	src/cxx/concepts.c \
	src/cxx/coro.c \
	src/cxx/access.c \
	src/cxx/mangle.c \
	src/cxx/emit.c \
	src/sema/sema.c \
	src/sema/type.c \
	src/sema/ldfloat.c \
	src/sema/w128.c \
	src/sema/uninit.c \
	src/sema/format.c \
	src/sema/rules.c \
	src/sema/analyze.c \
	src/ir/irgen.c \
	src/ir/irprint.c \
	src/ir/irparse.c \
	src/ir/gcov.c \
	src/ir/sanitize.c \
	$(OPT_SRCS) \
	src/debug/dwarf.c \
	src/debug/eh.c \
	src/elf/write.c \
	src/macho/write.c \
	src/coff/write.c \
	src/arch/target.c src/arch/backends.c src/arch/regalloc.c \
	src/arch/common.c \
	src/arch/code.c \
	src/arch/predef.c \
	$(call uniq,$(foreach f,$(TARGET_FAMILIES),$(FAMILY_SRCS_$(f))))

# SRCS is every source, whatever TARGETS says: it is what build.ebm, the
# self-host and tools/cross-embcc.sh compile, and they build the whole
# compiler. EMBCC_SRCS is this build's: the shared ones and the selected
# families'.
EMBCC_SRCS := $(filter-out $(foreach f,$(TARGET_FAMILIES),$(FAMILY_SRCS_$(f))),$(SRCS)) \
              $(call uniq,$(foreach f,$(TARGETS_BUILT),$(FAMILY_SRCS_$(f))))
OBJS := $(EMBCC_SRCS:src/%.c=$(OBJDIR)/%.o)

# One unit that is not under src/. src/link/link.c writes the EMBX
# image hash and the .embdbg sidecar through the SAME reader the embdbg
# tool uses -- R1, one implementation -- so every build that links the
# link library needs it: the driver (which links in-process now), the
# EmbBuild manifest, and the self-host. It is listed separately because
# the OBJS rule maps src/%.c and because it is compiled without its CLI
# main, which the driver already has.
SRCS_TOOLCORE := tools/embdbg/embdbg.c
# It goes with the families' objects, in OBJDIR, compiled with their
# TARGET_CFLAGS: without the x86 family it has no x86-64 decoder to call.
TOOLCORE_CFLAGS := -DEMBDBG_NO_MAIN -Wno-unused-function
EMBDBG_CORE := $(OBJDIR)/embdbg_core.o

$(EMBDBG_CORE): tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TARGET_CFLAGS) $(TOOLCORE_CFLAGS) -c -o $@ $<

# What a plain `make` builds: every tool and every target's runtime, so
# that what was built links. Without the runtimes, `embcc hello.c -o
# hello` on a Linux host found no crt1.o for x86_64-linux-gnu -- the first
# thing CMake's compiler check does -- and a firmware link found no
# librt.a. (The first rule used to be build/embdbg_core.o's, so `make`
# alone built that one object.) `make all` builds the tools alone.
.DEFAULT_GOAL := toolchain
# ...every target's of the families TARGETS builds: the hosted libraries of
# x86 and AArch64 when they are built, and the embedded runtimes, whose
# loops below run over the built families' triples (FAMILY_RT_*).
RUNTIMES_x86 := libc-x86_64 libcxx-x86_64 libc-linux-x86_64 libcxx-linux-x86_64 \
                rt-darwin-x86_64
RUNTIMES_aarch64 := libc-aarch64 libcxx-aarch64 libc-linux-aarch64 \
                    libcxx-linux-aarch64 rt-darwin-aarch64
RUNTIMES := $(foreach f,x86 aarch64,$(if $(call built,$(f)),$(RUNTIMES_$(f)))) \
            rt-embedded libc-embedded libcxx-embedded
.PHONY: toolchain runtimes
toolchain: all embdbg runtimes
runtimes: $(RUNTIMES)

all: embcc embread embld embas embls embidx embar embsvd embmap embpack embrt embsim \
     embflash embtrace embdoc binutils-names

# The binutils a build names, answered by EmbCC's own tools under the
# names CMake and a Makefile call them by: embar is ar and ranlib, embpack
# is objcopy for the image formats, embmap is size and nm. Each tool
# chooses its behaviour by the name it was called by.
BINUTILS_NAMES := ar:embar ranlib:embar objcopy:embpack size:embmap nm:embmap
.PHONY: binutils-names          # links, not a file of this name
binutils-names: embar embpack embmap
	@for p in $(BINUTILS_NAMES); do \
	    ln -sf $${p#*:} embcc-$${p%%:*}; \
	done

# Which host layer the last link used (PLATFORM and PROCESS), and which
# target families when not all of them (TARGETS). Switching any of them
# can leave every object up to date -- a subset's objects are in their own
# directory, and older than the ./embcc a different selection just linked
# -- so without this `make PLATFORM=iso` kept the old binary. Compared by
# NAME, not by time: a stamp touched in the same second as the link
# looked up to date.
PLATFORM_STAMP := $(BUILD)/platform.stamp
LINK_CONFIG := $(PLATFORM)-$(PROCESS)$(if $(TARGETS_OMITTED), $(TARGETS_BUILT))
ifneq ($(shell cat $(PLATFORM_STAMP) 2>/dev/null),$(LINK_CONFIG))
PLATFORM_CHANGED := platform-changed
endif
.PHONY: platform-changed
platform-changed:
	@mkdir -p $(BUILD); echo $(LINK_CONFIG) > $(PLATFORM_STAMP)

embcc: $(OBJS) $(EMBDBG_CORE) $(PLATFORM_CHANGED)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EMBDBG_CORE) $(PLATFORM_LIBS)

# The same compiler, linked INSIDE the object directory rather than at
# ./embcc. tests/golden/host-agnostic.sh builds EmbCC twice, with two
# different host compilers, and it runs in parallel with every other
# test -- so it must not replace the ./embcc those tests are running.
# With BUILD= pointing somewhere private, this target is entirely its
# own: `make CC=clang BUILD=/tmp/x /tmp/x/embcc`. The stamp too, so a
# second TARGETS= into the same BUILD relinks it.
$(BUILD)/embcc: $(OBJS) $(EMBDBG_CORE) $(PLATFORM_CHANGED)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(EMBDBG_CORE) $(PLATFORM_LIBS)

# embas — the standalone NASM/Intel-syntax assembler (A1, ARCHITECTURE §4). Reads
# the kernel's hand-written .asm and emits ELF objects the same writer (src/elf)
# the compiler uses produces, so the toolchain owns the whole build (drops nasm).
# src/arch/target.c and the two sema files it needs joined this list
# because src/elf/write.c asks target_ptr_size() to choose ELF32 vs
# ELF64 -- which it has done since the RISC-V backend landed, leaving
# `embas` unlinkable and `make all` failing. Nothing noticed, because
# no test builds embas: `make test` depends on embcc, embread, embld,
# embdbg and embls, and the assembler is only reached through the
# kernel build. tests/golden/tools-build.sh now builds every tool in
# `all`, which is the cheapest guard against the next one.
# embar -- the static-library archiver (tools/embar). ISO C and standalone,
# so the libraries below need no binutils `ar` on a host without GCC.
embar: tools/embar/embar.c
	$(CC) $(CFLAGS) -o $@ tools/embar/embar.c

# embsvd -- a device's register database from its CMSIS-SVD file: the
# device header, a startup file and a linker script (tools/embsvd). ISO C
# and standalone, like embar.
embsvd: tools/embsvd/embsvd.c tools/embsvd/svd.c tools/embsvd/svd.h
	$(CC) $(CFLAGS) -o $@ tools/embsvd/embsvd.c tools/embsvd/svd.c

# embmap -- where an image's flash and RAM go: sections, regions, the
# biggest symbols, the bytes by input file from a map, and the growth
# between two builds (tools/embmap). ISO C and standalone, like embar.
embmap: tools/embmap/embmap.c
	$(CC) $(CFLAGS) -o $@ tools/embmap/embmap.c

# embpack -- a linked image as the file a programmer or bootloader takes:
# raw binary, Intel HEX, S-records or UF2, with a CRC-32 stamped into a
# symbol or appended and a JSON manifest (tools/embpack). ISO C and
# standalone, like embar.
embpack: tools/embpack/embpack.c
	$(CC) $(CFLAGS) -o $@ tools/embpack/embpack.c

# embflash -- an image put into a target through the GDB remote protocol
# (QEMU, OpenOCD, pyOCD, J-Link's GDB server, the Black Magic Probe):
# flash erased and programmed by the server's algorithm, RAM written,
# verified, run (tools/embflash). ISO C, POSIX sockets and termios.
embflash: tools/embflash/embflash.c tools/embflash/esp32boot.inc tools/embflash/esp32stub.inc
	$(CC) $(CFLAGS) -o $@ tools/embflash/embflash.c

# embtrace -- what a program built with -finstrument-functions did: calls,
# times, the call tree and a Chrome/Perfetto trace, from the ring lib/rt's
# recorder (lib/rt/embtrace.c) dumps to a console (tools/embtrace). ISO C.
embtrace: tools/embtrace/embtrace.c
	$(CC) $(CFLAGS) -o $@ tools/embtrace/embtrace.c

# embrt -- the worst-case stack of each entry point and interrupt, from
# the compiler's frames (-fstack-usage), its call graph
# (-fcallgraph-info=su) and the objects' call relocations (tools/embrt).
# ISO C and standalone, like embar.
embrt: tools/embrt/embrt.c
	$(CC) $(CFLAGS) -o $@ tools/embrt/embrt.c

# embdoc -- the documentation as a website: README.md, docs/ and each
# tools/X/README.md rendered from their Markdown as GitHub renders it,
# with the navigation, an "On this page" list and a search index, and
# every relative link and #anchor checked (tools/embdoc). ISO C but for
# host.c, which lists and makes directories with POSIX's calls.
EMBDOC_SRCS := tools/embdoc/embdoc.c tools/embdoc/md.c tools/embdoc/assets.c \
               tools/embdoc/host.c
embdoc: $(EMBDOC_SRCS) tools/embdoc/embdoc.h
	$(CC) $(CFLAGS) -o $@ $(EMBDOC_SRCS)

# The site itself, in build/site: never committed (.github/workflows/
# site.yml publishes it from main). `./embdoc check` lists broken links.
site: embdoc
	./embdoc site --out $(BUILD)/site

# The manual's reference pages, printed by the compiler from the tables they
# describe (src/driver/options.def, diag.c's warnings, src/targets/). They are
# committed, so they read on GitHub too; tests/golden/reference-pages.sh fails
# when one no longer matches what the compiler prints. `make reference`
# brings them up to date.
reference: embcc
	./embcc --reference=options > docs/manual/options.md
	./embcc --reference=warnings > docs/manual/warnings.md
	./embcc --reference=targets > docs/manual/target-list.md
	./embcc --reference=boards > docs/manual/board-list.md
.PHONY: reference

# embsim -- a simulator (Cortex-M, RISC-V, AVR): runs an image on a model of the board
# QEMU models, counting instructions and estimating cycles with the
# table tools/bench uses (src/arch/cost.h). ISO C and libm; the
# modules are tools/embsim/sim.h's (docs/internals/embsim.md). The GDB
# server's connection is net-posix.c; net-none.c builds it without one.
EMBSIM_NET ?= tools/embsim/net-posix.c
EMBSIM_SRCS := tools/embsim/main.c tools/embsim/run.c tools/embsim/bus.c \
               tools/embsim/boards.c tools/embsim/loader.c \
               tools/embsim/semihost.c tools/embsim/trace.c \
               tools/embsim/cortexm.c tools/embsim/cortexm-thumb.c \
               tools/embsim/cortexm-fpu.c tools/embsim/scs.c \
               tools/embsim/systick.c tools/embsim/dwt.c \
               tools/embsim/uart-pl011.c tools/embsim/uart-cmsdk.c \
               tools/embsim/uart-nrf51.c tools/embsim/riscv.c \
               tools/embsim/riscv-fpu.c tools/embsim/clint.c \
               tools/embsim/uart-16550.c tools/embsim/sifive-test.c \
               tools/embsim/virt-rom.c tools/embsim/avr.c \
               tools/embsim/avr-io.c tools/embsim/avr-usart.c \
               tools/embsim/avr-timer16.c tools/embsim/gdb.c \
               tools/embsim/svd-map.c tools/embsvd/svd.c \
               tools/embsim/stm32-rcc.c tools/embsim/stm32-gpio.c \
               tools/embsim/stm32-usart.c tools/embsim/stm32-tim.c \
               tools/embsim/image.c tools/embsim/analysis.c \
               tools/embsim/coverage.c tools/embsim/profile.c \
               tools/embsim/stack.c tools/embsim/fault.c \
               tools/embsim/disasm.c tools/embsim/record.c tools/embsim/i2c-bus.c \
               tools/embsim/mips.c tools/embsim/malta.c \
               tools/embsim/atdf.c tools/embsim/pic32-evic.c \
               tools/embsim/pic32-cru.c tools/embsim/pic32-tmr.c \
               tools/embsim/pic32-gpio.c tools/embsim/pic32-uart.c \
               tools/embsim/pic32-pmp.c tools/embsim/pic32-adc.c \
               tools/embsim/pic-boards.c tools/embsim/ieee754.c \
               tools/embsim/mips-fpu.c tools/embsim/edc.c \
               tools/embsim/pic24.c tools/embsim/pic24-intc.c \
               tools/embsim/pic24-adc.c tools/embsim/pic24-epmp.c \
               tools/embsim/pic24-spi.c tools/embsim/pic24-uart.c \
               tools/embsim/pic24-tmr.c tools/embsim/pic24-osc.c \
               tools/embsim/mips-dsp.c tools/embsim/mips-micro.c \
               tools/embsim/mips16.c tools/embsim/pic24-nvm.c \
               tools/embsim/pic24-rtcc.c tools/embsim/pic24-dma.c \
               tools/embsim/pic24-i2c.c tools/embsim/pic24-ic.c \
               tools/embsim/pic24-oc.c tools/embsim/pic24-ccp.c \
               tools/embsim/pic32-spi.c tools/embsim/pic32-adc10.c \
               tools/embsim/pic24-dsp.c tools/embsim/pic24e-spi.c tools/embsim/pic24e-i2c.c \
               tools/embsim/pic24e-adc.c \
               tools/embsim/rp2040.c tools/embsim/rp2040-timer.c \
               tools/embsim/rp2040-io.c tools/embsim/rp2040-sio.c \
               tools/embsim/rp2040-uart.c tools/embsim/rp2040-ssi.c \
               tools/embsim/mcu-boards.c tools/embsim/stm32f1-gpio.c \
               tools/embsim/nrf52.c tools/embsim/esp32c3.c tools/embsim/samd21.c \
               $(EMBSIM_NET)
EMBSIM_HDRS := tools/embsim/sim.h tools/embsim/cortexm.h tools/embsim/riscv.h \
               tools/embsim/avr.h \
               tools/embsim/devices.h tools/embsim/net.h src/arch/cost.h \
               tools/embsim/svd-map.h tools/embsvd/svd.h \
               tools/embsim/image.h tools/embsim/analysis.h \
               tools/embsim/disasm.h tools/embsim/mips.h tools/embsim/pic32.h \
               tools/embsim/ieee754.h tools/embsim/pic24.h tools/embsim/rp2040.h \
               tools/embsim/i2c-bus.def
# Optimized, unlike the other tools: a simulator's speed is the tests'
# and the user's time, and -O2 runs a busy loop 2.5 (M3) to 3.6 (AVR)
# times as fast as no -O. -g stays (it is in CFLAGS); `make EMBSIM_OPT=`
# builds it unoptimized to debug. On the Makefile too, so that a change
# here (EMBSIM_OPT's default, the list) rebuilds it: without that, an
# ./embsim built before -O2 was the default stayed unoptimized.
EMBSIM_OPT ?= -O2
embsim: $(EMBSIM_SRCS) $(EMBSIM_HDRS) Makefile
	$(CC) $(CFLAGS) $(EMBSIM_OPT) -o $@ $(EMBSIM_SRCS) -lm

embas: tools/embas/embas.c src/arch/x86_64/as.c src/arch/x86_64/as.h \
       src/elf/write.c src/elf/elf.h src/driver/util.c src/driver/diag.c \
       src/arch/target.c src/sema/type.c src/sema/ldfloat.c \
       $(PLATFORM_SRCS) src/platform/platform.h
	$(CC) $(CFLAGS) -o $@ tools/embas/embas.c src/arch/x86_64/as.c \
	    src/elf/write.c src/driver/util.c src/driver/diag.c \
	    src/arch/target.c src/sema/type.c src/sema/ldfloat.c \
	    $(PLATFORM_SRCS)

# embld — the integrated linker (ARCHITECTURE §6, WORKPLAN stream B), as
# a standalone tool for host development. The link library also gets
# wired into embcc so `embcc prog.c -o prog` links in-process.
# embld also links the EmbDBG core (compiled -DEMBDBG_NO_MAIN, so no CLI main)
# so the linker can emit a native .embdbg at link time through the SAME format
# writer the embdbg tool uses — one implementation, not two.
embld: tools/embld/embld.c tools/embld/doctor.c src/link/link.c \
       src/driver/util.c src/driver/diag.c src/driver/explain.c \
       src/arch/riscv/emit.c src/arch/avr/emit.c src/arch/mips/emit.c \
       src/arch/loongarch/emit.c src/arch/thumb/a32.c src/arch/thumb/a32.h \
       src/arch/tricore/emit.c src/arch/xtensa/emit.c src/arch/ppc/emit.c src/arch/rx/emit.c src/arch/sparc/emit.c src/arch/code.c \
       src/link/link.h src/link/ldscript.h src/elf/elf.h src/embx/embx.c src/embx/embx.h \
       tools/embdbg/embdbg.c tools/embdbg/embdbg_core.h \
       $(PLATFORM_SRCS) src/platform/platform.h
	$(CC) $(CFLAGS) -DEMBDBG_NO_MAIN -Wno-unused-function -o $@ \
	    tools/embld/embld.c tools/embld/doctor.c src/link/link.c \
	    src/driver/util.c src/driver/diag.c src/driver/explain.c \
	    src/embx/embx.c tools/embdbg/embdbg.c $(PLATFORM_SRCS) \
	    src/arch/x86_64/disasm.c src/arch/riscv/emit.c src/arch/avr/emit.c \
	    src/arch/mips/emit.c src/arch/loongarch/emit.c src/arch/thumb/a32.c src/arch/tricore/emit.c src/arch/xtensa/emit.c src/arch/ppc/emit.c src/arch/rx/emit.c src/arch/sparc/emit.c src/arch/code.c

# NOTE: this list is HAND-MAINTAINED and `make check` does not build embls, so
# a backend file added without a line here breaks only `make test` -- and
# breaks it at the BUILD step, so the suite reports nothing rather than
# failing a test. That has now happened twice in one week: src/arch/avr/asm.c
# and src/arch/avr/irgen.c when the AVR assembler and inline asm landed, and
# src/arch/thumbv8m/predef*.c when ARMv8-M did. Anything defining a symbol
# that sema.c, irgen.c or predef.c's arch_table reaches belongs here.
#
# embls — the language server (docs/manual/diagnostics.md T5). It links EmbCC's own
# preprocessor and parser, so what an editor is told about a file comes from
# the compiler that will compile it; diagnostics it gets by running embcc
# itself. The parse runs in a forked child, because a front end ends the
# process where it cannot continue and a server must not.
EMBLS_SRCS = tools/embls/embls.c $(PLATFORM_SRCS) src/cpp/cpp.c src/lex/lex.c \
             src/parse/parse.c src/sema/type.c src/sema/ldfloat.c \
             src/sema/w128.c src/sema/uninit.c src/sema/format.c src/sema/rules.c src/sema/analyze.c \
             src/driver/util.c src/driver/diag.c src/driver/remark.c \
             src/driver/inspect.c src/driver/asmout.c src/driver/iface.c \
             src/driver/explain.c \
             src/arch/target.c src/arch/predef.c src/arch/x86_64/predef.c \
             src/arch/aarch64/predef.c src/arch/x86_64/predef_cxx.c \
             src/arch/aarch64/predef_cxx.c \
             src/arch/thumb/predef.c src/arch/thumb/predef_cxx.c \
             src/arch/riscv32/predef.c src/arch/riscv32/predef_cxx.c \
             src/arch/riscv64/predef.c src/arch/riscv64/predef_cxx.c \
             src/arch/mips32/predef.c src/arch/mips32/predef_cxx.c \
             src/arch/mips32eb/predef.c src/arch/mips32eb/predef_cxx.c \
             src/arch/mips64/predef.c src/arch/mips64/predef_cxx.c \
             src/arch/mips64eb/predef.c src/arch/mips64eb/predef_cxx.c \
             src/arch/loongarch64/predef.c src/arch/loongarch64/predef_cxx.c \
             src/arch/tricore/predef.c src/arch/tricore/predef_cxx.c \
             src/arch/xtensa/predef.c src/arch/xtensa/predef_cxx.c \
             src/arch/ppc32/predef.c src/arch/ppc32/predef_cxx.c \
             src/arch/rx/predef.c \
             src/arch/sparc32/predef.c src/arch/sparc32/predef_cxx.c \
             src/arch/coldfire/predef.c src/arch/coldfire/predef_cxx.c \
             src/arch/avr/predef.c src/arch/avr/predef_cxx.c \
             src/arch/avr/options.c \
             src/arch/thumbv8m/predef.c src/arch/thumbv8m/predef_cxx.c \
             src/arch/thumbv6m/predef.c src/arch/thumbv6m/predef_cxx.c \
             src/arch/thumbv8mbase/predef.c src/arch/thumbv8mbase/predef_cxx.c \
             src/arch/armv7a/predef.c src/arch/armv7a/predef_cxx.c \
             $(filter src/cxx/%,$(SRCS)) src/sema/sema.c src/ir/irgen.c \
             src/ir/irprint.c src/ir/irparse.c src/ir/gcov.c src/ir/sanitize.c \
             $(OPT_SRCS) src/debug/dwarf.c src/debug/eh.c src/elf/write.c \
             src/as/gas.c \
             src/arch/code.c src/arch/regalloc.c src/arch/common.c \
             src/arch/x86_64/irgen.c src/arch/x86_64/codegen.c \
             src/arch/x86_64/emit.c src/arch/x86_64/topasm.c \
             src/arch/x86_64/as.c src/arch/x86_64/att.c src/arch/x86_64/disasm.c \
             src/arch/aarch64/irgen.c \
             src/arch/aarch64/codegen.c src/arch/aarch64/emit.c \
             src/arch/aarch64/asm.c src/arch/thumb/irgen.c src/arch/thumb/asm.c \
             src/arch/thumb/emit.c src/arch/thumb/a32.c src/arch/thumb/attrs.c \
             src/arch/riscv/irgen.c src/arch/riscv/attrs.c \
             src/arch/riscv/asm.c src/arch/riscv/emit.c \
             src/arch/mips/irgen.c src/arch/mips/asm.c src/arch/mips/emit.c \
             src/arch/loongarch/irgen.c src/arch/loongarch/emit.c \
             src/arch/loongarch/asm.c \
             src/arch/tricore/irgen.c src/arch/tricore/asm.c \
             src/arch/tricore/emit.c \
             src/arch/xtensa/irgen.c src/arch/xtensa/emit.c \
             src/arch/xtensa/asm.c \
             src/arch/ppc/irgen.c src/arch/ppc/emit.c \
             src/arch/ppc/asm.c \
             src/arch/rx/irgen.c src/arch/rx/emit.c \
             src/arch/rx/asm.c \
             src/arch/sparc/irgen.c src/arch/sparc/emit.c \
             src/arch/sparc/asm.c \
             src/arch/coldfire/irgen.c src/arch/coldfire/asm.c \
             src/arch/coldfire/emit.c \
             src/arch/avr/asm.c src/arch/avr/irgen.c src/arch/avr/emit.c
embls: $(EMBLS_SRCS)
	$(CC) $(CFLAGS) -o $@ $(EMBLS_SRCS)

# embidx — the cross-TU index (vision 8.2). It links none of the
# compiler: it runs `embcc --emit-interfaces` and stores what comes back,
# so the TEXT is the contract between them and an external tool can take
# the same input.
embidx: tools/embidx/embidx.c
	$(CC) $(CFLAGS) -o $@ tools/embidx/embidx.c

# embread — the EMBX dumper/verifier (EMBX spec §9). A separate binary,
# not part of embcc: it reads images, it does not compile. The EMBX
# container definition it shares with the future linker lives in
# src/embx/, exactly as src/elf/ is shared by asm and link.
embread: tools/embread/embread.c src/embx/embx.c src/embx/embx.h
	$(CC) $(CFLAGS) -o $@ tools/embread/embread.c src/embx/embx.c

# embdbg — EmbDBG v0, the debug-info reader/symbolizer (EMBDBG step 1's
# consumer). Standalone like embread: it reads the DWARF EmbCC emits, it does
# not compile. The live-control half is gated on the kernel debug contract.
# embdbg reads the x86-64 decoder from src/arch/x86_64 (disasm.c) rather
# than carrying its own: the compiler needs it for -S, and one decoder is
# the same discipline as one encoder (R1).
embdbg: tools/embdbg/embdbg.c tools/embdbg/remote.c tools/embdbg/remote.h \
        src/elf/elf.h src/arch/x86_64/disasm.c src/arch/x86_64/disasm.h
	$(CC) $(CFLAGS) -o $@ tools/embdbg/embdbg.c tools/embdbg/remote.c \
	    src/arch/x86_64/disasm.c

# The target database is #included by target.c, and nothing else tracks
# header dependencies, so say this one.
$(OBJDIR)/arch/target.o: $(wildcard src/targets/*.def)
# ...and the option table, by options.c
$(OBJDIR)/driver/options.o: src/driver/options.def

# TARGET_CFLAGS: the families left out (make TARGETS=), empty for all.
$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TARGET_CFLAGS) -c -o $@ $<

# The tree is small; every object depending on every header is honest
# enough and cannot go stale (CONTRIBUTING lie #1).
$(OBJS): $(wildcard src/*/*.h src/arch/*/*.h)

# The libc and libcxx goldens run against the BUILT archives, so the suite
# depends on them: without this a stale archive is silently what gets
# tested, and a fix made in the library is reported as still broken (or,
# worse, a break is reported as fixed).
# The fast loop: every program in tests/exec and tests/cxx, compiled,
# linked and RUN -- 217 of them in about 27 seconds. It skips the
# goldens, which is where the time goes: three of them recompile the
# whole corpus a second time with gcc and diff the results, and one
# builds GCC's libstdc++ from source.
#
# This is what to run while changing something. It catches essentially
# every codegen and front-end regression, because those show up as a
# program printing the wrong answer -- which is what it checks.
#
# It is NOT a substitute for `make test` before a commit. What it does
# not check is exactly what the goldens exist for: that the answers
# agree with gcc's, that -S and -c build the same program, that the
# object format is what the platform's tools expect.
# Without x86 (make TARGETS=) the libraries are not built and tests/run.sh
# reports the suite skipped: there is nothing here to run it on.
check: embcc $(if $(call built,x86),libc-x86_64 libcxx-x86_64)
	tests/run.sh --exec-only

# embas belongs here too: tests/golden/x86_64/assembler.sh runs it, and
# without it in this list the suite passes from a dirty tree and fails
# from a clean one -- which is the wrong way round.
test: embcc embread embld embdbg embls embas embar embsvd embmap embpack \
      embrt embsim embflash embtrace embdoc $(if $(call built,x86),libc-x86_64 \
      libcxx-x86_64 libc-linux-x86_64 libcxx-linux-x86_64)
	tests/run.sh

# The aarch64 suite: compile for the second architecture and RUN the result
# under qemu-system-aarch64 (tests/harness/aarch64). Separate from `test`
# because it needs the cross newlib and QEMU, which `test` does not.
test-arm64: embcc $(if $(call built,aarch64),$(RUNTIMES_aarch64))
	tests/run.sh --target=aarch64-elf

# The C++ suites wholly on EmbCC's library: libstdc++ and libsupc++ built
# from GCC's sources by EmbCC, every tests/libstdcxx and tests/cxx program
# linked with it and compared with g++'s build — both targets. Opt-in (the
# library takes minutes to build): tests/golden/cxx-libstdcxx-embcc.sh.
test-libstdcxx: embcc
	EMBCC=$(CURDIR)/embcc EMBCC_TARGET=x86_64-elf EMBCC_LIBSTDCXX=1 \
	    sh tests/golden/cxx-libstdcxx-embcc.sh
	EMBCC=$(CURDIR)/embcc EMBCC_TARGET=aarch64-elf EMBCC_LIBSTDCXX=1 \
	    sh tests/golden/cxx-libstdcxx-embcc.sh

# ---- our C library (lib/libc) ------------------------------------------
# One implementation, ported to an OS by one small backend. Built with
# EmbCC itself, per target -- which is also the widest test the compiler
# gets outside its own sources.
# Everything above the seam, which is every target's library verbatim. The
# backend is appended per target -- that difference IS the port.
LIBC_SRCS_PORTABLE := $(wildcard lib/libc/src/*/*.c) \
                      $(wildcard lib/libc/src/math/fdlibm/*.c) \
                      $(wildcard lib/libc/src/math/msun/*.c)
LIBC_SRCS := $(LIBC_SRCS_PORTABLE) lib/libc/os/posixlike/backend.c
LIBC_INC  := -Ilib/libc/include -Ilib/libc/src/math

libc-x86_64: embcc embar
	@mkdir -p $(BUILD)/libc/x86_64
	@for f in $(LIBC_SRCS); do \
	    o=$(BUILD)/libc/x86_64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=x86_64-elf -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/x86_64/libc.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libc/x86_64/libc.a $(BUILD)/libc/x86_64/*.o
	@echo "libc: $(BUILD)/libc/x86_64/libc.a"
	@EMBCC=./embcc sh tools/build-rt.sh x86_64-elf $(BUILD)/libc/x86_64 > /dev/null
	@echo "rt: $(BUILD)/libc/x86_64/librt.a"

libc-aarch64: embcc embar
	@mkdir -p $(BUILD)/libc/aarch64
	@for f in $(LIBC_SRCS); do \
	    o=$(BUILD)/libc/aarch64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=aarch64-elf -c -O1 $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/aarch64/libc.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs $(BUILD)/libc/aarch64/libc.a $(BUILD)/libc/aarch64/*.o
	@echo "libc: $(BUILD)/libc/aarch64/libc.a"
	@EMBCC=./embcc sh tools/build-rt.sh aarch64-elf $(BUILD)/libc/aarch64 > /dev/null
	@echo "rt: $(BUILD)/libc/aarch64/librt.a"

# EmbCC's compiler runtime for macOS (lib/rt, as Mach-O): the routines its
# code calls that libSystem does not export -- __divdc3 for a complex
# division, the 128-bit shifts and multiply, __muloti4 -- which clang takes
# from its own libclang_rt.osx.a. The driver links it after -lSystem, so
# what the system has comes from the system.
rt-darwin-aarch64: embcc embar
	@EMBCC=./embcc sh tools/build-rt.sh aarch64-apple-darwin $(BUILD)/libc/darwin-aarch64 > /dev/null
	@echo "rt: $(BUILD)/libc/darwin-aarch64/librt.a"

rt-darwin-x86_64: embcc embar
	@EMBCC=./embcc sh tools/build-rt.sh x86_64-apple-darwin $(BUILD)/libc/darwin-x86_64 > /dev/null
	@echo "rt: $(BUILD)/libc/darwin-x86_64/librt.a"

# EmbLinkOS is a backend, not a second library: same sources, one different
# file at the bottom. It needs that OS's ABI headers, so it is opt-in.
#   make libc-emblinkos EMBLINKOS=$HOME/EmbLinkOs
#   make libc-emblinkos EMBLINKOS=... EMBLINKOS_ARCH=aarch64
# x86_64's goes to build/libc/emblinkos, aarch64's to emblinkos-aarch64.
EMBLINKOS ?= $(HOME)/EmbLinkOs
EMBLINKOS_ARCH ?= x86_64
ELOS_SUFFIX  = $(if $(filter aarch64,$(EMBLINKOS_ARCH)),-aarch64,)
ELOS_LIBC    = $(BUILD)/libc/emblinkos$(ELOS_SUFFIX)
ELOS_LIBCXX  = $(BUILD)/libcxx/emblinkos$(ELOS_SUFFIX)
# --unwind-tables: every function gets its .eh_frame, as GCC gives every
# x86-64 function by default. The unwinder needs its own frames' (without
# them a throw's walk ends where it starts, in _Unwind_RaiseException), and a
# throw from a C++ callback passes through C frames -- qsort's, bsearch's.
ELOS_CC      = ./embcc --target=$(EMBLINKOS_ARCH)-elf --unwind-tables
# The C library gets -D__EMBLIBC_UNWINDER, as its librt.a has the unwinder
# (backtrace walks with it), and alone -D__EMBLIBC_TLS_TEMPLATE: that OS's crt0 hands
# over the thread-local template (lib/libc/os/tls-template.h), which makes
# errno per thread and gives each new thread its block.

libc-emblinkos: embcc embar
	@[ -f "$(EMBLINKOS)/user/lib/embk.h" ] || { \
	    echo "libc-emblinkos: no $(EMBLINKOS)/user/lib/embk.h"; \
	    echo "  set EMBLINKOS=/path/to/EmbLinkOs"; exit 1; }
	@case "$(EMBLINKOS_ARCH)" in x86_64|aarch64) ;; *) \
	    echo "libc-emblinkos: EMBLINKOS_ARCH is x86_64 or aarch64"; exit 1;; esac
	@mkdir -p $(ELOS_LIBC)
	@for f in $(LIBC_SRCS_PORTABLE) lib/libc/os/posix-names.c \
	          lib/libc/os/emblinkos/backend.c; do \
	    o=$(ELOS_LIBC)/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    $(ELOS_CC) -c -O2 -D__EMBLIBC_TLS_TEMPLATE -D__EMBLIBC_UNWINDER $(LIBC_INC) -I$(EMBLINKOS)/user/lib $$f -o $$o || exit 1; \
	done
	@rm -f $(ELOS_LIBC)/libc.a $(ELOS_LIBC)/libm.a
	@$${EMBCC_X86_AR:-./embar} rcs $(ELOS_LIBC)/libc.a $(ELOS_LIBC)/*.o
	@# libm.a holds nothing: the math is in libc.a, as glibc's is, and an
	@# empty one beside it keeps a program's -lm from finding another libc's
	@printf '' > $(ELOS_LIBC)/libm-empty.c
	@$(ELOS_CC) -c $(ELOS_LIBC)/libm-empty.c -o $(ELOS_LIBC)/libm-empty.o.x
	@$${EMBCC_X86_AR:-./embar} rcs $(ELOS_LIBC)/libm.a $(ELOS_LIBC)/libm-empty.o.x
	@# librt.a: the compiler runtime, with the unwinder C++'s exceptions
	@# need -- its programs have no other (lib/rt/unwind.c says why)
	@for f in $(RT_SRCS); do \
	    o=$(ELOS_LIBC)/rt_$$(basename $$f .c).o.x; \
	    $(ELOS_CC) -c -O2 -D__EMBCC_RT_UNWIND $$f -o $$o || exit 1; \
	done
	@rm -f $(ELOS_LIBC)/librt.a
	@$${EMBCC_X86_AR:-./embar} rcs $(ELOS_LIBC)/librt.a $(ELOS_LIBC)/rt_*.o.x
	@$(ELOS_CC) -c -O2 lib/gcov/gcov.c -o $(ELOS_LIBC)/gcov.o.x
	@rm -f $(ELOS_LIBC)/libgcov.a
	@$${EMBCC_X86_AR:-./embar} rcs $(ELOS_LIBC)/libgcov.a $(ELOS_LIBC)/gcov.o.x
	@echo "libc: $(ELOS_LIBC)/libc.a (+ an empty libm.a, and librt.a)"

# C++ on EmbLinkOS: lib/libcxx over this C library, compiled by EmbCC.
#   make libcxx-emblinkos EMBLINKOS=$HOME/EmbLinkOs [EMBLINKOS_ARCH=aarch64]
libcxx-emblinkos: embcc embar
	@[ -f "$(EMBLINKOS)/user/lib/embk.h" ] || { \
	    echo "libcxx-emblinkos: no $(EMBLINKOS)/user/lib/embk.h"; exit 1; }
	@mkdir -p $(ELOS_LIBCXX)
	@for f in $(LIBCXX_SRCS); do \
	    o=$(ELOS_LIBCXX)/$$(basename $$f .cc).o; \
	    $(ELOS_CC) -c -O2 -x c++ $(LIBCXX_INC) -I$(EMBLINKOS)/user/lib $$f -o $$o || exit 1; \
	done
	@rm -f $(ELOS_LIBCXX)/libcxx.a
	@$${EMBCC_X86_AR:-./embar} rcs $(ELOS_LIBCXX)/libcxx.a $(ELOS_LIBCXX)/*.o
	@echo "libcxx: $(ELOS_LIBCXX)/libcxx.a"

# Linux, which is the first target this library reaches with NO other C
# library underneath it: lib/libc/os/linux/backend.c issues syscalls, so
# a program linked against this archive needs no glibc, no musl and no
# dynamic loader. That is also why there is a crt1.o here and not for
# the targets above -- on those, somebody else supplied the entry point.
#
# crt1.o stays OUT of the archive on purpose. _start must be defined
# exactly once in an image, and a definition that arrives by archive
# member is a definition that arrives by accident.
#   make libc-linux-x86_64   /   make libc-linux-aarch64
LIBC_SRCS_LINUX := $(LIBC_SRCS_PORTABLE) lib/libc/os/posix-names.c \
                   lib/libc/os/linux/backend.c \
                   lib/libc/os/linux/posix.c \
                   lib/libc/os/linux/thread.c lib/libc/os/linux/tls.c

# lib/rt -- the COMPILER runtime, which is a different library from the C
# one: libc implements what a program asks for by name, and no program
# ever writes __multi3. It is a separate archive so the freestanding
# targets, which link somebody else's libgcc, are not given two
# definitions of the same routine.
#
# It ships as librt.a beside libc.a, and the driver puts it on the link
# line AFTER libc, because a libc routine can call into it (a printf of
# a 128-bit value) and an archive is searched once.
RT_SRCS := $(wildcard lib/rt/*.c)

libc-linux-x86_64: embcc embar
	@mkdir -p $(BUILD)/libc/linux-x86_64
	@for f in $(LIBC_SRCS_LINUX); do \
	    o=$(BUILD)/libc/linux-x86_64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=x86_64-linux-gnu -c -O1 -fPIE $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-x86_64/libc.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libc/linux-x86_64/libc.a \
	    $(BUILD)/libc/linux-x86_64/*.o
	@./embcc --target=x86_64-linux-gnu -c -O1 -fPIE $(LIBC_INC) \
	    lib/libc/os/linux/start.c -o $(BUILD)/libc/linux-x86_64/crt1.o
	@rm -rf $(BUILD)/libc/linux-x86_64/rt && mkdir -p $(BUILD)/libc/linux-x86_64/rt
	@for f in $(RT_SRCS); do \
	    o=$(BUILD)/libc/linux-x86_64/rt/$$(basename $$f .c).o; \
	    ./embcc --target=x86_64-linux-gnu -c -O1 -fPIE $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-x86_64/librt.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libc/linux-x86_64/librt.a \
	    $(BUILD)/libc/linux-x86_64/rt/*.o
	@./embcc --target=x86_64-linux-gnu -c -O1 -fPIE lib/gcov/gcov.c \
	    -o $(BUILD)/libc/linux-x86_64/rt/gcov.o.x
	@rm -f $(BUILD)/libc/linux-x86_64/libgcov.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libc/linux-x86_64/libgcov.a \
	    $(BUILD)/libc/linux-x86_64/rt/gcov.o.x
	@for s in asan tsan msan; do \
	    ./embcc --target=x86_64-linux-gnu -c -O1 -fPIE lib/san/$$s.c \
	        -o $(BUILD)/libc/linux-x86_64/rt/$$s.o.x || exit 1; \
	    rm -f $(BUILD)/libc/linux-x86_64/lib$$s.a; \
	    $${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libc/linux-x86_64/lib$$s.a \
	        $(BUILD)/libc/linux-x86_64/rt/$$s.o.x || exit 1; \
	done
	@echo "libc: $(BUILD)/libc/linux-x86_64/libc.a + librt.a + libgcov.a + lib{a,t,m}san.a + crt1.o"

libc-linux-aarch64: embcc embar
	@mkdir -p $(BUILD)/libc/linux-aarch64
	@for f in $(LIBC_SRCS_LINUX); do \
	    o=$(BUILD)/libc/linux-aarch64/$$(echo $$f | tr / _ | sed 's/\.c$$/.o/'); \
	    ./embcc --target=aarch64-linux-gnu -c -O1 -fPIE $(LIBC_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-aarch64/libc.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs \
	    $(BUILD)/libc/linux-aarch64/libc.a $(BUILD)/libc/linux-aarch64/*.o
	@./embcc --target=aarch64-linux-gnu -c -O1 -fPIE $(LIBC_INC) \
	    lib/libc/os/linux/start.c -o $(BUILD)/libc/linux-aarch64/crt1.o
	@rm -rf $(BUILD)/libc/linux-aarch64/rt && mkdir -p $(BUILD)/libc/linux-aarch64/rt
	@for f in $(RT_SRCS); do \
	    o=$(BUILD)/libc/linux-aarch64/rt/$$(basename $$f .c).o; \
	    ./embcc --target=aarch64-linux-gnu -c -O1 -fPIE $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libc/linux-aarch64/librt.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs \
	    $(BUILD)/libc/linux-aarch64/librt.a $(BUILD)/libc/linux-aarch64/rt/*.o
	@./embcc --target=aarch64-linux-gnu -c -O1 -fPIE lib/gcov/gcov.c \
	    -o $(BUILD)/libc/linux-aarch64/rt/gcov.o.x
	@rm -f $(BUILD)/libc/linux-aarch64/libgcov.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs $(BUILD)/libc/linux-aarch64/libgcov.a \
	    $(BUILD)/libc/linux-aarch64/rt/gcov.o.x
	@for s in asan tsan msan; do \
	    ./embcc --target=aarch64-linux-gnu -c -O1 -fPIE lib/san/$$s.c \
	        -o $(BUILD)/libc/linux-aarch64/rt/$$s.o.x || exit 1; \
	    rm -f $(BUILD)/libc/linux-aarch64/lib$$s.a; \
	    $${EMBCC_AARCH64_AR:-./embar} rcs $(BUILD)/libc/linux-aarch64/lib$$s.a \
	        $(BUILD)/libc/linux-aarch64/rt/$$s.o.x || exit 1; \
	done
	@echo "libc: $(BUILD)/libc/linux-aarch64/libc.a + librt.a + libgcov.a + lib{a,t,m}san.a + crt1.o"

# The compiler runtime for each EMBEDDED target, as an archive per triple.
# These had no rule at all: a firmware build for any of the four targets the
# EmbLinkRTOS requirements name had to compile lib/rt by hand and choose the
# objects itself. tools/build-rt.sh is the one place the recipe lives, and
# tests/golden/embedded-runtime.sh uses it too, so the archive the test checks
# is the archive that ships.
# The -eabihf ones are the hard-float convention: its objects do not link
# with soft-float ones, so its runtime is a separate archive. RISC-V's
# hardware-float ABIs are the same, named by the ABI under the triple's
# directory (tools/build-rt.sh says how each is built).
RISCV_HF := riscv32-unknown-elf/ilp32f riscv32-unknown-elf/ilp32d \
            riscv64-unknown-elf/lp64f riscv64-unknown-elf/lp64d
# ...and the soft-float ABI without the A extension (an ESP32-C3's rv32imc),
# whose C library cannot hold the amo instructions of the others' locks
RISCV_HF += riscv32-unknown-elf/noa riscv64-unknown-elf/noa
RT_EMBEDDED_ALL := avr avr/avr6 thumbv6m-none-eabi thumbv8m.base-none-eabi thumbv7m-none-eabi thumbv7em-none-eabi \
               thumbv7em-none-eabihf thumbv8m.main-none-eabi \
               thumbv8m.main-none-eabihf armv7a-none-eabi armv7a-none-eabihf riscv32-unknown-elf \
               riscv64-unknown-elf $(RISCV_HF) mipsel-none-elf mips-none-elf loongarch64-unknown-elf tricore-none-elf \
                 xtensa-none-elf powerpc-none-eabi rx-none-elf sparc-none-elf m68k-none-elf \
                 mips64el-none-elf mips64-none-elf
# The triples of each family's embedded runtimes; the lists above are the
# runtimes there are, and each is built when its family is (make TARGETS=).
FAMILY_RT_arm := thumbv6m-none-eabi thumbv8m.base-none-eabi \
                 thumbv7m-none-eabi thumbv7em-none-eabi thumbv7em-none-eabihf \
                 thumbv8m.main-none-eabi thumbv8m.main-none-eabihf \
                 armv7a-none-eabi armv7a-none-eabihf
FAMILY_RT_riscv := riscv32-unknown-elf riscv64-unknown-elf $(RISCV_HF)
FAMILY_RT_avr := avr avr/avr6
FAMILY_RT_mips := mipsel-none-elf mips-none-elf mips64el-none-elf \
                  mips64-none-elf
FAMILY_RT_loongarch := loongarch64-unknown-elf
FAMILY_RT_tricore := tricore-none-elf
FAMILY_RT_xtensa := xtensa-none-elf
FAMILY_RT_ppc := powerpc-none-eabi
FAMILY_RT_rx := rx-none-elf
FAMILY_RT_sparc := sparc-none-elf
FAMILY_RT_m68k := m68k-none-elf
RT_TRIPLES := $(foreach f,$(TARGETS_BUILT),$(FAMILY_RT_$(f)))
# The triples of a list that no family claims: a triple added to a list
# and not to its family would be built by no selection, so it stops make.
rt_strays = $(filter-out $(foreach f,$(TARGET_FAMILIES),$(FAMILY_RT_$(f))),$(1))
ifneq ($(call rt_strays,$(RT_EMBEDDED_ALL)),)
$(error RT_EMBEDDED_ALL: $(call rt_strays,$(RT_EMBEDDED_ALL)) is in no FAMILY_RT_ list)
endif
RT_EMBEDDED := $(filter $(RT_TRIPLES),$(RT_EMBEDDED_ALL))
rt-embedded: embcc embar
	@for t in $(RT_EMBEDDED); do \
	    sh tools/build-rt.sh $$t $(BUILD)/libc/$$t || exit 1; \
	    echo "rt: $(BUILD)/libc/$$t/librt.a"; \
	done

# lib/libc for the embedded targets, on the bare-metal backend
# (lib/libc/os/baremetal), beside each one's librt.a. tools/build-libc.sh is
# the recipe, and tests/golden/libc-embedded.sh runs what it builds on the
# boards. Not avr: lib/libc's formatter and stream buffers alone are more
# than an ATmega328P's flash and SRAM. AVR's library is lib/avr's (the string
# and _P functions, sprintf and its _P forms, the EEPROM, setjmp), which rt-embedded
# builds into build/libc/avr/libc.a beside the startup.
LIBC_EMBEDDED_ALL := thumbv6m-none-eabi thumbv8m.base-none-eabi thumbv7m-none-eabi thumbv7em-none-eabi \
                 thumbv7em-none-eabihf thumbv8m.main-none-eabi \
                 thumbv8m.main-none-eabihf armv7a-none-eabi armv7a-none-eabihf \
                 riscv32-unknown-elf riscv64-unknown-elf $(RISCV_HF) \
                 mipsel-none-elf mips-none-elf \
                 loongarch64-unknown-elf tricore-none-elf \
                 xtensa-none-elf powerpc-none-eabi rx-none-elf sparc-none-elf m68k-none-elf \
                 mips64el-none-elf mips64-none-elf
ifneq ($(call rt_strays,$(LIBC_EMBEDDED_ALL)),)
$(error LIBC_EMBEDDED_ALL: $(call rt_strays,$(LIBC_EMBEDDED_ALL)) is in no FAMILY_RT_ list)
endif
LIBC_EMBEDDED := $(filter $(RT_TRIPLES),$(LIBC_EMBEDDED_ALL))
libc-embedded: embcc embar
	@for t in $(LIBC_EMBEDDED); do \
	    sh tools/build-libc.sh $$t $(BUILD)/libc/$$t || exit 1; \
	    echo "libc: $(BUILD)/libc/$$t/libc.a"; \
	done

libc-linux: libc-linux-x86_64 libc-linux-aarch64

# ---- installation ------------------------------------------------------
#
# EmbCC finds its own files relative to the binary (src/driver/paths.c),
# so nothing here is compiled in and the result can be moved afterwards
# or unpacked anywhere. PREFIX is where it will LIVE; DESTDIR is where
# to stage it now, which is what a package build sets and what the
# golden test uses.
#
# The version comes out of src/driver/version.h rather than being
# written again here: the directory this creates and the directory the
# compiler looks in have to be the same one, and two copies of a
# version string are two chances to ship an upgrade that reads the old
# version's headers.
PREFIX  ?= /usr/local
DESTDIR ?=
VERSION := $(shell sed -n 's/.*EMBCC_VERSION "\(.*\)".*/\1/p' \
                   src/driver/version.h)
LIBROOT  = $(DESTDIR)$(PREFIX)/lib/embcc/$(VERSION)

# Each installed target directory, and where the build put its pieces.
# The triples are what --target= accepts, so `ls` next to a failure
# answers "is that target installed?" directly.
# Split in two on purpose. `install` is what a person runs and builds
# what it needs first; `install-files` only COPIES, and is what
# tests/golden/install.sh uses -- a test that ran the first would be a
# test that rebuilds the compiler while the rest of the suite is using
# it, which is the one thing the suite must never do to itself.
# Everything is built first, and only then copied: as two prerequisites of
# one rule, `make -j install` copied while the libraries were still being
# built, and installed a tree with some missing, exiting 0.
install: toolchain
	@$(MAKE) --no-print-directory install-files

# The hosted triples' directories and where the build put them; the
# embedded ones are RT_EMBEDDED's, each in a directory of its own name.
INSTALL_PAIRS_x86 := "x86_64-elf:x86_64" "x86_64-linux-gnu:linux-x86_64" \
                     "x86_64-apple-darwin:darwin-x86_64"
INSTALL_PAIRS_aarch64 := "aarch64-elf:aarch64" "aarch64-linux-gnu:linux-aarch64" \
                         "aarch64-apple-darwin:darwin-aarch64"
install-files:
	@echo "installing EmbCC $(VERSION) into $(DESTDIR)$(PREFIX)"
	@mkdir -p $(DESTDIR)$(PREFIX)/bin
	@for t in embcc embld embas embread embdbg embls embidx embar; do \
	    cp $$t $(DESTDIR)$(PREFIX)/bin/$$t || exit 1; \
	    chmod 755 $(DESTDIR)$(PREFIX)/bin/$$t || exit 1; \
	done
	@for t in $(EXTRA_TOOLS); do \
	    if [ -f $$t ]; then cp $$t $(DESTDIR)$(PREFIX)/bin/$$t; \
	        chmod 755 $(DESTDIR)$(PREFIX)/bin/$$t; fi; \
	done
	@for p in $(BINUTILS_NAMES); do \
	    [ -f $(DESTDIR)$(PREFIX)/bin/$${p#*:} ] && \
	        ln -sf $${p#*:} $(DESTDIR)$(PREFIX)/bin/embcc-$${p%%:*}; \
	    true; \
	done
	@mkdir -p $(DESTDIR)$(PREFIX)/share/embcc/cmake/Compiler
	@cp cmake/embcc.cmake $(DESTDIR)$(PREFIX)/share/embcc/cmake/embcc.cmake
	@cp cmake/Compiler/*.cmake $(DESTDIR)$(PREFIX)/share/embcc/cmake/Compiler/
	@mkdir -p $(DESTDIR)$(PREFIX)/share/doc/embcc
	@cp LICENSE NOTICE $(DESTDIR)$(PREFIX)/share/doc/embcc/
	@mkdir -p $(DESTDIR)$(PREFIX)/share/man/man1
	@SOURCE_DATE_EPOCH=$$(git log -1 --format=%ct 2>/dev/null || date +%s) \
	    ./embcc --reference=man > $(DESTDIR)$(PREFIX)/share/man/man1/embcc.1
	@mkdir -p $(LIBROOT)/include $(LIBROOT)/include/c++ \
	          $(LIBROOT)/freestanding
	@cp -R lib/libc/include/. $(LIBROOT)/include/
	@cp -R lib/libcxx/include/. $(LIBROOT)/include/c++/
	@cp -R include/. $(LIBROOT)/freestanding/
	@for pair in $(foreach f,$(TARGETS_BUILT),$(INSTALL_PAIRS_$(f))) \
	             $(foreach t,$(RT_EMBEDDED),"$(t):$(t)"); do \
	    triple=$${pair%%:*}; dir=$${pair#*:}; \
	    mkdir -p $(LIBROOT)/$$triple; \
	    for f in libc.a librt.a libgcov.a libasan.a libtsan.a libmsan.a crt1.o; do \
	        [ -f $(BUILD)/libc/$$dir/$$f ] && \
	            cp $(BUILD)/libc/$$dir/$$f $(LIBROOT)/$$triple/$$f; \
	    done; \
	    [ -f $(BUILD)/libcxx/$$dir/libcxx.a ] && \
	        cp $(BUILD)/libcxx/$$dir/libcxx.a $(LIBROOT)/$$triple/libcxx.a; \
	    for f in $(BUILD)/libc/$$dir/board-*.o $(BUILD)/libc/$$dir/esp32-*.o \
	             $(BUILD)/libc/$$dir/esp32.ld; do \
	        [ -f $$f ] && cp $$f $(LIBROOT)/$$triple/; done; \
	    case $$triple in *-linux-gnu) \
	        cp lib/libc/os/linux/link.ld $(LIBROOT)/$$triple/link.ld ;; \
	    avr|avr/avr6) for f in $(BUILD)/libc/$$dir/crt*.o $(BUILD)/libc/$$dir/*.ld; do \
	        [ -f $$f ] && cp $$f $(LIBROOT)/$$triple/; done ;; \
	    esac; \
	    true; \
	done
	@echo "installed: $(DESTDIR)$(PREFIX)/bin/embcc"
	@echo "           $(DESTDIR)$(PREFIX)/share/man/man1/embcc.1"
	@echo "           $(LIBROOT)/"
	@echo "check it with: $(DESTDIR)$(PREFIX)/bin/embcc --print-search-dirs"

# The tools beyond the compiler's own set, installed when they were built.
EXTRA_TOOLS := embpack embmap embsim embsvd embflash embtrace embrt

# A GCC cross toolchain's names, for a project that names its compiler
# that way: `make install-gnu TRIPLES="arm-none-eabi riscv64-unknown-elf"`
# links <triple>-gcc and -cc to embcc (which reads the target from its
# name), -ar and -ranlib to embar, -objcopy to embpack, -size and -nm to
# embmap. An existing toolchain file or Makefile then builds with EmbCC
# with only PATH changed. By default the GCC names of the families built
# (TARGETS): a name for a family left out would only be refused.
TRIPLES ?= $(if $(call built,arm),arm-none-eabi) \
           $(if $(call built,riscv),riscv64-unknown-elf riscv32-unknown-elf) \
           $(if $(call built,avr),avr)
install-gnu: install-files
	@for tr in $(TRIPLES); do \
	    for p in gcc:embcc cc:embcc $(BINUTILS_NAMES); do \
	        ln -sf $${p#*:} $(DESTDIR)$(PREFIX)/bin/$$tr-$${p%%:*}; \
	    done; \
	    echo "installed: $(DESTDIR)$(PREFIX)/bin/$$tr-gcc and its binutils"; \
	done

# Removes exactly what install wrote, and the versioned directory with
# it -- never $(PREFIX)/lib/embcc itself, which may hold another version.
uninstall:
	@for t in embcc embld embas embread embdbg embls embidx embar \
	          $(EXTRA_TOOLS); do \
	    rm -f $(DESTDIR)$(PREFIX)/bin/$$t; \
	done
	@for p in $(BINUTILS_NAMES); do \
	    rm -f $(DESTDIR)$(PREFIX)/bin/embcc-$${p%%:*}; \
	done
	@rm -f $(DESTDIR)$(PREFIX)/share/embcc/cmake/embcc.cmake
	@rm -f $(DESTDIR)$(PREFIX)/share/embcc/cmake/Compiler/EmbCC*.cmake
	@rm -f $(DESTDIR)$(PREFIX)/share/doc/embcc/LICENSE \
	       $(DESTDIR)$(PREFIX)/share/doc/embcc/NOTICE
	@rm -rf $(LIBROOT)
	@echo "removed EmbCC $(VERSION) from $(DESTDIR)$(PREFIX)"

libc-linux-all: libc-linux libcxx-linux-x86_64 libcxx-linux-aarch64

libc: libc-x86_64 libc-aarch64

# ---- our C++ runtime (lib/libcxx) --------------------------------------
# The Itanium C++ ABI on top of libgcc's unwinder: operator new, static-
# local guards, the type_info hierarchy, dynamic_cast, and the personality
# routine. This is what libsupc++ is; the standard library sits above it.
LIBCXX_SRCS := $(wildcard lib/libcxx/src/*.cc)
LIBCXX_INC  := -Ilib/libcxx/include -Ilib/libc/include

libcxx-x86_64: embcc embar
	@mkdir -p $(BUILD)/libcxx/x86_64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/x86_64/$$(basename $$f .cc).o; \
	    ./embcc -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/x86_64/libcxx.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libcxx/x86_64/libcxx.a \
	    $(BUILD)/libcxx/x86_64/*.o
	@echo "libcxx: $(BUILD)/libcxx/x86_64/libcxx.a"

libcxx-aarch64: embcc embar
	@mkdir -p $(BUILD)/libcxx/aarch64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/aarch64/$$(basename $$f .cc).o; \
	    ./embcc -c -O2 -x c++ --target=aarch64-elf $(LIBCXX_INC) $$f -o $$o \
	        || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/aarch64/libcxx.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs \
	    $(BUILD)/libcxx/aarch64/libcxx.a $(BUILD)/libcxx/aarch64/*.o
	@echo "libcxx: $(BUILD)/libcxx/aarch64/libcxx.a"

# The same runtime for the Linux triples, so std::thread, std::mutex and
# the exception machinery can be exercised on a real kernel rather than
# only on the freestanding harness.
libcxx-linux-x86_64: embcc embar
	@mkdir -p $(BUILD)/libcxx/linux-x86_64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/linux-x86_64/$$(basename $$f .cc).o; \
	    ./embcc --target=x86_64-linux-gnu -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/linux-x86_64/libcxx.a
	@$${EMBCC_X86_AR:-./embar} rcs $(BUILD)/libcxx/linux-x86_64/libcxx.a \
	    $(BUILD)/libcxx/linux-x86_64/*.o
	@echo "libcxx: $(BUILD)/libcxx/linux-x86_64/libcxx.a"

libcxx-linux-aarch64: embcc embar
	@mkdir -p $(BUILD)/libcxx/linux-aarch64
	@for f in $(LIBCXX_SRCS); do \
	    o=$(BUILD)/libcxx/linux-aarch64/$$(basename $$f .cc).o; \
	    ./embcc --target=aarch64-linux-gnu -c -O2 -x c++ $(LIBCXX_INC) $$f -o $$o || exit 1; \
	done
	@rm -f $(BUILD)/libcxx/linux-aarch64/libcxx.a
	@$${EMBCC_AARCH64_AR:-./embar} rcs $(BUILD)/libcxx/linux-aarch64/libcxx.a \
	    $(BUILD)/libcxx/linux-aarch64/*.o
	@echo "libcxx: $(BUILD)/libcxx/linux-aarch64/libcxx.a"

libcxx: libcxx-x86_64 libcxx-aarch64

# The C++ runtime for the 32-bit embedded targets C++ is supported on
# (32-bit ARM, RV32): operator new/delete, the guards, __aeabi_atexit --
# the -fno-exceptions -fno-rtti subset, tools/build-libcxx.sh. Into
# build/libcxx/<triple>/, where the driver looks for it beside
# build/libc/<triple>/libc.a and librt.a.
LIBCXX_EMBEDDED_ALL := thumbv6m-none-eabi thumbv7m-none-eabi thumbv7em-none-eabi \
                   thumbv7em-none-eabihf thumbv8m.main-none-eabi \
                   thumbv8m.main-none-eabihf armv7a-none-eabi \
                   armv7a-none-eabihf riscv32-unknown-elf \
                   riscv32-unknown-elf/ilp32f riscv32-unknown-elf/ilp32d \
                   mipsel-none-elf mips-none-elf mips64-none-elf \
                   powerpc-none-eabi sparc-none-elf m68k-none-elf \
                   xtensa-none-elf tricore-none-elf rx-none-elf
ifneq ($(call rt_strays,$(LIBCXX_EMBEDDED_ALL)),)
$(error LIBCXX_EMBEDDED_ALL: $(call rt_strays,$(LIBCXX_EMBEDDED_ALL)) is in no FAMILY_RT_ list)
endif
LIBCXX_EMBEDDED := $(filter $(RT_TRIPLES),$(LIBCXX_EMBEDDED_ALL))
libcxx-embedded: embcc embar
	@for t in $(LIBCXX_EMBEDDED); do \
	    sh tools/build-libcxx.sh $$t $(BUILD)/libcxx/$$t || exit 1; \
	    echo "libcxx: $(BUILD)/libcxx/$$t/libcxx.a"; \
	done

clean:
	rm -rf $(BUILD) embcc embread embld embdbg embas embls embidx embar embsvd embdoc

# ...and what ./configure wrote.
distclean: clean
	rm -f config.mk

.PHONY: all check test test-arm64 test-libstdcxx site libc libc-x86_64 libc-aarch64 \
        libc-emblinkos libcxx-emblinkos libc-linux libc-linux-x86_64 libc-linux-aarch64 \
        libcxx libcxx-x86_64 libcxx-aarch64 \
        libcxx-linux-x86_64 libcxx-linux-aarch64 libcxx-embedded \
        install install-files uninstall libc-linux-all clean distclean
