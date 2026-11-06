# embsim — run a firmware image without a board

`embsim` runs a linked ELF image, or an Intel HEX one, the way the part
does: a Cortex-M (ARMv6-M, ARMv7-M), a RISC-V core (RV32 and RV64,
IMAFDC), an AVR (the ATmega328P), a MIPS32 Release 2 core (QEMU malta's
24Kc, a PIC32MZ's), a PIC24F (the PIC24FJ1024GB610) or a dsPIC33E with
its DSP engine (the dsPIC33EP512MU810). It is ISO C
with no dependencies, so it builds on any machine, including one
without QEMU. It counts the instructions it executes and estimates the
cycles they take, and gdb, lldb or embdbg can debug the image while it
runs. Given the part's CMSIS-SVD file, it puts every peripheral register
the file describes on the bus, and traces a driver's accesses to them by
name. This page is the command reference; how EmbSim is built inside,
and how to add a core, a peripheral or a board, is
[EmbSim's internals](../../internals/embsim.md).

## Synopsis

```text
embsim IMAGE.elf|IMAGE.hex [--board NAME] [--cpu NAME] [--ram-size SIZE]
                 [--until STRING] [--max-insns N] [--stats]
                 [--count FILE] [--trace FILE] [--no-semihosting]
                 [--gdb [HOST:]PORT [--gdb-wait]]
                 [--svd FILE.svd] [--trace-periph[=NAME,...]]
                 [--coverage FILE [--coverage-format=text|lcov]]
                 [--profile[=FILE] [--profile-format=report|collapsed|collapsed-insns]]
                 [--stack-report[=FILE]] [--stack-limit ADDR|SYMBOL]
                 [--stack-su FILE.su]... [--stack-embrt FILE.json] [FILE.su...]
                 [--fault-report[=FILE]] [--input FILE|-] [--record FILE]
                 [--dfp DIR] [--set NAME=VALUE]... [--disassemble[=FILE]]
embsim [IMAGE.elf] --replay FILE [--count FILE] [--stats] [...]
embsim --svd FILE.svd --svd-map
```

```sh
embcc --target=thumbv7em-none-eabihf --opt=speed -c main.c
embld -e reset -Ttext 0 -Tdata 0x20000000 boot.o main.o -o fw.elf
embsim fw.elf --board mps2-an386 --stats

embcc --target=riscv64-unknown-elf --isa=rv64gc --abi=lp64d --opt=speed -c main.c
embld -e _start -Ttext 0x80000000 -Tstack 0x80800000 boot.o io.o main.o -o fw.elf
embsim fw.elf --board virt --stats

embcc --target=avr --opt=size -c main.c
embld -e __vectors -Ttext 0 -Tdata 0x100 boot.o io.o main.o librt.a -o fw.elf
embsim fw.elf --board uno --stats

embsvd STM32F405.svd --header STM32F405.h --startup startup.c \
    --ld f405.ld --flash 0x08000000:1M --ram 0x20000000:128K
embcc --target=thumbv7em-none-eabi --opt=speed -c startup.c main.c
embcc --target=thumbv7em-none-eabi --link-script=f405.ld startup.o main.o -o fw.elf
embsim fw.elf --svd STM32F405.svd --trace-periph=USART2,RCC

embcc --target=mipsel-none-elf --mcu=32MZ2048EFH100 --dfp=$DFP *.c *.S -o fw.elf
embsim fw.elf --board explorer16-32 --dfp $DFP --set temperature=30

embsim thermo.hex --board explorer16-32-pic24 --dfp $DFP24 --set S3=pressed
```

The image is an ELF file (its PT_LOAD segments), or an Intel HEX file
(MPLAB's production `.hex`, an objcopy `-O ihex`), whose records are
placed at their addresses on EmbSim's bus.

## The boards

Each board has the memory and UART of QEMU's model of it, so an image
built for QEMU runs unchanged.

| Board | Core | Memory | UART |
|---|---|---|---|
| `lm3s6965evb` (default) | Cortex-M3 | 256 KiB flash at 0, 64 KiB SRAM at `0x20000000` | PL011 at `0x4000C000` |
| `mps2-an385` | Cortex-M3 | 4 MiB at 0, 4 MiB at `0x20000000`, 16 MiB at `0x60000000` | CMSDK at `0x40004000` |
| `mps2-an386` | Cortex-M4 with FPv4-SP | as `mps2-an385` | CMSDK |
| `mps2-an500` | Cortex-M7 with FPv5 (double) | as `mps2-an385` | CMSDK |
| `microbit` | Cortex-M0 | 256 KiB flash at 0, 16 KiB SRAM at `0x20000000` | nRF51 at `0x40002000` |
| `stm32f405` | Cortex-M4 with FPv4-SP | 1 MiB flash at `0x08000000` (and at 0), 128 KiB SRAM at `0x20000000`, 64 KiB CCM at `0x10000000` | the USARTs of its SVD ([below](#the-stm32f405)) |
| `bluepill` | STM32F103C8: Cortex-M3 | 64 KiB flash at `0x08000000` (and at 0), 20 KiB SRAM at `0x20000000` | the USARTs of ST's SVD ([below](#the-blue-pill-stm32f103c8)), and the board's LED |
| `nrf52840dk` | nRF52840: Cortex-M4 with FPv4-SP, 64 MHz | 1 MiB flash at 0, 256 KiB RAM at `0x20000000` | UART0, UARTE0-1 of Nordic's SVD ([below](#the-nrf52840-dk)), and the DK's LEDs and buttons |
| `arduino-zero` | the Arduino Zero's ATSAMD21G18A: a Cortex-M0+, 1 MHz out of reset | 256 KiB flash at 0, 32 KiB SRAM at `0x20000000` | the SERCOMs as USARTs, of Microchip's SVD ([below](#the-arduino-zero-atsamd21g18a)), and the board's LEDs |
| `pico` | the Raspberry Pi Pico's RP2040: core 0, a Cortex-M0+ | its ROM at 0, 2 MiB flash at `0x10000000` (boot2, then the image), 264 KiB SRAM at `0x20000000` | UART0 and UART1 of RP2040.svd ([below](#the-raspberry-pi-pico)), and the board's LED |
| `esp32c3-devkitm-1` | the ESP32-C3-DevKitM-1's ESP32-C3: RV32IMC | 400 KiB SRAM: DRAM at `0x3FC80000`, IRAM at `0x4037C000`; 8 KiB RTC fast memory at `0x50000000` | UART0 and UART1 of Espressif's SVD ([below](#the-esp32-c3-devkitm-1)), and the board's RGB LED and BOOT button |
| `virt` | RISC-V, RV32 or RV64 with IMAFDC | 128 MiB RAM at `0x80000000` | NS16550A at `0x10000000` |
| `uno` | ATmega328P (AVR5) | 32 KiB flash, 2 KiB SRAM at data `0x100` | USART0 at data `0xC0` |
| `malta` | MIPS32r2 (24Kc), little-endian | 64 MiB RAM at 0 (`0x80000000` in kseg0) | NS16550A at `0x1F000900`, registers 8 bytes apart |
| `pic32mz2048efh100` | PIC32MZ EF (MIPS32r2, EIC mode, shadow sets) | 512 KiB RAM at 0, 2 MiB flash at `0x1D000000`, boot flash at `0x1FC00000` | UART1-6, of the part's ATDF ([below](#pic32-parts-the-explorer-1632-and-the-curiosity)) |
| `explorer16-32` | the same part, on Microchip's Explorer 16/32 | as `pic32mz2048efh100` | the same, and the board's LEDs, buttons, sensors and LCD |
| `curiosity-pic32mz-ef-2` | PIC32MZ2048EFM144, on Microchip's Curiosity PIC32MZ EF 2.0 | as `pic32mz2048efh100` | the same, and the board's LEDs and switches |
| `explorer16-32-pic32mx` | PIC32MX795F512L (the M4K: MIPS32r2, MIPS16e, EIC mode, one shadow set), on the Explorer 16/32 with its PIM | 128 KiB RAM at 0, 512 KiB flash at `0x1D000000`, 12 KiB boot flash at `0x1FC00000` | UART1-6, of the part's EDC ([below](#pic32mx-parts-and-the-explorer-1632s-pic32mx-pim)), and the board's LEDs, buttons, sensors, LCD and EEPROM |
| `pic24fj1024gb610` | PIC24FJ1024GB610 (the PIC24F core, with EDS) | 1 MiB of program flash, 32 KiB of RAM at data `0x0800` | UART1-6, of the part's EDC ([below](#pic24-parts-and-the-explorer-1632s-pic24-pim)) |
| `explorer16-32-pic24` | the same part, on the Explorer 16/32 with its PIC24FJ1024GB610 PIM | as `pic24fj1024gb610` | the same, and the board's LEDs, buttons, sensors, LCD and EEPROM |
| any PIC32MZ EF part by its name: `pic32mz1024efg064`, `pic32mz0512efe100`... | that part, alone | its program flash and RAM as its ATDF gives them | as `pic32mz2048efh100`, of its own ATDF |
| any PIC32MX part by its name: `pic32mx795f512l`, `pic32mx250f128b`... | that part, alone | its program flash, boot flash and RAM as its EDC gives them | as `explorer16-32-pic32mx`'s part, of its own EDC |
| any PIC24FJ GA6/GB6 part by its name: `pic24fj256ga610`, `pic24fj128gb606`... | that part, alone | its program memory, configuration words and RAM as its EDC gives them | as `pic24fj1024gb610`, of its own EDC |
| `explorer16-32-dspic33` | dsPIC33EP512MU810 (the dsPIC33E core with its DSP engine), on the Explorer 16/32 with its PIM | 512 KiB of program flash, 52 KiB of RAM at data `0x1000` (Y space from `0x9000`) | UART1-4, of the part's EDC ([below](#dspic33ep-parts)), and the board's LEDs, buttons, sensors, LCD and EEPROM |
| any dsPIC33EP part by its name: `dspic33ep512mu810`, `dspic33ep64mc202`, `dspic33ep256gp506`... | that part, alone: the dsPIC33E core (the PIC24E's instructions, the DSP engine) | its program memory, configuration words, RAM and Y data space as its EDC gives them | UART1-4, of its EDC ([below](#dspic33ep-parts)) |
| any PIC24EP part by its name: `pic24ep512gu810`, `pic24ep64gp202`... | that part, alone: the PIC24E core (the dsPIC33E's, without the DSP engine) | as a dsPIC33EP's | as a dsPIC33EP's |

A part by its name needs its pack, from `--dfp DIR` (or its file in
`EMBSIM_SVD_PATH`); every part of the four packs' families is one, with
the models the PIC32MZ2048EFH100, the PIC32MX795F512L, the
PIC24FJ1024GB610 and the dsPIC33EP512MU810 have. A part of another
family (`pic24fj128ga010`, a dsPIC33F or dsPIC33C) is refused by name.
A program built for the part (`embcc --mcu=32MZ1024EFG064`) finds its
memory where the part has it; a 64-pin part has the ports and pins of
its EDC or ATDF, so a test that drives RA0 needs a 100-pin one.

- `--cpu` runs another core on the board: `cortex-m0`, `cortex-m0plus`,
  `cortex-m3`, `cortex-m4` or `cortex-m7` on the Cortex-M boards (the
  Pico's is `cortex-m0plus`); `rv32`
  or `rv64` on `virt`, which otherwise takes the image's width (an
  ELFCLASS64 image is RV64); `atmega328p` on `uno`; `24kc` on `malta`;
  `pic32mz` on the PIC32MZ boards, `pic32mx` on the PIC32MX ones;
  `pic24fj1024gb610` (or the part's own name) on the PIC24 ones.
- `--ram-size` changes the size of the SRAM at `0x20000000`, or of
  virt's RAM (the harness's QEMU runs give it `-m 8`, so
  `--ram-size 8M` is the same machine).
- virt also has QEMU's reset ROM at `0x1000`, the SiFive test device at
  `0x100000` that ends a run, and the CLINT at `0x2000000`; the PLIC's
  space reads as zero and ignores writes.
- On `uno` the AVR's data space is at `0x800000` on EmbSim's bus, as gdb
  numbers it: the registers at `0x800000`, the I/O registers from
  `0x800020`, the SRAM from `0x800100` (`--ram-size` resizes it), and
  flash at 0. Timer/Counter1 is there too, and the other I/O registers
  keep what is written.
- On `malta` the core starts at `0xBFC00000`, in a boot ROM that jumps
  to the image's entry, as QEMU's `-kernel` does; SOFTRES (`0x1F000500`)
  written with `0x42` resets the board, which ends the run, as QEMU's
  `-no-reboot` does. A MIPS image's segments are loaded at the physical
  address of their kseg0 or kseg1 address.
- On the PIC24 boards EmbSim's bus holds both of the part's spaces:
  program memory at twice its address (each 24-bit word the four bytes
  an XC16 image and its HEX file give it: its three and a phantom 0),
  the configuration memory at twice `0x800000` (a dsPIC33EP's and a
  PIC24EP's at twice `0xF80000`, where its configuration words are not
  in program memory's last page), and the data space at `0x2000000`
  plus its Extended Data Space address. An ELF segment goes to twice
  its load address; a HEX record to its address as written.
- A segment of the image outside the board's memory gets memory of its
  own, so a link script for a similar part still runs.
- Flash on the lm3s6965, the micro:bit, the STM32F405, the Blue Pill,
  the nRF52840 DK and the Pico ignores stores, as flash does. On the MPS2 boards it is SSRAM, which takes
  them.

## What it models: the Cortex-M

- **The instruction sets.**
  - ARMv6-M.
  - ARMv7-M, with the DSP multiplies and saturation of ARMv7E-M.
  - The floating-point unit: FPv4-SP, and FPv5's double precision.
    Arithmetic follows the architecture's NaN rules and FPSCR's DN and
    FZ bits, not the host's.
  - An instruction the core does not have is a UsageFault (a HardFault
    on ARMv6-M), as on the part.
- **Reset.** The core fetches the stack pointer and the reset vector from
  the vector table at address 0. The ELF entry point is not used.
- **Exceptions.**
  - Entry stacks the frame, the FPU's extended frame when CONTROL.FPCA is
    set, and keeps the 8-byte alignment that xPSR bit 9 records.
  - EXC_RETURN chooses the stack and the frame to return with.
  - Priorities, with AIRCR's grouping, PRIMASK, BASEPRI and FAULTMASK.
  - SVC, PendSV, SysTick, and the NVIC's interrupts. Pend one through
    ISPR, STIR or ICSR.
  - Faults: undefined instructions, invalid state, unaligned accesses
    (and CCR.UNALIGN_TRP), divide by zero (CCR.DIV_0_TRP), bus errors,
    and a disabled coprocessor (CPACR). Each is recorded in CFSR, HFSR
    and BFAR.
  - A fault whose handler is disabled or cannot preempt escalates to
    HardFault. One that HardFault cannot take locks the core up.
- **Thread mode.** Code can run on the PSP, and unprivileged.
- **Exclusives.** LDREX and STREX keep a local monitor.
- **Bit-banding.** The bit-band aliases of SRAM and of the peripherals,
  on the boards that have them.
- **Time.** SysTick and DWT's CYCCNT advance by the estimated cycles,
  so a timed run gives the same answer every time.
- **Other peripherals.** Apart from the UART, the system control space
  and DWT, the peripheral space reads as zero and ignores writes --
  unless an SVD file describes it (`--svd`, below).

## Peripherals from an SVD file: `--svd`

`--svd FILE.svd` reads the part's CMSIS-SVD file -- the one its vendor
publishes, and embsvd writes the device header from -- and puts each of
its peripherals' registers on the bus at the address the file gives.
A register then behaves as the file says its fields do:

| The file says | A read | A write |
|---|---|---|
| `resetValue` | the value until written, and again after a reset | |
| `read-write` (or nothing) | the value | sets the field |
| `read-only` | the value | ignored |
| `write-only` | 0 | sets the field (a model may use it) |
| `writeOnce` | 0 | the first write after reset sets it; the rest are ignored |
| `read-writeOnce` | the value | the first write after reset sets it |
| `modifiedWriteValues` `oneToClear`, `oneToSet`, `oneToToggle` | | each 1 written clears, sets or toggles its bit; a 0 leaves it |
| `zeroToClear`, `zeroToSet`, `zeroToToggle` | | the same for each 0 written |
| `clear`, `set` | | any write clears or sets the whole field |
| `readAction` `clear`, `set` | the value, and then the field is cleared or set | |
| bits no field covers | their reset value | ignored |

- A byte or halfword access reads or writes those bytes alone; a word
  access that spans two narrow registers reaches both.
- Clusters, register arrays (`dim`, `dimIncrement`, `dimIndex`) and
  `derivedFrom` -- of a register, a cluster, a field or a peripheral --
  are placed as embsvd places them in its header: EmbSim reads the file
  with embsvd's reader.
- **An access where no register is is a bus fault** -- a hole in a
  peripheral, or the space between two -- and stderr says where, since a
  HardFault handler rarely does:

  ```text
  embsim: bus fault: a word read at 0x40004440 (pc 0x08000398): USART2 has no register at +0x040
  embsim: bus fault: a word write at 0x40008000 (pc 0x080003b2): no peripheral of STM32F405 is there
  ```

  The space covered is the file's peripherals', from the lowest register
  to the highest; outside it, the board's map answers as without
  `--svd`. On the part, a hole inside a peripheral's block usually reads
  as 0 instead: the fault is EmbSim's rule, to catch a wrong offset.
- Two registers at one address -- an `alternateRegister`, or Nordic's
  peripherals that share their space (SPI0, SPIM0, TWI0...) -- are one
  register, named by the first in the file.
- The core's own peripherals (NVIC, SCB, SysTick, the MPU: anything at
  `0xE0000000` and up) are the core's, not the file's.
- A debugger's reads (`x` in gdb) have no side effect: no `readAction`,
  no trace.
- A peripheral with no model is that alone: its registers keep what is
  written, as the file allows, and nothing else happens. The models of
  [the STM32F405](#the-stm32f405) are what make a peripheral do
  something.

`--svd` works on any Cortex-M board: the file's peripherals go before
the board's space that reads as zero, and the board's own UART still
answers where it is. A file whose name is a board's (`STM32F405.svd`)
picks that board unless `--board` says otherwise.

`--svd-map` lists the registers EmbSim made from the file, without
running anything, one a line: the peripheral, the address, the size in
bits, the register's name and its path, the reset value and the access;
and, for a register not on the bus, why (`-- is P.R`: another register
at its address; `-- the core's`).

### Tracing a driver: `--trace-periph`

`--trace-periph` prints, on stderr, each access to the peripherals' registers by
the SVD's names; `--trace-periph=NAME,...` only those named, as
`PERIPHERAL` or `PERIPHERAL.REGISTER`, with `*` for any characters
(`--trace-periph=USART*,GPIOA.ODR`). Each line has the instruction's
address and what happened:

```text
periph 080002fc W USART2.CR1.UE 0->1
periph 080002fc W USART2.CR1.TE 0->1
periph 08000276 R USART2.SR 0x000000c0
periph 08000246 W GPIOA.BSRR.BS5=1
periph 08000246 H GPIOA.ODR.ODR5 0->1
periph 08000312 W RCC.CR.HSEON 0->1
periph 08000312 H RCC.CR.HSERDY 0->1
periph 00000a1c W UARTE0.ENABLE.ENABLE 0x0->0x8 (Disabled->Enabled)
periph 08000446 W USART3.BRR 0x00000683 ignored: its clock is off (RCC.APB1ENR.USART3EN)
```

- `W`: the program wrote; a line for each field that changed, or for a
  write-only field, what was written to it (`BS5=1`). A write that
  changed nothing says so (`(no change)`, `ignored: read-only`).
- `R`: the program read; the register's value.
- `H`: the hardware changed a field -- a model (a ready bit, ODR from
  BSRR, a timer's flag), or a `readAction` (`(by the read)`).
- A field's value is 0 or 1, or hex when it is wider, with its
  enumerated value's name when the file gives one.

### The STM32F405

The `stm32f405` board is QEMU's netduinoplus2: 1 MiB of flash at
`0x08000000`, seen at 0 too (where the core reads its vector table),
128 KiB of SRAM, 64 KiB of CCM RAM, a Cortex-M4 with FPv4-SP and 4 NVIC
priority bits. Its peripherals are ST's `STM32F405.svd` (in the
cmsis-svd-data project), which EmbSim finds with `--svd FILE` or in the
directories of `EMBSIM_SVD_PATH` (separated by `:`):

```sh
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board stm32f405
```

Over its registers, four models, found by the SVD's names (`RCC`,
`GPIO*`, `USART*` and `UART*`, `TIM*`):

- **RCC.** A ready bit follows its enable: HSIRDY, HSERDY, PLLRDY and
  PLLI2SRDY in CR, LSERDY in BDCR, LSIRDY in CSR are set the moment the
  oscillator or PLL is turned on, and cleared when it is off. CFGR's SWS
  follows SW. **The clock gates:** a peripheral whose enable bit
  (`USART2EN` in `APB1ENR`...) is clear reads as 0 and ignores writes,
  as the part's do, and the first such access is a warning:

  ```text
  embsim: warning: a write at pc 0x08000446: USART3.BRR is ignored while its clock is off (RCC.APB1ENR.USART3EN is 0)
  ```

- **GPIO.** BSRR sets and resets ODR's bits (set wins when both are
  written). IDR is what each pin reads from its configuration, since
  nothing outside drives the pins: an output reads what ODR drives (an
  open-drain one at 1 reads its pull), an input or alternate-function
  pin reads 1 when pulled up, else 0, an analog pin 0. Out of reset
  GPIOA's IDR is `0xa000`: PA13 and PA15, the debug port's, pulled up.
- **USART1-6, UART4-5.** SR resets to `0xc0` (TXE and TC; ST's file says
  `0x00c00000`); a write to DR with CR1's UE and TE set sends the byte
  to stdout at once, so TXE stays set and TC sets; TC, RXNE, LBD and CTS
  are cleared by writing 0; the TXE, TC and RXNE interrupts are raised
  through the NVIC when enabled. Reception is not modelled (the board's
  console for `--input` is the core's boards' UARTs, above).
- **TIM1-14**, their time base: CNT counts up while CEN is set, one
  count per PSC + 1 clocks of the timer's clock; past ARR it returns to
  0 and that is an update event: UIF, and the interrupt with UIE (the
  file's, `TIM1_UP_TIM10` for TIM1). PSC takes effect at the update
  event, ARR too with ARPE; EGR.UG reinitializes the counter and makes
  one (setting UIF unless URS); UDIS and OPM. The timer's clock is the
  core's, or half of it with APB1 at /4 (the APB prescaler in RCC's
  CFGR: x2 when it is not 1). A timer whose clock is gated off does
  not count.
- SysTick is the core's, as on every Cortex-M board.

QEMU's netduinoplus2 is the referee where QEMU models the peripheral,
and the reference manual (RM0090) where it does not:

| | QEMU 11.1 | EmbSim, as RM0090 |
|---|---|---|
| RCC, GPIO | not modelled: they read 0 | modelled, above |
| a peripheral whose clock is off | works anyway | reads 0, ignores writes |
| USART SR written with ones | they are set (`0x3ff`) | rc_w0 flags and read-only ones keep their value |
| TIM EGR.UG | UIF stays 0 | UIF set (URS 0) |
| TIM CNT with CEN clear | keeps counting | holds |
| TIM update interrupt | its alarm is armed by writes to CNT, ARR, PSC or EGR, not CEN; none came in the configurations tried | every update with UIE |
| a hole in a peripheral's block (USART2 + 0x40) | reads 0 | a bus fault (EmbSim's rule) |
| USART SR, BRR and CR1, TC cleared, the TXE interrupt; TIM2's CR1, PSC and ARR, UIF cleared, CNT counting; a reserved address's bus fault | as EmbSim | as QEMU |

Out of reset, the timer and the core see the same clock; EmbSim does not
model the clocks' frequencies themselves -- its time is the core's
cycles, whatever the PLL is set to.

## The Raspberry Pi Pico

No QEMU has an RP2040; EmbSim's `pico` board is the Raspberry Pi Pico:
the RP2040's core 0 (a Cortex-M0+ with two NVIC priority bits), its ROM
at 0, 2 MiB of flash at `0x10000000` (and its XIP aliases at
`0x11000000`, `0x12000000` and `0x13000000`), 264 KiB of SRAM at
`0x20000000`, the XIP cache's 16 KiB as SRAM at `0x15000000` and the USB
controller's 4 KiB of DPRAM at `0x50100000`. Its peripherals are the
Pico SDK's `RP2040.svd` (`src/rp2040/hardware_regs/` in the SDK), found
with `--svd FILE` or in `EMBSIM_SVD_PATH`.

```sh
embsvd RP2040.svd --header RP2040.h --startup startup.c \
    --ld rp2040.ld --flash 0x10000000:2M --ram 0x20000000:264K
embcc --target=thumbv6m-none-eabi --opt=speed -c startup.c main.c
embcc --target=thumbv6m-none-eabi --link-script=rp2040.ld startup.o main.o -o fw.elf
EMBSIM_SVD_PATH=$PICO_SDK_PATH/src/rp2040/hardware_regs embsim fw.elf --board pico
```

**Building for it.** For the RP2040, embsvd's startup holds a boot2 --
the flash's first 256 bytes, which the bootrom runs -- and its linker
script puts it first, the vector table after it at `0x10000100` (the
flash must be given at `0x10000000`). embsvd's boot2 sets the XIP up for
03h reads, which any SPI flash answers, as the SDK's
`boot2_generic_03h` does, and its last word is the CRC-32 the bootrom
checks. An image built with the SDK's own boot2 (`boot2_w25q080`, the
Pico's) boots the same way.

**The boot.** EmbSim does not run the bootrom's code; its ROM does what
the bootrom does to boot from flash (the datasheet's 2.8.1):

- the CRC-32 (polynomial `0x04c11db7`, seed `0xffffffff`, not
  reflected) of the flash's first 252 bytes must be its next word, or
  the run ends at once (status 3), as the part would go on to its USB
  boot:

  ```text
  embsim: the RP2040's bootrom finds no boot2 in flash: the CRC-32 of its first 252 bytes is 0x8ecd5dc8, the word after them 0x0d21ec2c
  embsim: no boot2 in flash: the part would wait for USB (its BOOTSEL mode); 0 instructions, 0 cycles (est.)
  ```

- boot2 is copied to the top 256 bytes of SRAM (`0x20041f00`) and
  entered with `lr` 0, as the bootrom enters it, with the peripherals as
  the bootrom leaves them: PADS_BANK0, IO_QSPI and PADS_QSPI out of
  reset, the QSPI pins' function the XIP's, GPIO26-29's input enables
  off, the rest in reset;
- the ROM's vector table, its header (`'M' 'u'`, version 1, ROM version
  3: a B2) and its table lookup are there, but the lookup finds no
  function: the bootrom's functions -- flash programming, the float
  library, `memcpy`, `reset_usb_boot` -- are not modelled, so an SDK
  program that calls them (`pico_stdlib`'s) does not run.

**The bus.** As on the part (2.1.2 and 2.1.4):

- each peripheral's registers are again at +0x1000, where a write XORs
  its bits into the register, at +0x2000 (sets them) and at +0x3000
  (clears them), as the SDK's `hw_xor_bits`, `hw_set_bits` and
  `hw_clear_bits` use them; a bit the write does not name is not written
  at all. The SIO has none. A read there reads the register;
- a byte or halfword write to a register writes all of it, the data
  repeated across the bus: `strb` of `0xa5` writes `0xa5a5a5a5`;
- a peripheral RESETS holds in reset reads its registers' reset values
  and ignores writes, and the first such access is a warning, as a
  clock-gated STM32 peripheral's is:

  ```text
  embsim: warning: a read at pc 0x100003c2: UART0.UARTCR reads its reset value while it is held in reset (RESETS.RESET.UART0 is 1)
  ```

  The datasheet does not say what such an access does; the registers
  held at their reset values is EmbSim's reading. Out of reset is what
  RESET_DONE says: at once.

**The clocks.** The core runs at clk_sys, which the program sets up, and
the board reports its time at it:

- **ROSC** runs at 6.5 MHz, the datasheet's nominal boot frequency
  (2.17.1; its 2.15.2.1 says "typically 6MHz"); clk_ref and clk_sys
  run from it out of reset;
- **XOSC** is the Pico's 12 MHz crystal. CTRL.ENABLE anything but
  DISABLE (`0xd1e`) enables it -- an invalid value sets BADWRITE, as the
  SDK's `xosc_init` does --, and STATUS.STABLE sets once
  STARTUP.DELAY x 256 of its cycles have passed (x 4 with X4). COUNT
  counts down at 12 MHz;
- **PLL_SYS and PLL_USB** run when PWR's PD and VCOPD are clear, at
  XOSC / REFDIV x FBDIV_INT / (POSTDIV1 x POSTDIV2) once POSTDIVPD is
  clear too, and CS.LOCK sets at once (16 <= FBDIV <= 320);
- **the clock generators**: clk_ref (from the ROSC, its aux mux or the
  XOSC) and clk_sys (clk_ref or its aux mux) switch glitchlessly --
  SELECTED is one-hot of SRC at once --; clk_peri, clk_usb, clk_adc and
  clk_rtc run while CTRL.ENABLE is set; each divides by its DIV (INT and
  FRAC). FC0 measures any of them, done at once: FC0_RESULT in kHz and
  32nds;
- **the watchdog's tick** divides clk_ref by TICK.CYCLES into the
  microsecond the timer counts (the SDK's `watchdog_start_tick`).
  Out of reset CYCLES is 0, which EmbSim takes as no tick: the timer
  does not count until the program starts it. The watchdog's counter,
  loaded by LOAD, goes down by two each tick while CTRL.ENABLE (the
  RP2040-E1 erratum); at zero, or on CTRL.TRIGGER, it resets the chip,
  which ends the run (status 0); SCRATCH0-7 keep what is written.

**The models**:

- **TIMER**: the 64-bit microsecond count (TIMERAWH, TIMERAWL; TIMELR's
  read latches the high half TIMEHR then reads; TIMELW then TIMEHW write
  it); PAUSE. ALARM0-3: a write arms the alarm, and when the count's low
  half reaches it INTR's bit sets, ARMED's clears and, with INTE,
  TIMER_IRQ_n is raised. Writing ARMED disarms.
- **IO_BANK0, PADS_BANK0**: each GPIO's function (CTRL.FUNCSEL) drives
  it -- SIO from GPIO_OUT and GPIO_OE, a UART's TX and RTS high (idle) --,
  through OUTOVER and OEOVER and the pad's OD; the pin is its output,
  else what the board drives, else its pad's pull (both: the bus
  keeper), else 0. Its input (IE) goes to GPIO_IN through INOVER, and to
  the interrupts through IRQOVER: INTR0-3's level bits follow it, their
  edge bits latch its rises and falls until written with 1, and
  PROC0_INTS raises IO_IRQ_BANK0. GPIOn_STATUS shows each stage.
- **SIO**, as core 0 sees it: CPUID 0; GPIO_OUT, GPIO_OE and their
  QSPI-bank twins with SET, CLR and XOR; GPIO_IN and GPIO_HI_IN; the 32
  spinlocks (a read claims, `1 << n`, or 0 when claimed; a write
  releases); the divider -- results at once, CSR.READY after 8 cycles,
  DIRTY until QUOTIENT is read; a divisor of 0, which the datasheet
  leaves undefined, gives a quotient of all ones (-1 signed, 1 for a
  negative dividend) and the dividend as remainder --; the FIFOs, core
  0's side: eight words out (RDY, WOF), nothing ever in (ROE), and
  SIO_IRQ_PROC0.
- **UART0, UART1** (PL011): UARTDR's bytes go to stdout, at once while
  UARTEN and TXE are set and clk_peri runs, else they wait in the FIFO
  (32 with FEN); UART0's receiver is the run's input (`--input`);
  UARTFR from the FIFOs; UARTRIS: TXRIS as a byte goes and the FIFO is
  at its level, RXRIS at the receive level, RTRIS at once below it;
  UARTMIS raises UART0_IRQ or UART1_IRQ. Every UART writes to stdout,
  whatever its pins' function.
- **SSI and the flash**: a W25Q16JV behind the SSI, its contents the
  flash's (the XIP window reads them directly). DR0's frames in TX/RX
  mode are a command and its answer; in EEPROM-read mode the
  instruction, the address (standard, dual or quad framing) and
  CTRLR1.NDF + 1 frames back. A command ends when the program reads SR
  or DR0 after writing, or disables the SSI -- unless IO_QSPI holds the
  chip select low, as the SDK's `flash_do_cmd` does. READ, FAST READ,
  the quad I/O fast read and its continuous mode, the status registers
  (QE 0 at power-up), WRITE ENABLE, JEDEC ID (`ef 40 15`), the unique ID
  (EmbSim's: `456d6253 696d0001`), PAGE PROGRAM and the erases, which
  finish at once.

Where RP2040.svd's reset values are not the part's, the datasheet's are
used: SYSINFO.CHIP_ID `0x20002927` (a B2) and PLATFORM (ASIC), XOSC.CTRL
`0x00d1eaa0`, ROSC.STATUS (running), PSM.DONE (every domain powered),
CHIP_RESET.HAD_POR, the SSI's SR (its FIFOs empty).

**The board.** The LED on GP25, lit high; GP24 is VBUS sense, high (the
board on USB); `--set BOOTSEL=pressed` pulls the QSPI chip select low,
which the program reads with the select let go (IO_QSPI's OEOVER), as
the SDK's `get_bootsel_button` does. The LED's changes are lines on
stderr, at the board's time:

```text
pico: LED on (GP25) at 1.487 ms
pico: LED off (GP25) at 1.987 ms
```

## The Blue Pill (STM32F103C8)

The `bluepill` board is the STM32F103C8 board sold as the "Blue Pill":
a Cortex-M3 with 4 NVIC priority bits, 64 KiB of flash at `0x08000000`
(seen at 0 too, BOOT0 low), 20 KiB of SRAM, the bit-band aliases. Its
peripherals are ST's `STM32F103xx.svd` (the F103 family's, in the
cmsis-svd-data project: it describes the high-density parts' FSMC,
SDIO, TIM5-8, UART4-5 and the rest too, which the C8 does not have; their
registers are there all the same), found with `--svd FILE` or in
`EMBSIM_SVD_PATH`:

```sh
embsvd STM32F103xx.svd --nvic-prio-bits 4 --header STM32F103xx.h \
    --startup startup.c --ld f103.ld --flash 0x08000000:64K --ram 0x20000000:20K
embcc --target=thumbv7m-none-eabi --opt=speed -c startup.c main.c
embcc --target=thumbv7m-none-eabi --link-script=f103.ld startup.o main.o -o fw.elf
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board bluepill
```

The models are [the STM32F405's](#the-stm32f405) with the F1's own:

- **RCC**, as the F405's (ready bits, the clock gates), and the F1's
  rules (RM0008, "Reset and clock control"): a clock source is used once
  it is ready -- CFGR's SWS takes SW's value only when the source's
  ready bit is set, and switches when it becomes so --; PLLRDY sets when
  PLLON is set and the PLL's source (HSI / 2, or HSE, halved with
  PLLXTPRE) is ready. The clocks have frequencies: HSI 8 MHz, HSE the
  board's 8 MHz crystal, the PLL its source times PLLMUL's factor,
  SYSCLK the source in use, HCLK SYSCLK over HPRE's divisor. The core
  runs at HCLK, and the board reports its time at it. A GPIO port's
  clock enable is IOPxEN.
- **GPIOA-G**, the F1's layout (RM0008, "General-purpose and
  alternate-function I/Os"): BSRR sets and resets ODR's bits (set wins),
  BRR resets them; IDR is each pin from its configuration: an analog
  input 0; a floating input what the board drives, else 0 (EmbSim's
  choice); an input with a pull what the board drives, else ODR's bit (1
  up, 0 down); a push-pull output ODR's bit; an open-drain one 0, or let
  go at 1 (floating); an alternate-function output 1 on a USART's TX pin
  (the line idle, as AFIO's MAPR places it), else 0. The debug port's
  pins are its while AFIO's MAPR.SWJ_CFG keeps them -- out of reset all
  five --, whatever CRL and CRH say: PA13 and PA15 pulled up, PA14 down,
  PB4 up (RM0008, "General-purpose I/O"), PB3 0; SWJ_CFG 001 gives PB4
  back, 010 PA15, PB3 and PB4, 100 all five.
- **USART1-3** as the F405's, and USART1's receiver is the run's input
  (`--input`): a byte comes in while UE and RE are set and RXNE is
  clear, and a read of DR takes it.
- **TIM1-4** and SysTick as the F405's; a timer's clock follows HCLK.

The board's LED is on PC13, lit low; it has no user button.

```text
bluepill: LED on (PC13) at 2.543 ms
bluepill: LED off (PC13) at 3.543 ms
```

QEMU's STM32F1 (its stm32vldiscovery, an STM32F100) models the USARTs,
at the F103's addresses, and none of RCC, GPIO, AFIO or the timers (they
read 0); where it models them EmbSim's values are QEMU's, where it does
not RM0008's, and where they differ:

| | QEMU 11.1 (stm32vldiscovery) | EmbSim, as RM0008 |
|---|---|---|
| RCC, GPIO, AFIO, the timers | not modelled: they read 0 | modelled, above |
| a peripheral whose clock is off | works anyway | reads 0, ignores writes |
| USART SR written with ones | they are set (`0x3ff`) | rc_w0 flags and read-only ones keep their value |
| USART1's SR, BRR and CR1, TC cleared, the TXE interrupt; a reserved address's bus fault | as EmbSim | as QEMU |

## The nRF52840 DK

The `nrf52840dk` board is Nordic's nRF52840 DK (PCA10056): the nRF52840,
a Cortex-M4 with FPv4-SP and 3 NVIC priority bits at 64 MHz, 1 MiB of
flash at 0, 256 KiB of RAM at `0x20000000` (and again as code RAM at
`0x00800000`). Its peripherals are Nordic's `nrf52840.svd`, found with
`--svd FILE` or in `EMBSIM_SVD_PATH`:

```sh
embsvd nrf52840.svd --header nrf52840.h --startup startup.c \
    --ld nrf52840.ld --flash 0:1M --ram 0x20000000:256K
embcc --target=thumbv7em-none-eabihf --opt=speed -c startup.c main.c
embcc --target=thumbv7em-none-eabihf --link-script=nrf52840.ld startup.o main.o -o fw.elf
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board nrf52840dk
```

The header names things as Nordic's MDK does (`NRF_UART0`,
`NRF_TIMER1->CC[0]`); a program built for the FPU turns it on first
(CPACR), in `SystemInit` or its startup.

Every peripheral modelled has Nordic's tasks, events and interrupts (the
Product Specification's "Peripheral interface"): writing 1 to a task
register triggers it; an event register is set by the hardware and
cleared by writing 0; INTEN, INTENSET and INTENCLR hold one enable bit
per event (bit n for the event at 0x100 + 4n); the interrupt is raised
while an enabled event is set -- when it sets, and again when a write
leaves it set; SHORTS connect events to tasks at once. The models (the
nRF52840 Product Specification v1.1):

- **CLOCK.** HFCLKSTART starts the 64 MHz crystal: HFCLKSTARTED and
  HFCLKSTAT (Xtal, running) after HFXODEBOUNCE x 16 us (256 us out of
  reset; the crystal's power-up time is not counted). LFCLKSTART starts
  the 32.768 kHz clock from LFCLKSRC's source, copied to LFCLKSRCCOPY:
  LFCLKSTARTED and LFCLKSTAT at once.
- **P0, P1.** OUT, OUTSET, OUTCLR; DIR, DIRSET, DIRCLR, which are
  PIN_CNF[n]'s DIR bits. IN is each pin whose input buffer is connected
  (PIN_CNF.INPUT 0; out of reset none is): an output what OUT drives,
  unless DRIVE lets that level go (D0S1 and D0H1 a 0, S0D1 and H0D1 a 1);
  else what the board drives, else the pull (up 1, down 0), else 0
  (floating, EmbSim's choice).
- **UART0 and UARTE0** share their space and one model, as the part's do;
  ENABLE 4 is the UART, 8 the UARTE. The UART: after STARTTX a byte
  written to TXD is sent and TXDRDY set (not before); after STARTRX the
  run's input (`--input`) fills RXD's six-byte FIFO, RXDRDY a byte. The
  UARTE (and UARTE1): STARTTX sends TXD.MAXCNT bytes from TXD.PTR
  (TXSTARTED, TXDRDY each, ENDTX, TXD.AMOUNT); STARTRX puts the input at
  RXD.PTR (RXSTARTED, RXDRDY each, ENDRX and RXD.AMOUNT at RXD.MAXCNT;
  STOPRX ends the buffer early, RXTO); ENDRX_STARTRX and ENDRX_STOPRX.
  EasyDMA reaches Data RAM alone -- the PS: elsewhere "may result in a
  HardFault or RAM corruption" -- so a buffer in flash is a warning, and
  nothing moves:

  ```text
  embsim: warning: UARTE0's TXD.PTR 0x00000dfc is not in Data RAM: EasyDMA reaches only that (the part may fault or corrupt RAM); nothing is moved
  ```

- **TIMER0-4.** START, STOP, CLEAR, COUNT, CAPTURE[n], SHUTDOWN; in Timer
  mode the counter counts at 16 MHz / 2^PRESCALER, in the Counter modes
  at each COUNT while started; BITMODE's width (16, 8, 24, 32 bits) wraps
  it; COMPARE[n] when it becomes CC[n]; COMPAREn_CLEAR and
  COMPAREn_STOP. TIMER0-2 have four CC registers and TIMER3-4 six, as the
  PS's instance table says (the SVD gives each six: on TIMER0-2 the last
  two keep what is written, and compare with nothing).
- **RTC0-2**, on the LFCLK (stopped, it does not count): the 24-bit
  COUNTER at 32768 Hz / (PRESCALER + 1), PRESCALER latched by START,
  CLEAR and TRIGOVRFLW and read-only while running; TICK each count,
  OVRFLW from 0xFFFFFF to 0, COMPARE[n] as COUNTER becomes CC[n] -- each
  event set only while EVTEN or INTEN enables it; TRIGOVRFLW sets COUNTER
  to 0xFFFFF0. RTC0 has three CC registers, RTC1 and RTC2 four. The PS
  says a CC written with N or N + 1 while COUNTER is N "may not" trigger:
  EmbSim triggers at the transition.
- **FICR**: CODEPAGESIZE 4096, CODESIZE 256, INFO.PART `0x52840`,
  VARIANT "AACA", PACKAGE QI, RAM 256 KiB, FLASH 1 MiB, a DEVICEID of
  EmbSim's; every other word reads `0xFFFFFFFF`, as unprogrammed -- so the
  MDK's errata checks find no revision to work around.

The core runs at 64 MHz, and the board reports its time at it. The
board: LED1-4 on P0.13-16, lit low; BUTTON1-4 on P0.11, P0.12, P0.24,
P0.25, low when pressed (`--set BUTTON1=pressed`), with no pull-up on the
board (a program enables the pin's own); the interface MCU's UART is
whichever UART the program uses (P0.06, P0.08): every UART writes to
stdout.

```text
nrf52840dk: LED1 on (P0.13) at 2.921 ms
nrf52840dk: LED1 off (P0.13) at 3.921 ms
```

QEMU's micro:bit, an nRF51, has the same UART, GPIO and TIMER at the
same addresses, and is the referee where they agree:

| | QEMU 11.1 (micro:bit) | EmbSim, as the nRF52840's PS |
|---|---|---|
| UART0's ENABLE, BAUDRATE | read 0, the reset value | keep what is written |
| a TIMER's COUNT task while stopped | counts | does not count |
| CLOCK, RTC, UARTE, P1, FICR (the nRF52840's) | not there | modelled, above |
| UART0's TXD and TXDRDY; GPIO's DIR, OUT, PIN_CNF, IN with pulls; TIMER0 as a counter (compares, shorts, the 8-bit wrap, CAPTURE), INTENSET and INTENCLR | as EmbSim | as QEMU |

## The Arduino Zero (ATSAMD21G18A)

The `arduino-zero` board is the Arduino Zero: an ATSAMD21G18A, a
Cortex-M0+ with two NVIC priority bits, 256 KiB of flash at 0, 32 KiB of
SRAM at `0x20000000`, and the NVM rows at `0x00800000` (the user row,
the factory calibration, the serial number), which read as erased. The
image is the whole flash, as the EDBG port programs it: the Arduino
bootloader is not there. Its peripherals are Microchip's
`ATSAMD21G18A.svd`, found with `--svd FILE` or in `EMBSIM_SVD_PATH`:

```sh
embsvd ATSAMD21G18A.svd --no-cmsis --header samd21.h --startup startup.c \
    --ld samd21.ld --flash 0:256K --ram 0x20000000:32K
embcc --target=thumbv6m-none-eabi --opt=size -c startup.c main.c
embcc --target=thumbv6m-none-eabi --link-script=samd21.ld startup.o main.o -o fw.elf
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board arduino-zero
```

The SVD gives each SERCOM its four modes, and each TC its three, as
unions over the same registers (`SERCOM5->USART.DATA`,
`TC3->COUNT16.CC0`); the SERCOM and TC models keep the registers
themselves, as the mode lays them out. The models (the datasheet,
Atmel-42181G):

- **SYSCTRL**: OSC8M at 8 MHz / 2^PRESC (1 MHz out of reset);
  OSCULP32K; OSC32K and XOSC32K (the Zero's crystal), ready after their
  STARTUP times (tables 16-10, 16-9: STARTUP 2 is 2048 + 3 cycles), with
  a 32 kHz output under EN32K; the DFLL48M at 48 MHz in open loop, at
  DFLLMUL.MUL times its reference in closed loop, locked (DFLLLCKC,
  DFLLLCKF) 200 us after the reference runs (table 36-50) -- without
  one it does not lock, runs on at 48 MHz with DFLLRCS, and EmbSim
  warns. DFLLRDY is always set. Erratum 9905 is modelled: writing
  DFLLVAL, DFLLMUL or DFLLSYNC while DFLLCTRL.ONDEMAND is set and nothing
  runs on the DFLL freezes the part, which ends the run (status 3).
  INTFLAG has PCLKSR's rises.
- **GCLK**: generators 0-8 and the generic clocks, written whole with
  their ID and read after an 8-bit write of it; reset as tables 14-5,
  14-9 and 14-13 have them (generator 0 on OSC8M, 2 on OSCULP32K,
  GCLK_WDT on generator 2, off). A generator is its source / DIV, or /
  2^(DIV+1) with DIVSEL, with the DIV bits table 14-11 gives it (five on
  generator 2); one changed to a source not running yet stays on its
  old one until it runs; one on itself gives no clock.
- **PM**: generator 0 / 2^CPUDIV is the CPU's clock, and the board
  reports its time at it; CKRDY. The APBA, APBB and APBC masks gate
  their peripherals' registers -- out of reset the SERCOMs and TCs are
  masked -- as on the STM32s:

  ```text
  embsim: warning: a write at pc 0x00000350: SERCOM0.I2CM.CTRLA is ignored while its clock is off (PM.APBCMASK.SERCOM0 is 0)
  ```

- **NVMCTRL**: READY, PARAM (4096 pages of 64 bytes), CTRLB.RWS; a CPU
  clock above what RWS allows at 3.3 V (table 36-39: 24 MHz with no wait
  state, 48 with one) is warned of once:

  ```text
  embsim: warning: the CPU runs at 48.000 MHz with NVMCTRL's CTRLB.RWS 0: at 3.3 V the flash needs a wait state above 24 MHz (table 36-39), and the part would read it wrong
  ```

- **PORT** and its IOBUS at `0x60000000`, one model: DIR, OUT and their
  CLR, SET and TGL, byte and halfword writes in their lanes; PINCFG and
  PMUX, also by WRCONFIG (HWSEL, WRPINCFG, WRPMUX). A pin is driven
  when DIR is set and PMUXEN is not; else the board drives it, or
  PULLEN pulls it OUT's way, or it floats (0). IN shows a pin only with
  INEN (table 22-1).
- **SERCOM0-5 as USARTs** (CTRLA.MODE 0 or 1): CTRLA, CTRLB and BAUD
  are enable-protected; with TXEN, the APB clock and GCLK_SERCOMn_CORE,
  a byte written to DATA goes to stdout at once, with DRE and TXC. The
  first SERCOM whose receiver is enabled takes `--input` into its
  two-level buffer (RXC, DATA), where a byte waits while the buffer is
  full; RXEN cleared empties it. INTENSET, INTENCLR, INTFLAG and the
  interrupt. SYNCBUSY reads 0.
- **TC3-5**: COUNT16, COUNT8 and COUNT32 (TC4, with TC5 its slave:
  STATUS.SLAVE, its COUNT and CCs not reached), counting GCLK_TCx /
  PRESCALER up or down to the top -- the maximum, COUNT8's PER, CC0 in
  MFRQ and MPWM; past a lower top, to the maximum first -- with OVF at
  the wrap and MCx as the counter leaves CCx; ONESHOT, RETRIGGER, STOP,
  STATUS.STOP; CTRLA enable-protected. COUNT reads what the last read
  request (READREQ.RREQ, or RCONT) synchronised, as on the part: without
  one it does not change.

The board (Arduino's variant files for the Zero): the LED L on PA17,
lit high; the TX and RX LEDs on PA27 and PB03, lit low; port 0 is PA, 1
PB. Every SERCOM writes to stdout; the EDBG port is SERCOM5 (PB22,
PB23).

```text
arduino-zero: L on (PA17) at 138.224 ms
arduino-zero: L off (PA17) at 139.224 ms
```

## What it models: the RISC-V core

- **The instruction sets.** RV32I and RV64I, M, A (LR/SC and every
  AMO), F and D, C (every compressed form at each width), Zicsr and
  Zifencei. An instruction outside them -- a reserved encoding, an
  extension EmbCC does not emit (B, V, Zfh), S-mode's SRET and
  SFENCE.VMA -- is an illegal-instruction trap.
- **Floating point.** IEEE arithmetic done in integers, so a result is
  the same on any host: the five rounding modes and the dynamic one,
  the accrued flags in fflags with tininess detected after rounding,
  the canonical NaN, NaN-boxing of singles in the 64-bit registers, and
  FMIN and FMAX as minimumNumber and maximumNumber. The FPU is off at
  reset (mstatus.FS 0), and an F or D instruction then traps, as on
  QEMU; the harness's boot turns it on.
- **Reset.** The core starts at `0x1000`, in QEMU's reset ROM, which
  loads a0 (the hart, 0), a1 (the device tree's address) and a2 (the
  firmware information) and jumps to the start of RAM. The ROM is the
  same, word for word, as QEMU's for the image, so the two run the same
  instructions from the first; the device tree itself is not there.
- **Privilege.** Machine mode, and user mode for an mret to it (machine
  CSRs, mret and an ecall's cause then behave as in U-mode). misa says
  A, C, D, F, I, M and U.
- **Traps.** mtvec, direct and vectored; mepc, mcause, mtval (the
  instruction for an illegal one, the address for an access, the pc
  for ebreak); mstatus's MIE, MPIE and MPP. A trap whose vector cannot
  be fetched is a lockup, which ends the run.
- **Memory.** The bus is 32 bits wide: an RV64 access above 4 GiB, or
  anywhere nothing is, is an access fault. Ordinary loads and stores
  may be misaligned, as on QEMU; LR, SC and the AMOs may not (a
  misaligned-address trap). SC succeeds when its address and the value
  LR read are still there, which is QEMU's rule.
- **Interrupts.** The CLINT's software (msip) and timer (mtimecmp)
  interrupts, through mip and mie. WFI waits for one: it skips ahead to
  mtimecmp when the timer is enabled in mie, and ends the run when
  nothing can wake it.
- **The counters.** mcycle and cycle read the estimated cycles, minstret
  and instret the instructions; time and the CLINT's mtime count with
  mcycle, one tick per cycle (the core is clocked at the device tree's
  10 MHz timebase). All three can be written. On QEMU these follow the
  host's clock instead, so a program that prints them differs there.
- **CSRs.** The machine-mode ones (mstatus, misa, mie, mip, mtvec,
  mscratch, mepc, mcause, mtval, mcounteren, mcountinhibit, menvcfg,
  medeleg, mideleg, the IDs), the PMP registers (kept, not enforced),
  the counters and the event counters (zero), and fflags, frm and fcsr.
  Another CSR, or a write to a read-only one, is an illegal
  instruction.
- **Semihosting** on RV32: an uncompressed `ebreak` between `slli x0,
  x0, 0x1f` and `srai x0, x0, 7` is a call (the RISC-V Semihosting
  specification), the operation in a0 and its parameter in a1, the
  result in a0; the calls are ARM's ([below](#output-and-the-end-of-a-run)).
  Any other ebreak is a breakpoint trap. QEMU answers the same sequence
  with `-semihosting-config enable=on`.

The ESP32-C3's core is this one, as the ESP32-C3 has it
([below](#the-esp32-c3-devkitm-1)).

## The ESP32-C3-DevKitM-1

The `esp32c3-devkitm-1` board is Espressif's ESP32-C3-DevKitM-1: an
ESP32-C3, its RV32IMC core at 20 MHz out of reset, with its 400 KiB of
SRAM as the core reaches it -- SRAM1's 384 KiB as DRAM at `0x3FC80000`
(loads and stores) and again as IRAM at `0x40380000` (fetches), SRAM0's
16 KiB as IRAM at `0x4037C000` -- and the RTC's 8 KiB of fast memory at
`0x50000000`. Its peripherals are Espressif's `esp32c3.svd`, found with
`--svd FILE` or in `EMBSIM_SVD_PATH`.

There is no ROM and no flash: EmbSim does what the ROM's loader does
with an image whose segments are all in SRAM -- puts each where it is
linked, and starts at the ELF's entry. ESP-IDF's ROM, its second-stage
bootloader and the flash cache are out of scope; so is an ESP-IDF
application, which calls the ROM's functions. On the chip, such an image
is made from the ELF by esptool's `elf2image` (EmbCC has no tool for
it) and written at flash offset 0, where the ROM loads it as it loads a
bootloader.

```sh
embsvd esp32c3.svd --no-cmsis --header esp32c3.h --startup startup.c --ld esp32c3.ld
embcc --target=riscv32-unknown-elf --opt=size -c startup.c main.c
embcc --target=riscv32-unknown-elf --link-script=esp32c3.ld startup.o main.o -o fw.elf
EMBSIM_SVD_PATH=$HOME/svd embsim fw.elf --board esp32c3-devkitm-1
```

embsvd's script puts the code in IRAM and the rest in DRAM past it (the
same SRAM, so the two must not overlap), below the ROM loader's own RAM;
its startup's `_start` sets the stack pointer, and its vector table is
the core's: entry 0 for exceptions, entry N for CPU interrupt N, each a
jump to a weak `Exception_Handler` or `CPU_INTN_Handler`. Define those
with `__attribute__((interrupt))`. The ESP32-C3 has no A extension, and
EmbCC needs it ([RISC-V](../embedded.md#risc-v)): code that uses an
atomic -- and lib/libc's stdio, whose lock is LR/SC -- traps there with
an illegal instruction, on the chip as on EmbSim. The ROM leaves the
watchdogs running; a program disables or feeds them first (EmbSim does
not model them).

The core (the RISC-V core [above](#what-it-models-the-risc-v-core), as
the ESP32-C3 Technical Reference Manual v1.4, chapter 1, has it):

- **RV32IMC**, with Zicsr and Zifencei: an AMO, LR, SC, or an F or D
  instruction is an illegal instruction. misa reads `0x40101104`.
- **Its CSRs only**: mvendorid (`0x612`), marchid, mimpid, mhartid;
  mstatus (MIE, MPIE, MPP's low bit, its high bit tied to it, TW);
  mtvec, always vectored, its base 256-aligned; mscratch, mepc, mcause,
  mtval; the PMP registers (kept); the trigger module's (kept); the
  performance counter (mpcer, mpcmr, mpccr: CYCLE counts the cycles but
  those in WFI, INST the instructions, IDLE the cycles in WFI, with
  COUNT_EN and COUNT_SAT; the other events count nothing); the
  dedicated GPIO's cpu_gpio_oen and cpu_gpio_out (kept: the GPIO
  matrix's signals are not modelled) and cpu_gpio_in (0). No mie, mip,
  cycle or time: any other CSR, a debug-mode one, or a write to a
  read-only one is an illegal instruction, mtval the instruction.
- **Interrupts** come from the interrupt matrix: CPU interrupt N is
  taken at mtvec + 4N with mcause N and bit 31, when mstatus.MIE allows.
  WFI waits for one even with MIE off.
- **Time**: a taken branch's cycles pass on the board's time too, so the
  performance counter and the peripherals' clocks agree.

The peripherals modelled (the TRM's chapters):

- **INTERRUPT_CORE0** (1.5, 8): each of the 62 sources' map register
  routes it to a CPU interrupt, 1-31. A level type (CPU_INT_TYPE 0) is
  pending while a source mapped to it is high; an edge type captures
  the rise, and CPU_INT_CLEAR clears it once the core has taken it, or
  while the interrupt is disabled -- not before. CPU_INT_EIP_STATUS
  shows the ones pending, enabled, and with a priority (CPU_INT_PRI_N,
  1-15) not below CPU_INT_THRESH; the core takes the highest priority,
  then the lowest number. INTR_STATUS_REG_0-1 are the sources' levels.
- **SYSTEM**: the CPU clock, from XTAL_CLK (40 MHz / (PRE_DIV_CNT + 1),
  20 MHz out of reset), the PLL (80 or 160 MHz by CPUPERIOD_SEL) or
  RC_FAST_CLK (17.5 MHz / (PRE_DIV_CNT + 1)); the board reports its time
  at it. CPU_INTR_FROM_CPU_0-3 raise sources 50-53, the software's own.
- **SYSTIMER** (10): UNIT0 and UNIT1 count at 16 MHz while enabled
  (UNIT0 out of reset); UPDATE latches a count with VALUE_VALID, LOAD
  loads one. A comparator, from COMPx_LOAD and TARGETx_WORK_EN, alarms
  once in target mode -- at once when the count is past the target by
  less than 2^51 - 1 -- or every period in period mode; INT_RAW, INT_ENA,
  INT_ST, INT_CLR, and the alarm's source.
- **UART0, UART1** (26): a byte written to FIFO is sent at once (to
  stdout), with TX_DONE; TXFIFO_CNT is 0, and TXFIFO_EMPTY raised while
  that is below its threshold. UART0's receiver is the run's input
  (`--input`): RXFIFO_CNT, the FIFO read from FIFO, RXFIFO_FULL raised
  while it holds more than its threshold, RXFIFO_RST. INT_ST and the
  source as INT_RAW & INT_ENA.
- **GPIO and IO_MUX** (5): OUT, ENABLE and their W1TS and W1TC; a pad
  in its GPIO function (MCU_SEL 1, or 0 where function 0 is GPIO) with
  its output enabled and FUNCn_OUT_SEL 128 drives OUT's bit, inverted by
  INV_SEL, open drain by PINn's PAD_DRIVER. IN shows the pads whose
  FUN_IE is set: what is driven, else the board, else FUN_WPU or
  FUN_WPD, else 0 (floating, EmbSim's choice). The pads' reset values
  are table 5.12-1's, pin by pin.
- **RTC_CNTL**: OPTIONS0.SW_SYS_RST resets the chip, which ends the run
  (status 0).

The board (its schematic, V1): the BOOT button on GPIO9, low when
pressed (`--set BOOT=pressed`), with no pull-up of its own (R14 is not
fitted; GPIO9's internal one is on out of reset); and the RGB LED, an
SK68XXMINI-HS on GPIO8. That LED takes 24 bits, green, red and blue,
each a high pulse -- 0.3 us a 0, 0.6 us a 1 (the SK6812MINI's datasheet,
+-0.15 us) -- and shows them after 80 us low. EmbSim decodes GPIO8's
pulses at the board's time (a high under 0.45 us is a 0; one outside
0.15-0.75 us is warned of, once) and reports the colour as its 24th bit
ends:

```text
esp32c3-devkitm-1: RGB LED #201008 (GPIO8) at 6.120 ms
esp32c3-devkitm-1: RGB LED off (GPIO8) at 7.120 ms
```

## What it models: the AVR

- **The instruction set.** The ATmega328P's: the classic AVR core with
  the multiplies (MUL, MULS, MULSU, FMUL, FMULS, FMULSU), MOVW, LPM
  Rd,Z and LPM Rd,Z+, and JMP and CALL; a 16-bit program counter, so a
  call pushes two bytes (big-endian on the stack, as the part does).
  SREG's H, S, V, N, Z and C are the instruction set manual's for every
  instruction, SBC, SBCI and CPC leaving Z set only when it was. An
  instruction the part does not have -- ELPM, EIJMP, EICALL, the
  XMEGA's -- and SPM, which is not modelled, end the run as a lockup
  that names it.
- **Memory.** The register file, SP and SREG in the data space (LD from
  address 30 reads r30); X, Y and Z with pre-decrement, post-increment
  and displacement; program memory read by LPM. The program counter
  wraps at 32 KiB, as the part's does.
- **Reset.** The core starts at 0, the reset vector, with SP at RAMEND
  (0x08FF) and SREG 0 (QEMU's starts with I set; the harness clears
  SREG before it matters).
- **Interrupts.** The vector table at 0, two words a vector; the lowest
  vector requested is taken when I is set, with the return address
  pushed, I cleared and the source's flag cleared; after SEI and after
  RETI one more instruction runs first. USART0's data-register-empty and
  transmit-complete, and Timer/Counter1's capture, compare A and B and
  overflow, are the sources.
- **SLEEP**, when SMCR.SE is set, waits for an interrupt: it skips ahead
  to the device that will request one, and ends the run when nothing
  can (or I is clear). Every sleep mode is treated as Idle. BREAK is a
  NOP, as on a part whose on-chip debugging is off; WDR is a NOP.
- **USART0** sends what is written to UDR0 when the transmitter is on
  (UCSR0B.TXEN0); UDRE0 is always set, and TXC0 is set at once.
- **Timer/Counter1** counts the core's cycles through its prescaler,
  which runs free as the part's does, in every waveform mode: normal,
  CTC with OCR1A or ICR1 as TOP, fast PWM and the dual-slope modes,
  setting TOV1, OCF1A, OCF1B and ICF1 where the datasheet says; a flag
  is cleared by writing it a one, or by entering its vector; the 16-bit
  registers go through TEMP.
- **Cycles are exact.** Each instruction takes the datasheet's cycles
  ("Instruction Set Summary"): a taken branch one more, a skip one more
  over a one-word instruction and two over a two-word one, an
  interrupt's entry four, and four more when it wakes SLEEP. The part
  has no wait states, so this is the count the silicon takes.

## PIC32 parts, the Explorer 16/32 and the Curiosity

QEMU has no PIC32 board; EmbSim's are where EmbCC's PIC32 output runs
(`embcc --mcu=32MZ2048EFH100`, [MIPS32](../embedded.md#mips32)). A PIC32 board is
built from the part's device pack -- Microchip's
`Microchip.PIC32MZ-EF_DFP`, which MPLAB X installs and
packs.download.microchip.com serves -- found with `--dfp DIR` (the
pack's directory, which holds `atdf/PIC32MZ2048EFH100.atdf`), `--svd
FILE.atdf`, or a directory holding the file in `EMBSIM_SVD_PATH`:

```sh
embsim fw.elf --board explorer16-32 --dfp ~/.mchp_packs/Microchip/PIC32MZ-EF_DFP/1.4.168
```

**The registers.** The pack's ATDF file (the "AVR tools device file"
Microchip writes for its PIC32, SAM and AVR parts alike) describes every
register of the part; EmbSim reads it as it reads an SVD file, and each
register behaves as [above](#peripherals-from-an-svd-file---svd): 71
peripherals and 1608 registers on the PIC32MZ2048EFH100, 4230 with their
CLR, SET and INV registers. Two things are a PIC32's:

- Each register the pack's processor module gives CLR, SET and INV
  registers (`xc32/<part>/p<part>.S`, beside `atdf/`) has them at +4,
  +8 and +0xC: a write clears, sets or inverts its bits in the register,
  as one write of the result, so a model sees `T2CONSET = 0x8000` as
  T2CON written with ON set. They read as 0. Every SFR and each of these
  is at the address the pack's device header gives it, but USBCRCON,
  where Microchip's two files disagree (EmbSim follows the ATDF).
- An address in the SFR space where no register is reads as 0 and
  ignores writes, as the part's do: a startup writes OFFn's whole table,
  over the vectors the part does not implement.

Where one ATDF register group names every instance's registers (Timer2
to Timer9 in one, UART1 to UART6 in one), each instance is a peripheral
of its own: `TMR3`, `UART2`. `--svd-map` lists them, `--trace-periph`
traces them by these names.

**The core** is the `pic32mz` model of the [MIPS32
core](#what-it-models-the-mips32-core), a PIC32MZ EF's: PRId 0x0001A700
(the M5150), Config1 with a 16-entry TLB, 16 KiB and 4 KiB 4-way caches
of 16-byte lines (`cache` does nothing), Config3 with EIC and vectored
interrupts, DSP and DSPr2, MIPS32 and microMIPS; seven shadow register
sets (SRSCtl.HSS 7); Status.MX and CU1 writable. The part boots in the
ISA DEVCFG0's BOOTISA bit names (1, as erased flash reads, MIPS32; 0
microMIPS), which is Config3.ISAOnExc's value out of reset too (the
mode exception handlers run in; writable). The
interrupts come from the EVIC in EIC mode. Its FPU is modelled: XC32's
hard-float (FP64) images run as EmbCC's soft-float ones do -- the
Thermometre's XC32 build shows the same temperature on the LCD as its
EmbCC build.

**The models** over the registers:

- **EVIC.** An interrupt is requested while its flag (IFSx), its enable
  (IECx) and a priority (IPCx: IP 1-7) are set; the highest priority
  is presented, then the highest subpriority, then the lowest number.
  The core takes it when its priority is above Status.IPL: at EBASE +
  OFFn of its vector in multi-vector mode (INTCON.MVEC), else EBASE +
  0x200, in the shadow register set PRISS gives its priority; INTSTAT
  says which. The core's timer (Count = Compare) and its two software
  interrupts (Cause.IP0, IP1) set CTIF, CS0IF and CS1IF as they rise.
- **SYSKEY and the oscillator.** Writing 0xAA996655 then 0x556699AA to
  SYSKEY unlocks OSCCON, SPLLCON and the PBxDIV registers; a write to
  one while locked is ignored, as on the part, and the first is a
  warning. OSCCON starts with the clock the configuration words choose
  (DEVCFG1.FNOSC); setting OSWEN switches at once. PB1-PB5 and PB8
  divide by 2 out of reset, PB7 by 1; CLKSTAT says every clock is
  ready. DEVID reads the part's ID and its silicon revision, the
  latest the pack's ATDF lists (0x67213053 on the PIC32MZ2048EFH100:
  revision B2), and ignores writes.
- **Timer1 to Timer9.** Counting while ON, at peripheral bus 3's clock
  (the core's cycles over PB3DIV) over the prescaler; at PRx, back to 0
  and the interrupt flag. Timer2 to Timer9 pair into 32-bit timers
  (T32), with the odd timer's interrupt. PRx is 0xFFFF out of reset.
- **Ports A-K.** All inputs and every analog-capable pin analog out of
  reset (TRISx, ANSELx: the pins the pack's device header names, as the
  data sheet's reset values have them; the ATDF lists all sixteen).
  PORTx reads the pins -- an output what LATx drives, an input what the
  board drives on it or its pull-up (CNPUx), an analog pin 0 -- and a
  write to PORTx writes LATx.
- **UART1-6.** A byte written to UxTXREG with ON and UTXEN goes to
  stdout at once (TRMT stays set); the transmit interrupt is persistent
  while there is room; the run's input (`--input`) arrives at the first
  UART receiving.
- **PMP**, in its master modes: a write to PMDIN is a write cycle to the
  board (the address on the PMA lines PMAEN enables, the data), a read
  of PMDIN gives the previous read cycle's data and starts the next;
  BUSY for the wait states (WAITB, WAITM, WAITE, at PB2's clock), then
  the interrupt with IRQM 01, and PMADDR stepped with INCM.
- **ADCHS**, enough to convert an input and read it: turning the ADC on
  makes its reference ready (BGVRRDY), ANENx makes ADCx ready (WKRDYx);
  the global software trigger (GSWTRG) converts each input whose
  trigger source it is, and the scan list with STRGSRC 1; RQCNVRT
  converts ADINSEL's. The result is the board's voltage against 3.3 V
  at the resolution SELRES gives, in ADCDATAn with ARDYn (reading it
  clears ARDYn) and, enabled, its interrupt. ADC0-4 convert their
  alternate inputs AN45-AN49 with ADCTRGMODE.SHxALT 01.

**The `explorer16-32` board** is Microchip's Explorer 16/32 (DM240001-3)
with the PIC32MZ EF plug-in module (MA320019). Its pins are the PIM's, as
its information sheet maps the board's 100-pin socket to the part:

| | Pin | |
|---|---|---|
| LEDs D3-D10 | RA0, RA1, RA2, RA3, RF13, RF12, RA6, RA7 | lit when the pin drives high |
| buttons S4, S5, S6 | RF8, RA7 (with D10), RD13 | low when pressed (the board's pull-ups); S3 is not wired on this PIM |
| TC1047A temperature sensor | AN4 (RB4) | 500 mV + 10 mV per degree C |
| potentiometer R6 | AN45 (RB5) | ADC0's alternate input |
| 16x2 LCD, HD44780 | PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD | the PMP in master mode |
| 25LC256 serial EEPROM | SCK RA5, SI RG8, SO RG7, CS RG15 | the program clocks it on the pins (RA5 has no SPI clock) |
| USB-UART | the UART the program uses | every UART's output is stdout |

The EEPROM is the PIC24 board's (below), on the socket pins the PIM
gives RA5, RG8, RG7 and RG15; since no SPI unit reaches RA5, a program
drives SCK and SI itself and reads SO as SPI mode 0 or 3 has it: SI
taken as SCK rises, SO's next bit out as it falls.

The run reports on stderr each LED that goes on or off, and the LCD's
two lines each time the program clears the display or sends its cursor
home -- the frame it had drawn -- and when the run ends:

```text
explorer16-32: LED D5 on (RA2) at 502.378 ms
explorer16-32: LCD |Temp:24.95 C    | at 1004.377 ms
explorer16-32:     |                |
```

The times are the core's cycles at 200 MHz. The LCD is an HD44780: its
instruction set (clear, home, entry mode, display on/off, cursor and
display shift, function set with 8- or 4-bit interface and one or two
lines, the CGRAM and DDRAM addresses), its 80-byte display RAM with line
2 at 0x40, never busy.

`--set NAME=VALUE` gives the board's inputs: `temperature=DEGREES` (25
by default), `pot=VOLTS` or `pot=PERCENT%` (1.65 V by default), and
`S4`, `S5` or `S6` `=pressed` or `=released`.

**The `curiosity-pic32mz-ef-2` board** is Microchip's Curiosity PIC32MZ
EF 2.0 (DM320209), with a PIC32MZ2048EFM144 (`embcc
--mcu=32MZ2048EFM144`, the same pack). Its pins are those of Harmony 3's
board support package for it:

| | Pin | |
|---|---|---|
| LED1, LED2, LED3 | RJ7, RK7, RJ3 | lit when the pin drives low |
| RGB LED: red, green, blue | RB7, RB8, RB9 | lit when the pin drives low |
| switches SW1-SW4 | RJ4, RJ5, RJ6, RC14 | low when pressed; the board has no pull-ups, so a released switch reads its pin's own (CNPUx), else 0 |
| USB-UART | the UART the program uses | every UART's output is stdout |

An LED lit low is off while its pin is an input or an open drain let
go. `--set SW1=pressed` (to `SW4`, `=pressed` or `=released`) gives the
switches:

```text
$ embsim fw.elf --board curiosity-pic32mz-ef-2 --dfp $DFP --set SW1=pressed
curiosity-pic32mz-ef-2: LED LED1 on (RJ7) at 0.052 ms
curiosity-pic32mz-ef-2: LED RGB green on (RB8) at 0.052 ms
```

Flash an image leaves out reads as erased flash does: 0xFFFFFFFF.

**Not modelled on the PIC32:** the FPU's and the DSP ASE's microMIPS
encodings (`-mmicromips` with hard float or the DSP), the MCU ASE's
IRET, the peripherals other
than these (their registers keep what is written), change-notification
interrupts, the external interrupt pins, conversion and baud-rate
timing, DMA, the watchdog and deadman timer, the prefetch cache, PMD's
module disables, sleep and idle.

## PIC32MX parts, and the Explorer 16/32's PIC32MX PIM

A PIC32MX board is built from `Microchip.PIC32MX_DFP` (`--dfp DIR`). Its
ATDF files will not do -- the PIC32MX795F512L's places 35 peripherals
(the timers, UARTs, ADC, SPI, I2C, input capture, output compare, PMP,
RTCC) 0xC below their addresses and has no port G or change-notice
registers -- so EmbSim reads the part's EDC file
(`edc/PIC32MX795F512L.PIC`, MPLAB's own), which has every register: 2955
of them at the device header's addresses, 2166 of them CLR, SET and INV
registers (the EDC's `portals`), each peripheral named as the EDC names
its instance (`UART2`, `TMR3`, the ports one `GPIO`). The interrupt
requests are the EDC's; which vector each one shares is the ATDF's
(beside it, `atdf/`), the one thing both files agree on.

**The core** is the `pic32mx` model of the [MIPS32
core](#what-it-models-the-mips32-core), the M4K: PRId 0x00018700,
Config with fixed mapping (no TLB: useg at physical 0x40000000 up,
kseg2 and kseg3 unmapped) and no caches, Config1 MIPS16e and EJTAG,
Config3 EIC and vectored interrupts, two register sets (SRSCtl.HSS 1);
no FPU, no DSP ASE, no microMIPS.

**The models:**

- **The interrupt controller**, in EIC mode as the family reference
  manual's section 8 has it: a request pending (IFSx and IECx) at its
  vector's priority (IPCx, by vector: UART1's error, receive and
  transmit requests are its one vector, 24); the highest priority, then
  subpriority, then the lowest vector. In single-vector mode
  (INTCON.MVEC 0) every interrupt is presented as vector 0, at EBASE +
  0x200, in the shadow set with INTCON.SS0; in multi-vector mode at
  EBASE + 0x200 + the vector times IntCtl.VS's spacing, in the shadow
  set if its priority is DEVCFG3.FSRSSEL's. INTSTAT says which vector
  and at what level (VEC, SRIPL).
- **The oscillator**: OSCCON from the configuration words (FNOSC, and
  PBDIV from DEVCFG1.FPBDIV), locked behind SYSKEY; the peripheral bus
  divides the core's clock by PBDIV (1, 2, 4, 8) for the timers and the
  ADC.
- **Timer1-5**, as the PIC32MZ's; **UART1-6**; **the PMP**.
- **Ports A-G**: port B's analog pins are AD1PCFG's clear bits; the
  pull-ups are CNPUE's, by each pin's change-notice number (the EDC's
  pin names: RD7 is CN16); with CNCON.ON, a pin whose CNEN bit is set
  flags CHANGE_NOTICE when its level changes.
- **SPI1-4**, as masters: a word written to SPIxBUF (8, 16 or 32 bits)
  is exchanged at once with the board's device on the unit's pins;
  SPIRBF, SPIROV, the receive interrupt.
- **ADC1**, the 10-bit converter: SAMP and ASAM, manual (SSRC 000) or
  auto-convert (111) after SAMC TADs, 12 TADs a conversion, CH0SA or
  the AD1CSSL scan, FORM's eight formats, the 16-word buffer (BUFM's
  halves), SMPI's interrupt, CLRASAM.

**The `explorer16-32-pic32mx` board** is the Explorer 16/32 with the
PIC32MX795F512L PIM (MA320003). The part's 100-pin package puts the
board's signals on the socket's pins one to one, as its own pin list
says:

| | Pin | |
|---|---|---|
| LEDs D3-D10 | RA0-RA7 | lit when the pin drives high |
| buttons S3, S4, S5, S6 | RD6, RD13, RA7 (with D10), RD7 | low when pressed (the board's pull-ups) |
| TC1047A temperature sensor | AN4 (RB4) | 500 mV + 10 mV per degree C |
| potentiometer R6 | AN5 (RB5) | |
| 16x2 LCD, HD44780 | PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD | the PMP |
| 25LC256 serial EEPROM | SCK2 RG6, SDI2 RG7, SDO2 RG8, CS RD12 | SPI2 |
| USB-UART | the UART the program uses | every UART's output is stdout |

The times the board reports are the core's cycles at 80 MHz, the part's
usual setting. `--set` takes what `explorer16-32-pic24` takes.

EmbCC builds for these parts with `--mcu=32MX795F512L` and the pack's
`--dfp`: its linker script places each `.vector_N` in its slot, EBASE +
0x200 + N times 32 bytes, as the pack's own script does.

**Not modelled on the PIC32MX:** the bus matrix's RAM and flash
partitions (BMX: user mode finds nothing at physical 0x40000000 up), the
prefetch cache (CHECON keeps what is written), the external interrupt
pins, the proximity timer (TPC), a PIC32MX1xx/2xx's per-port change
notices (CNCONx, CNENx, CNSTATx), the other peripherals (DMA, I2C,
input capture, output compare, RTCC, USB, CAN, Ethernet: their
registers keep what is written).

## What it models: the MIPS32 core

- **The instruction set.** MIPS32 Release 2, little-endian: every
  integer instruction, the branches and jumps with their delay slots
  (the likely branches skip theirs when not taken), mul, madd, msub and
  the HI/LO multiplies and divides, clz and clo, ext and ins, seb, seh,
  wsbh, rotr and rotrv, the traps, ll and sc, lwl, lwr, swl and swr,
  rdhwr; cache, pref, sync and synci do nothing. A divide by zero, or
  of INT_MIN by -1, leaves LO the dividend and HI 0, as QEMU's does (the
  architecture leaves them unpredictable). A floating-point instruction
  is a Coprocessor Unusable exception on the 24Kc, which has no FPU; an
  encoding no MIPS32r2 core has is a Reserved Instruction exception.
- **microMIPS** (`pic32mz`): the compressed instruction set XC32's and
  clang's `-mmicromips` produce, 16- and 32-bit instructions mixed --
  the 16-bit forms with their three-bit registers and encoded
  immediates, lwm and swm, lwp and swp, lwxs, addiupc, movep; the
  branches with offsets in halfwords and delay slots of either size
  (jals, jalrs, jalrs16, bgezals and bltzals want a 16-bit one, jal,
  jalr, jalr16, bal and jalx a 32-bit one: the other is a Reserved
  Instruction), and the compact beqzc, bnezc, jrc and jraddiusp. The
  ISA mode is bit 0 of a jump's target, as on the part: jr and jalr to
  an even address switch to MIPS32, jalx switches either way, a link
  register gets the mode to return to; an exception saves it in EPC's
  bit 0 (EPC the branch's address for its delay slot) and eret goes
  back to it. A branch in a delay slot is a Reserved Instruction, as on
  QEMU. The FPU's and the DSP ASE's microMIPS encodings end the run as
  a lockup that names them. `--trace` shows the halfwords;
  `--fault-report` names a microMIPS instruction by its halfwords.
- **MIPS16e** (`24kc`, malta's core, Config1.CA): the 16-bit
  instructions and the EXTENDed ones with 16-bit immediates, t8 for the
  compares, save and restore with their argument, static and extra s
  registers, addiupc and lwpc, the sign and zero extensions; b, beqz,
  bnez, bteqz, btnez, jrc and jalrc with no delay slot, jal, jalx, jr and
  jalr with a 16-bit one. The ISA mode is bit 0 of a jump's target, as
  for microMIPS (handlers run in MIPS32). As on QEMU, a branch in a
  delay slot, an extended addiupc or lwpc in one, an instruction EXTEND
  cannot extend and the 64-bit ones are Reserved Instructions.
- **The FPU** (`pic32mz`, which `--cpu pic32mz` puts on malta too):
  coprocessor 1 with 64-bit registers (Status.FR) or paired 32-bit ones,
  single and double precision and the word and long-word conversions,
  in IEEE 754-2008 mode (FCSR.NAN2008, ABS2008): loads and stores, the
  indexed ones too, the moves, add to sqrt, recip and rsqrt, abs and neg
  (the sign alone), the unfused multiply-adds, cvt, round, trunc, ceil
  and floor (a NaN gives 0, an overflow the largest integer of its
  sign), c.cond into FCC0-FCC7, bc1t and bc1f, FCSR's Cause, Flags and
  Enables and the floating-point exception. The arithmetic is IEEE
  754's in integers, the RISC-V core's: a NaN an operation makes is the
  default one (QEMU propagates an operand's payload; both are the
  architecture's). Paired singles and flush-to-zero are not modelled.
- **Coprocessor 0.** Status, Cause, EPC, ErrorEPC, BadVAddr, EBase,
  IntCtl, SRSCtl, SRSMap, Count and Compare, Config to Config3, PRId,
  HWREna, UserLocal, LLAddr, the TLB's registers and the debug ones;
  di, ei, eret, deret, wait, rdpgpr and wrpgpr.
- **Exceptions** are the privileged architecture's: EPC and Cause.BD
  for an instruction in a delay slot, Status.EXL and ERL, the boot
  vectors at `0xBFC00200` while Status.BEV is set and EBase's after,
  the TLB refill vector, BadVAddr, Cause.CE for a coprocessor that is
  not usable.
- **Memory.** kseg0 and kseg1 reach the bus's low 512 MiB unmapped;
  kuseg, kseg2 and kseg3 go through a 16-entry TLB (tlbr, tlbwi, tlbwr,
  tlbp), and kuseg is unmapped while Status.ERL is set. Loads and
  stores must be aligned (but for lwl, lwr, swl, swr): an address error
  otherwise. An access to nothing is a bus error exception. sc succeeds
  when ll's address still holds the value ll read (QEMU's rule), and
  eret clears the link.
- **Interrupts.** Compatibility mode (Cause.IP against Status.IM, the
  timer on IP7) and vectored mode (IntCtl.VS), the hardware lines
  IP2-IP7, and the two software interrupts; on a PIC32, EIC mode: the
  EVIC's level against Status.IPL (8 bits: Status bits 18 and 16-10),
  captured in Cause.RIPL as the interrupt is taken, its handler at
  EBASE plus the EVIC's offset, its shadow register set.
- **Shadow register sets** (`pic32mz`): an exception or interrupt saves
  SRSCtl.CSS in PSS and switches to ESS (an exception), the EVIC's set
  (EIC mode) or SRSMap's (vectored mode); eret switches back; rdpgpr
  and wrpgpr reach the previous set.
- **The DSP ASE** (`pic32mz`), Revision 2 in its MIPS32 encodings: the
  accumulators $ac1-$ac3 (mfhi, mflo, mthi, mtlo, the multiplies and
  accumulates), DSPControl (rddsp, wrdsp, bposge32), and the arithmetic
  on bytes, halfwords and words -- the wrapping, saturating and halving
  adds and subtracts, the shifts, the multiplies, the dot products and
  multiply-accumulates into an accumulator, the precision changes, the
  compares and picks, the extracts (extr, extp, extpdp), shilo, mthlip,
  insv, append, prepend, balign, the replicates, the indexed loads --
  with DSPControl's overflow flags, carry, ccond, pos and EFI as each
  instruction sets them. Each is a DSP State Disabled exception while
  Status.MX is clear. Where the manual leaves a result open (extp's with
  EFI set, insv's past bit 32, balign by 2) it is QEMU's.
- **The core timer.** Count counts every other
  cycle; Compare's match sets Cause.TI. wait sleeps until an interrupt:
  it skips ahead to the device that will raise one, and ends the run
  when nothing can.
- **sdbbp** enters debug mode, which needs a probe: with none, the run
  ends as a lockup at the breakpoint (startups put one in a default
  handler, for a debugger to stop at).

## PIC24 parts, and the Explorer 16/32's PIC24 PIM

QEMU has no PIC24 either. A PIC24 board is built from the part's device
pack -- `Microchip.PIC24F-GA-GB_DFP`, from MPLAB X or
packs.download.microchip.com -- found with `--dfp DIR` (the pack's
directory, which holds `edc/PIC24FJ1024GB610.PIC`), `--svd FILE.PIC`,
or `EMBSIM_SVD_PATH`. An image is XC16's: its ELF file, or the HEX file
MPLAB X writes beside it.

```sh
embsim dist/default/production/thermo.X.production.hex \
    --board explorer16-32-pic24 --dfp ~/.mchp_packs/Microchip/PIC24F-GA-GB_DFP/1.9.336
```

**The registers.** The pack's EDC file (MPLAB's description of a 16-bit
part) gives every special function register: 664 on the
PIC24FJ1024GB610, each at the address the pack's linker script
(`xc16/support/PIC24F/gld/p24FJ1024GB610.gld`) gives it, with its value
at power-on and the fields of the data sheet's names. Each behaves as
[above](#peripherals-from-an-svd-file---svd); the unimplemented bits
read 0, and the SFR space where no register is reads 0 and ignores
writes, as the part's does. The file names no peripherals: a run of
registers of one module is one, by its first register's name -- `INT`
(the interrupt controller), `OSC`, `TMR` (Timer1-5), `UART` (UART1-6),
`SPI` (SPI1-3), `GPIO` (ports A-G), `ADC1`, `PMP`, `PPS`, `RTCC`, and so
on, as `--svd-map` lists them. The core's registers (W0-W15, SPLIM, PCL,
PCH, DSRPAG, DSWPAG, RCOUNT, SR, CORCON, DISICNT, TBLPAG) are the core's.

**The models:**

- **The interrupt controller.** An interrupt is requested while its
  flag (IFSx), its enable (IECx) and a priority (IPCx: 1-7) are set and
  INTCON2.GIE is; the highest priority is presented, then the lowest
  number, and the core takes it when it is above its own. INTTREG says
  which; INTCON1 gets the traps' status bits; NSTDIS stops nesting.
- **The oscillator.** OSCCON starts on the configuration words' FNOSC;
  its bytes take a write only right after their unlock sequence (0x46,
  0x57 to the low byte; 0x78, 0x9A to the high one), as XC16's
  `__builtin_write_OSCCONL` and `_OSCCONH` write them, and a write
  without one is ignored with a warning. OSWEN switches at once. The
  core runs at half the oscillator: the 8 MHz FRC, the FRC divided by
  CLKDIV.RCDIV (OSCFDIV), or 32 MHz from the 96 MHz PLL over
  CLKDIV.CPDIV -- the time the board reports.
- **The Peripheral Pin Select** registers ignore writes while
  OSCCON.IOLOCK is set.
- **Timer1 to Timer5**, at the instruction clock over TCKPS (1, 8, 64,
  256), their flag at the match with PRx; Timer2/3 and Timer4/5 pair
  into 32-bit timers (T32), TMR3HLD and TMR5HLD latched by a read of
  the low half.
- **Ports A-G**, as the PIC32's (ANSx and IOCPUx for ANSELx and CNPUx),
  and their interrupt-on-change: with PADCON.IOCON, a pin's rising edge
  (IOCPx) or falling one (IOCNx) sets its IOCFx bit, IOCSTAT's port bit
  and the IOC interrupt's flag; the program clears IOCFx bits (a 1
  written keeps one, never sets it), and IOCSTAT follows.
- **UART1-6.** A byte written to UxTXREG with UARTEN and UTXEN goes to
  stdout at once; the transmit interrupt is flagged as the buffer has
  room again; the run's input arrives at the first UART on.
- **The EPMP**, in master mode: its chip selects' regions of the
  Extended Data Space (PMCSxBS), reached through the EDS window, are bus
  cycles to the board: the word offset of the address on the PMA lines
  PMCON4 enables, the low byte on an 8-bit port (one cycle a word, as
  Microchip's own Explorer 16/32 LCD driver depends on), 16 bits on a
  16-bit one.
- **ADC1**: SAMP or ASAM start a sample, the program's clearing of SAMP
  (SSRC 0000) or SAMC TADs (SSRC 0111) the conversion, 12 TADs (14 with
  MODE12); CH0SA's input, or with CSCNA the next of AD1CSSL/AD1CSSH's;
  the result against AVDD (3.3 V) in FORM's format, into the FIFO or the
  input's own buffer (BUFREGEN), and the ADC1 interrupt after SMPI + 1.
  AN28 is the band gap (1.2 V), AN29 AVSS, AN30 AVDD.
- **SPI1-3**, as masters: a byte written to SPIxBUFL is exchanged at
  once with the slave the board has on the unit's pins, and read back
  from SPIxBUFL (SPIRBE, SPIRBF, SPIROV).
- **The flash controller (NVM)**: TBLWTL and TBLWTH at TBLPAG 0xFA
  write its 128 latches (they read back by TBLRDL and TBLRDH there, and
  are 0xFFFFFF after a reset); NVMCON's operations -- two words (0001),
  a row (0010), a page's erase (0011), the user memory's (1110) -- run
  when WR is set right after NVMKEY's unlock (0x55 then 0xAA) with WREN:
  a program clears bits (an AND, as flash does), an erase sets the
  words to 0xFFFFFF. The core stalls for the data sheet's time (20 us a
  word pair, 1.5 ms a row, 20 ms an erase), then WR is cleared and the
  NVM interrupt flagged. WR without the unlock or WREN, or an operation
  the part has not, sets WRERR instead.
- **The RTCC**, in BCD: TIMEH/TIMEL (hours, minutes, seconds) and
  DATEH/DATEL (year, month, day, weekday) with their carries through
  leap years, from the secondary oscillator, the LPRC or the system
  clock (CLKSEL) over PS and DIV, while RTCEN; the alarm (ALMTIMEx,
  ALMDATEx, AMASK, CHIME, ALMRPT) flags ALMEVT and the RTCC interrupt;
  WRLOCK keeps the time and the control registers from the program, and
  is cleared only right after NVMKEY's unlock; a 1 written to TSAEVT
  takes a timestamp (TSATIMEx, TSADATEx) and the RTCCTS interrupt.
- **The DMA controller**, channels 0-7 (DMACHn, DMAINTn, DMASRCn,
  DMADSTn, DMACNTn): a byte or a word from source to destination in the
  data space, RAM or a register, each time the channel's trigger comes --
  the interrupt CHSEL names (any of the data sheet's Table 5-1) or CHREQ
  set by the program; one transaction a trigger (one-shot) or the whole
  count (continuous), the repeated modes reloading the addresses and the
  count; SAMODE and DAMODE keep, increment or decrement the addresses;
  DONEIF and the DMAn interrupt at the end, HALFIF half way (the
  interrupt too with HALFEN); RAM outside DMAL-DMAH stops the channel
  with LOWIF or HIGHIF. A transfer takes no time.
- **I2C1-3**, as masters, on a bus with no slave on it (the Explorer
  16/32's I2C goes to the MCP2221A's own master and to empty headers):
  SEN, RSEN, PEN and ACKEN take a bit time, (I2CxBRG + 2) instruction
  cycles, then clear and flag the MI2Cx interrupt (S, P); a byte written
  to I2CxTRN is sent in nine (TBF, TRSTAT) and is not acknowledged
  (ACKSTAT 1); one written while the module is busy is a collision
  (IWCOL); RCEN receives 0xFF, the bus's pull-ups, in eight (RBF; I2COV
  when RBF was still set), and reading I2CxRCV clears RBF.
- **The pins the peripherals drive and read.** The EDC names each pin
  (RD1, RP24, OCM1A...): an output the Peripheral Pin Select puts on an
  RPn pin (RPORx: OC1-3, an SCCP's OCMx), or a peripheral's own pin (an
  MCCP's OCMxA-F), drives it over TRISx and LATx -- PORTx reads it, the
  interrupt-on-change and the board see it -- and an input the PPS maps
  (RPINRx: IC1R...) reads its pin's level, so one module's output can be
  another's input on the same pin.
- **Input capture IC1-6**: ICxTMR counts the instruction clock or
  Timer1-5's (ICTSEL) while the module is on, cleared by SYNCSEL's
  source (Timer1-3's period, the ICs', CCPs' and ADC's interrupts, the
  OCs' and CCPs' sync outputs) or, with ICTRIG, held at 0 until it sets
  TRIGSTAT; the input's edges ICM selects (every edge, rising, falling,
  every fourth or sixteenth rising) capture it into the four-deep FIFO
  (ICxBUF, ICBNE) at the time they come; the interrupt every ICI + 1
  captures, and ICOV when one comes with the FIFO full.
- **Output compare OC1-6**: OCxTMR counts OCTSEL's clock, back to 0
  after SYNCSEL's source (11111: its own match with OCxRS) or at
  0xFFFF, held by OCTRIG until TRIGSTAT; single compare (set, clear,
  toggle at OCxR), a single or continuous pulse (OCxR to OCxRS), edge-
  and center-aligned PWM with OCxR and OCxRS double-buffered; the
  interrupt at each, OCINV and OCTRIS on the output, and the match with
  OCxRS the module's sync output.
- **MCCP1-3 and SCCP4-7**: the time base on the system clock or its
  double over TMRPS, its period CCPxPRL (CCPxPRH:CCPxPRL with T32), or
  another CCP's sync output, or held by TRIGEN until TRSET (TRCLR holds
  it again); the CCTx interrupt each period. Timer mode (two 16-bit
  timers, the second's period the CCPx interrupt, or one 32-bit), the
  single edge compare modes (SCEVT), dual edge compare and its buffered
  PWM, on the pins OCxEN enables with POLACE and POLBDF; input capture
  from the ICMx pin into a four-deep FIFO (two with T32), the interrupt
  every OPS + 1 captures, ICOV, ICDIS, and Edge Detect mode's captures
  that never overflow.

**The `explorer16-32-pic24` board** is the Explorer 16/32 with the
PIC24FJ1024GB610 PIM (MA240023). The board's schematic names the socket
pin of each part, and the PIM connects the part's pins to the socket
one to one, but for the exceptions its information sheet (DS50002423)
lists; its jumpers are in their USB position:

| | Pin | |
|---|---|---|
| LEDs D3-D10 | RA0-RA7 | lit when the pin drives high |
| buttons S3, S4, S5, S6 | RD6, RD13, RA7 (with D10), RD7 | low when pressed (the board's pull-ups) |
| TC1047A temperature sensor | AN4 (RB4) | 500 mV + 10 mV per degree C |
| potentiometer R6 | AN5 (RB5) | |
| 16x2 LCD, HD44780 | PMD0-7, RS on PMA0, E on PMWR, R/W on PMRD | the EPMP |
| 25LC256 serial EEPROM | SCK RG6 (RP21), SI RG8 (RP19), SO RG7 (RP26), CS RG0 | the SPI unit the PPS puts on those pins |
| USB-UART | the UART the program uses | every UART's output is stdout |

The EEPROM is a 25LC256: 32 KiB, erased (0xFF) when the run starts;
READ, WRITE (a 64-byte page, written as CS rises, then 5 ms busy),
WREN, WRDI, RDSR, WRSR and its block protection. The run reports the
LEDs and the LCD as the PIC32 Explorer does; `--set` gives
`temperature=DEGREES`, `pot=VOLTS` or `pot=PERCENT%`, and `S3` to `S6`
`=pressed` or `=released`.

**Not modelled on the PIC24:** the other peripherals' behaviour (their
registers keep what is written: CRC, CLC, CTMU, USB, the comparators),
I2C's slave mode and a slave on the bus, the DMA's peripheral indirect
and null write modes, the RTCC's power control, its fine division and
its pins, the NVM's dual partitions, the cascaded 32-bit input capture
and output compare, the output compare's faults, the CCPs' center-
aligned, variable frequency and external input modes, their output
modes other than steering, dead time, auto-shutdown and gating, their
clocks other than the system clock and its double, the sync sources
the models do not make (the comparators, CLCs, CTMU, INTx pins), the
cycle or two the part's captures and compare outputs lag, the external
interrupt pins, the alternate vector table,
the watchdog and deadman timer, Sleep and Idle beyond waiting for an
interrupt, the EPMP's slave modes and its wait states' time, the SPI's
and the UART's bit timing.

## What it models: the PIC24F core

- **The instruction set** of the PIC24F, by Microchip's 16-bit MCU and
  DSC Programmer's Reference Manual (DS70000157): every instruction the
  manual marks as the PIC24F's, byte and word forms, with the
  addressing modes (register direct, indirect with pre/post increment
  and decrement, register offset, signed 10-bit offset, the file
  registers' 13 and 16-bit addresses) and SR's flags as each
  description gives them (Z only cleared by ADDC, SUBB, SUBBR, CPB).
  BSW.Z writes Z's complement, as the manual's examples have it. The
  dsPIC's DSP instructions and the PIC24E's are not the core's: an
  opcode it does not have makes the part reset (RCON.IOPUWR), and the
  run ends there as a lockup naming it.
- **REPEAT**: the target runs RCOUNT + 1 times, SR.RA set meanwhile,
  and may be interrupted between any two runs. DIV.S, DIV.U and their
  .D forms give the quotient, remainder and flags on the first of their
  18 runs (REPEAT #17), so an interrupt in the middle leaves W0 and W1
  as the program will see them; a divide by zero is a math error trap.
- **The data space.** The core's registers at 0x0000-0x0055; a W
  register used as a pointer before anything is written to it makes the
  part reset (RCON.IOPUWR): the run ends there. The SFRs and RAM below
  0x8000; above, the EDS window: DSRPAG (reads) and DSWPAG (writes)
  pages 1-0x1FF are the EDS (RAM above 0x8000, then the EPMP's space),
  0x200-0x2FF program memory's low words and 0x300-0x3FF its high bytes
  (program space visibility: XC16's `const` data); page 0 there is an
  address error, as is a word access at an odd address. W15 as a
  pointer below 0x0800 or above SPLIM is a stack error.
- **Table reads** of program and configuration memory (TBLPAG); table
  writes go to the flash's latches, which are not modelled. Program
  memory an image leaves out reads as erased flash does: 0xFFFFFF
  (NOPR, if it is run), the phantom byte 0.
- **Exceptions.** Traps (address, stack and math errors, the hard and
  soft ones) and interrupts through the vector table at 0x000004: the
  PC and SR's low byte with IPL3 pushed, the priority raised (IPL3 for
  a trap), the vector's address; RETFIE undoes it. DISI holds
  priorities 1-6 off for its count. A trap prints what raised it on
  stderr; one at or below the CPU's priority makes the part reset, and
  the run ends there.
- **PWRSAV** waits for an interrupt, skipping ahead to the device that
  will raise one; with none, the run ends. RESET ends it (exit 0).

## dsPIC33EP parts

A dsPIC33EP or PIC24EP part is built, by its name, from Microchip's
dsPIC33E device pack (`Microchip.dsPIC33E-GM-GP-MC-GU-MU_DFP`, which
holds both families: `--dfp DIR`, or `EMBSIM_SVD_PATH`):

    embsim fir.hex --board dspic33ep512mu810 \
        --dfp ~/.mchp_packs/Microchip/dsPIC33E-GM-GP-MC-GU-MU_DFP/1.4.170

Its EDC gives the register file -- every SFR at the address the pack's
linker script gives it (the dsPIC33E's EDC splits Timer1 from Timer2-9,
the PPS's RPORx from its RPINRx and UART1-2 from UART3-4 into modules
of their own; each is one peripheral here) -- the program memory and
configuration words (in their own memory at 0xF80000 on the larger
parts, in program memory's last page on the smaller), the RAM from
0x1000 and its Y data space. The models are the PIC24's that fit the
part: the interrupt controller (with the dsPIC33E's math error trap
conditions and its DO stack overflow), the oscillator (the 7.37 MHz FRC,
the PLL: Fin * M / (N1 * N2) from PLLFBD and CLKDIV) and the PPS lock,
the timers, UART1-4 and the ports with ANSELx; and the dsPIC33E's own:

- **SPI1-4** in master mode (SPIxSTAT, SPIxCON1, SPIxBUF): a byte (a word
  with MODE16) written to SPIxBUF is exchanged at once with what the
  board has on the unit's pins; SPIRBF says a word came back, reading
  SPIxBUF clears it, and one arriving while it is set is lost (SPIROV).
- **ADC1**, channel 0: sampling by SAMP or ASAM, the conversion when the
  program clears SAMP (SSRC 000) or after SAMC TADs (111), 12 TADs (14 at
  12 bits, AD12B) of ADCS + 1 instruction cycles; CH0SA or the inputs
  AD1CSSL and AD1CSSH scan; 10 or 12 bits in FORM's four formats; the
  results through ADC1BUF0-F as SMPI and BUFM say, DONE, the AD1
  interrupt.
- **The PMP** in its master modes, as the PIC32's (PMDIN1 its data
  register, its wait states instruction cycles).

A part by its name has no board around it, so nothing outside drives its
pins: an input reads its pull-up (CNPUx), else 0. `--set RD5=low` drives
one for the whole run, as a board's input would -- `R`, the port's letter
(there is no port I), the bit, and `low`, `high`, `0` or `1`; up to 16
pins.

`explorer16-32-dspic33` is the Explorer 16/32 with the dsPIC33EP512MU810
PIM, whose pins are the board's 100-pin socket's as the
PIC32MX795F512L PIM's: the LEDs on RA0-RA7, S3 (RD6), S4 (RD13), S5
(RA7) and S6 (RD7) pulled up by the board, the TC1047A on AN4 and the
potentiometer on AN5, the LCD on the PMP (RS on PMA0), the 25LC256 on
SPI2's own pins (RG6-RG8) with its CS on RD12. `--set` takes what it
takes for `explorer16-32-pic24`.

## What it models: the dsPIC33E core

The PIC24 core of a dsPIC33EP part is the PIC24E's, with the dsPIC's
DSP engine; a PIC24EP's is the PIC24E's alone. By the programmer's
reference manual (DS70000157) and the dsPIC33F family reference manual's
CPU and data memory sections (DS70204, DS70202):

- **The PIC24E's instructions** where they differ from the PIC24F's:
  CALL, RCALL, CALL.L and LNK keep the stack frame flag (CORCON.SFA) in
  the stacked return address's bit 0, RETURN, RETLW and RETFIE restore
  it, ULNK clears it, as an exception's entry stacks it; the computed
  RCALL, GOTO and BRA's own encodings, CALL.L and GOTO.L (a 23-bit
  target in a register pair); CP and CPB with an 8-bit literal; REPEAT
  #lit15 and REPEAT of a whole Wn; CPBEQ, CPBNE, CPBGT and CPBLT (an
  offset of 1 is the compare and skip); MULW's 16-bit product; MOVPAG.
  An opcode of the PIC24F's alone, or a DSP instruction on a PIC24EP, is
  a lockup naming it, as on the PIC24F.
- **The accumulators** A and B, 40 bits, at 0x22-0x2C (ACCxU reads
  sign-extended to 16 bits). Written as registers they change no status
  bit.
- **The DSP instructions**: MAC, MPY, MPY.N and MSC of two of W4-W7 or of
  one squared, ED and EDAC, CLR and MOVSAC, with their prefetches (a word
  from X space through W8 or W9 -- `[Wx]`, `[Wx]+=k`, `[Wx]-=k`,
  `[W9+W12]` -- and one from Y space through W10 or W11 into W4-W7; an
  address in the other space reads 0) and the other accumulator's
  write-back to W13 or `[W13]+=2`, rounded; ADD and SUB of the
  accumulators, NEG; ADD Ws and LAC with their shift of the word placed
  in bits 31:16; SAC and SAC.R with theirs; SFTAC by a literal or by Wb;
  the MCU multiplies into an accumulator (MUL.xx Wb, Ws, Acc); BRA OA,
  OB, SA and SB.
- **The multiplier**: signed, unsigned or mixed (CORCON.US: W4 and W6
  unsigned, W5 and W7 signed), 1.31 fractional (the product shifted left
  once) or integer (CORCON.IF).
- **The saturation logic**, as SATA, SATB and ACCSAT say: none (40 bits,
  a carry out of bit 39 setting SA or SB: the sign lost), normal (1.31,
  saturated to 0x007FFFFFFF or 0xFF80000000) or super (9.31, to
  0x7FFFFFFFFF or 0x8000000000). OA and OB say a result does not fit
  1.31 (never with normal saturation); SA and SB stay set until the
  program clears them (clearing SAB clears both). INTCON1's OVATE, OVBTE
  and COVTE make an overflow and a catastrophic one a math error trap
  (OVAERR...), as a divide by zero (DIV0ERR) and SFTAC past 16 bits
  (SFTACERR, the accumulator unchanged) always are.
- **Rounding and stores**: SAC.R and the write-back round as CORCON.RND
  says, conventionally (up from 0x8000) or convergently (0x8000 up only
  with bit 16 set); with CORCON.SATDW (set out of reset), SAC and SAC.R
  store 0x7FFF or 0x8000 for what bits 39:16 do not hold as 1.15; the
  write-back is never saturated.
- **DO loops**: DO #lit15 or Wn runs to its last instruction DCOUNT + 1
  times, with no instruction of its own; four can nest, the outer loops'
  DCOUNT, DOSTART and DOEND on the DO stack (CORCON.DL, SR.DA), and a
  fifth DO is the soft trap (INTCON3.DOOVR), changing nothing. Setting
  CORCON.EDT, or writing DCOUNT 0, ends a loop at the end of its pass.
- **DIVF** (after REPEAT #17): the 1.15 quotient of Wm by Wn in W0, the
  remainder in W1, OV when Wm is not smaller than Wn.
- **The address generators**: modulo addressing -- MODCON's XWM register,
  every instruction's indirect address through the X AGU (its pre- and
  post-modification wrapping by the buffer's length past XMODEND
  stepping up or below XMODSRT stepping down, an offset address
  likewise), and YWM's, the Y prefetches' (YMODSRT, YMODEND); and
  bit-reversed addressing -- XBREV.BREN, MODCON's BWM register, its word
  writes through `[Wn++]` and `[++Wn]` stepped by XB in reverse-carry
  order, its reads and byte writes and decrements not.
- **The data space**: SFRs below 0x1000, RAM from 0x1000 (W15 there out
  of reset, a stack error below it), DSRPAG and DSWPAG 1 out of reset,
  so data addresses above 0x8000 reach the RAM there (a larger part's Y
  space) as on the part.

## Counting

Each instruction gets the cost in `src/arch/cost.h`, the table
tools/bench uses in QEMU. On the Cortex-M that covers loads, multiples,
divides, VFP division and square root, and 2 cycles more for a taken
branch. On RISC-V (a plain in-order pipeline, at either width): 1, a
load 2, a divide or remainder 16, fdiv and fsqrt 16, and 2 more for a
taken branch or jump.

- `--stats` prints the count and the estimate when the run ends.
- `--count FILE` writes them as two lines, the format of the bench's
  QEMU plugin, so either can feed `tools/bench`.

On the MIPS32 core (a microAptiv-class pipeline): 1, a load 2, a
multiply into HI and LO or `mul` 2, a divide 18, and nothing more for a
taken branch -- the delay slot hides it, and its instruction counts as
any other.

On the PIC24F the count is the manual's, in instruction cycles (half
the oscillator's): one an instruction, two for a taken branch, GOTO,
CALL, RCALL, MOV.D, PUSH.D, POP.D and the table instructions, three
for RETURN, RETLW and RETFIE, one more for a skip over a word and two
over a two-word instruction, one more for a read through the EDS
window, five for an exception's entry. On the PIC24E and dsPIC33E, the
manual's for them: four for a taken branch, GOTO, CALL and RCALL, five
for a taken compare and branch, six for RETURN, RETLW and RETFIE, five
for the table instructions, four more for a read of program memory
through the EDS window, one more for a read of a peripheral's
register; DO two, and its loop's return none.

On the Cortex-M, RISC-V and MIPS the estimate is a model, not a
cycle-accurate core: there are no wait states and no pipeline stalls. It
charges the things a compiler chooses between. On the AVR it is the
datasheet's count (above). On the AVR `--count`'s file has a third
line: the instructions a skip passed over, which are not run and are
not in the count (QEMU's plugin counts them; see below).

`--trace FILE` writes the address and halfwords of every instruction
executed (`-` for stderr); on RISC-V, the instruction as one word, or
as one halfword for a compressed one; on the AVR, its one or two
words, at the byte address; on MIPS, the instruction as one word, at
its virtual address; on the PIC24, its one or two 24-bit words, at its
program address.

## Output, and the end of a run

The board's UART writes to stdout. The UART must be enabled as on the
part: CMSDK's CTRL, the nRF51's ENABLE and STARTTX, the AVR's TXEN0.
The NS16550A sends what is written to THR, and its LSR always says the
transmitter is empty. Its receiver has nothing, unless the run has an
input (`--input`, [below](#record-and-replay---input---record---replay)).

On the Cortex-M (`bkpt 0xab`) and on RV32 (an `ebreak` between its
markers, [above](#what-it-models-the-risc-v-core)), semihosting is on
unless `--no-semihosting` is given. It handles:
- console output: SYS_WRITEC, SYS_WRITE0 and SYS_WRITE;
- the exit calls: SYS_EXIT and SYS_EXIT_EXTENDED;
- the small calls a C library makes at start-up.

| The run ends when | Exit status |
|---|---|
| the image calls SYS_EXIT_EXTENDED | the code it gives |
| the image calls SYS_EXIT | 0 for ADP_Stopped_ApplicationExit, else 1 |
| the image stores 0x5555 to virt's test device | 0 |
| the image stores 0x3333 to the test device | the upper halfword stored |
| the image writes 0x42 to malta's SOFTRES | 0 |
| the image sets the ESP32-C3's RTC_CNTL OPTIONS0.SW_SYS_RST | 0 |
| a PIC24's RESET instruction | 0 |
| the image requests a reset (AIRCR.SYSRESETREQ) | 0 |
| a branch to itself, or a WFI, MIPS wait or PIC24 PWRSAV, that no exception or interrupt can interrupt | 0 |
| an AVR SLEEP that nothing can wake (the AVR harness ends in such a loop) | 0 |
| the output contains `--until`'s string | 0 |
| the core locks up (a RISC-V trap or MIPS exception whose vector cannot be fetched; an AVR instruction the part does not have; a MIPS sdbbp with no debugger; a PIC24 opcode the core does not have, a W register used as a pointer before it is written, or a trap at the CPU's priority -- each a reset on the part; an instruction EmbSim does not model) | 3 |
| the stack overflows, with `--stack-report` or `--stack-limit` | 3 |
| `--max-insns` instructions have run | 4 |
| `--replay`: the run is not the recording's | 5 |
| the image cannot be loaded, or an option is wrong | 2 |

A run that ends at a lockup or at `--max-insns` says why on stderr.

## Testing and analysis

These are the measurements a firmware's CI wants of a run, and what a
debugging session wants after one. Each is an option; a run without
them is exactly the run it always was, and with them the program's
output, its exit status and its counts do not change.

They read the image's symbol table, and its DWARF when it was built
with `-g`: the line table for source lines. EmbCC, GCC and clang images
alike (DWARF 2 to 5).

### Coverage: `--coverage`

`--coverage FILE` counts each instruction the run executes, and at the
end writes which source lines and functions ran, and how often (`-` for
stderr):

```sh
embcc --target=thumbv7m-none-eabi --opt=none -g -c main.c
embsim fw.elf --coverage fw.cov
embsim fw.elf --coverage fw.info --coverage-format=lcov
genhtml fw.info -o coverage-html
```

The report is gcov's layout, file by file: a summary, each function and
the times it was entered, and the source with each line's count
(`#####`: code that never ran; `-`: no code).

```text
tests/golden/embsim-an/cov.c: 14 of 17 lines (82.4%), 2 of 3 functions (66.7%)
  function never_called (line 17): 0
  function classify (line 22): 5
  function main (line 32): 1
    #####:   17:static int never_called(int x)
        -:   18:{
    #####:   19:    return x * 3;
        -:   20:}
        -:   21:
        5:   22:static int classify(int v)
        -:   23:{
        5:   24:    int r = 1;
        5:   25:    if (v < 0)
        1:   26:        r = -1;
        4:   27:    else if (v == 0)
        1:   28:        r = 0;
        5:   29:    return r;
...
        6:   35:    for (int i = 0; i < 5; i++)
        5:   36:        sum += classify(i - 1);
        1:   37:    if (sum > 100)
    #####:   38:        sum = never_called(sum);

total: 14 of 17 lines (82.4%), 5 of 7 functions (71.4%)
```

- A line has code when the line table gives it addresses in the image's
  executable segments. Its count is that of its most-executed
  instruction: the times it ran, for straight-line code. A `for` line
  counts its test (6 for 5 turns). A line that holds code shared by
  several statements counts them all: at -O0 a function's epilogue is
  usually on its last `return`, which then counts every return.
- A function's count is its first instruction's: the times it was
  entered. A function inlined everywhere counts 0, and its lines count
  where they were inlined.
- The source is read from the path the line table gives (relative to
  the directory EmbSim runs in); when it cannot be, the lines with code
  are listed alone.
- Without `-g` there is no line table: the report has the functions
  alone, from the symbol table, with a warning.
- Functions with no line information (a startup file built without
  `-g`, the C library) are listed apart, and count in the total.
- `--coverage-format=lcov` writes lcov's tracefile instead (`SF`, `FN`,
  `FNDA`, `FNF`, `FNH`, `DA`, `LF`, `LH`), which genhtml, Codecov,
  Coveralls and most CI services read.

### Profiling: `--profile`

`--profile` writes, when the run ends, the instructions and estimated
cycles each function took, on stderr; `--profile=FILE` to a file.

```text
embsim profile: fw.elf, 2809 instructions, 4164 cycles (est.)
  self insns  self cycles   self   incl insns  incl cycles   incl     calls  function
        1320         1920  46.1%         1338         1941  46.6%        30  spin
         995         1330  31.9%         2809         4164 100.0%         0  reset
         224          410   9.8%         1814         2834  68.1%         1  main
         129          231   5.5%          174          311   7.5%         2  putn
          29           77   1.8%           29           77   1.8%         5  rec
          30           42   1.0%           30           42   1.0%         6  leaf
          18           21   0.5%           18           21   0.5%         3  tick
...
        2809         4164 100.0%                                             (total)
```

- **self** is what ran in the function itself; **incl** adds what its
  callees ran, and its interrupt handlers' when an interrupt came while
  it was on the stack. A recursive function's inclusive counts each
  instruction once. **calls** is how often a call, or an exception's
  entry, reached it.
- **The counts add up exactly to `--stats`' totals.** Each step of the
  run goes to one function, whatever it costs: a taken branch's extra
  cycles, a WFI's or a SLEEP's wait (to the function that waited), an
  AVR interrupt's four entry cycles (to the code it interrupted).
- Code no symbol covers (QEMU's reset ROM on virt) is `[unknown]`.

EmbSim follows the calls and returns as the core executes them, which
needs no frame pointer and no debug information:

| Core | Calls | Returns |
|---|---|---|
| Cortex-M | `bl`, `blx` | `bx`, `pop {..., pc}`, `ldm` with the pc, `ldr pc`, `mov pc` |
| RISC-V | `jal` and `jalr` that link (`ra` or `t0`), `c.jal`, `c.jalr` | `jalr` and `c.jr` that do not |
| AVR | `call`, `rcall`, `icall` | `ret` |
| MIPS | `jal`, `jalr` that links, `bal` and the other linking branches | `jr`, `jalr $zero` |

A MIPS call or return transfers after its delay slot: the slot's
instruction counts to the caller, and the frame begins (or ends) where
the slot hands over. A return is one when it lands where a call on the stack returns to:
that call's frame and the ones above it end, so a tail call (a branch to
another function, which then returns for both) leaves the stack right,
and the function it branched to is shown as called from the one that
branched. An exception's entry is a frame too, and its return (an
EXC_RETURN, `mret`, `reti`, `eret`) ends it wherever the handler returns to.
Where the vector table holds a jump (the AVR's, RISC-V's vectored mode,
a MIPS vector's `j` and its delay slot) the handler is the jump's
target, and the jump counts as the handler's.

`--profile-format=collapsed` writes the call paths instead, one a line
with its cycles (`collapsed-insns`: its instructions), the format
flamegraph.pl, speedscope and inferno read:

```text
reset;main;a 36
reset;main;a;leaf 21
reset;main;rec;rec;rec 16
reset;main;spin 1920
reset;main;spin;tick 21
```

```sh
embsim fw.elf --profile=fw.folded --profile-format=collapsed
flamegraph.pl fw.folded > fw.svg
```

### The stacks: `--stack-report` and `--stack-limit`

`--stack-report` writes, when the run ends, how deep each stack went and
which functions took it there (on stderr; `--stack-report=FILE` to a
file). Give it the `.su` files `--stack-usage` wrote (`--stack-su FILE`,
or simply the files: an argument ending in `.su` is one), and embrt's
`--json` report (`--stack-embrt FILE`), and their numbers go beside the
run's:

```sh
embcc --target=thumbv7m-none-eabi --opt=basic --stack-usage --call-graph=su -c *.c
embrt *.o --entry main --isr SysTick_Handler --json > bound.json
embsim fw.elf --stack-report --stack-embrt bound.json *.su
```

```text
embsim stack: fw.elf
main stack (MSP): top 0x20010000, deepest 0x2000ffa0: 96 bytes used of 65216 (down to 0x20000140, _end)
process stack (PSP): not used
by function, deepest first (bytes; frame and incl from the function's entry):
  function                    deepest   depth   frame     .su    incl   embrt
  writec                   0x2000ffa0      96      16      16      16       -
  rec                      0x2000ffa8      88       8       8      40       -
  putn                     0x2000ffb0      80      32      32      48      48
  tick                     0x2000ffb0      80       0       0       0       0
  main                     0x2000ffd0      48      24      24      72    none
the run stayed within the static numbers
```

- **The stacks.** The main stack's high-water mark, and on a Cortex-M
  the process stack's (when a thread ran on the PSP; each is read as
  the core banks it). The top is the stack pointer's highest value (on
  a Cortex-M, the reset value from the vector table too). The region
  runs down to the stack's limit: `--stack-limit`, else the link's
  `__stack_limit` (`__StackLimit`, `_sstack`), else the end of `.data`
  and `.bss` (`_end`, `__bss_end`, `_ebss`, `end`), else the bottom of
  the RAM the stack is in.
- **deepest**, **depth**: the lowest sp while the function was running,
  and how far that is below the top.
- **frame**: the most the function moved sp below its entry -- the
  number `--stack-usage` gives, beside it as **.su**. On the AVR the
  call's two-byte return address is the callee's, as avr-gcc's and
  EmbCC's `.su` count it.
- **incl**: the most the stack went below the function's entry while it
  was on the stack, its callees' frames included and the interrupts that
  came meanwhile not -- the number embrt bounds for an entry point,
  beside it as **embrt** (`none` where embrt finds no bound, such as
  recursion).
- A frame or a depth above its static number is marked `ABOVE THE
  STATIC BOUND`: the static analysis missed something, and the last line
  says so.
- On RISC-V and the AVR there is one stack pointer: a program that
  switches stacks (an RTOS's tasks) is measured over all of them.
- A PIC24's stack grows up, from XC16's `__SP_init` (W15 before the
  startup sets it is not counted) toward `__SPLIM_init` (or the top of
  RAM): the report gives its **base** and **highest** W15 (data
  addresses), and the call's four-byte return address is the callee's
  frame, as on the AVR. The part's own check of W15 against SPLIM is
  the core's: a stack error trap, which `--fault-report` reports.

**An overflow ends the run.** With `--stack-report` or `--stack-limit
ADDR` (or a symbol's name), the main stack going below its limit -- past
`--stack-limit` or `__stack_limit`, or, with neither, into `.bss` and
`.data` -- stops the run at the instruction that moved sp there, as a
part with a stack guard would fault, with status 3:

```text
embsim: stack overflow: sp 0x2000f7f8 is below the stack limit 0x2000f800 (__stack_limit), at deep (ovf.c:9); 354 instructions, 573 cycles (est.)
embsim: stack overflow: sp 0x1ffffff0 is below the end of .data and .bss 0x20000004 (_end), at deep+0x2 (ovf.c:9); 10046 instructions, 16432 cycles (est.)
```

An exception's entry that stacks past the limit is reported as one (`in
an exception's entry, at` the instruction it interrupted).

### Faults: `--fault-report`

`--fault-report` decodes each fault the moment the core takes it, on
stderr (`--fault-report=FILE` to a file): a Cortex-M's HardFault,
MemManage, BusFault and UsageFault, a RISC-V exception (an ECALL and
an EBREAK, which a program makes on purpose, are not faults), and a
MIPS exception other than an interrupt, a syscall and a break: Cause's
code in words, BadVAddr for an address or TLB exception, the
coprocessor of a Coprocessor Unusable, the registers of the set the
exception interrupted:

```text
embsim: fault: DSP ASE state disabled: a DSP instruction with Status.MX clear (Cause.ExcCode 26) at 0x9d000b5c, in main+0x6c (pic32-fw.c:123)
  instruction 0x9d000b5c: 01200811  mthi t1, $ac1
```

```text
embsim: fault: HardFault (exception 3) at 0x00000160, in crash (fault.c:31)
  HFSR  0x40000000  FORCED: a configurable fault escalated to HardFault (its handler disabled, or not able to preempt)
  CFSR  0x00008200
        PRECISERR: a precise data bus error at 0x30000000
  BFAR  0x30000000 (BFARVALID)
  instruction 0x00000160: 6800       ldr r0, [r0, #0]
  stacked frame at 0x2000ff90 (MSP):
    r0  0x30000000  r1  0x20000004  r2  0xe000e010  r3   0x00000000
    r12 0x00000000  lr  0x00000173  pc  0x00000160  xpsr 0x0100000f
    lr: level2+0xc (fault.c:37)
  r4  0x00000007  r5  0x00000000  r6  0x00000000  r7   0x00000000
  r8  0x00000000  r9  0x00000000  r10 0x00000000  r11  0x00000000
  backtrace (.debug_frame):
    #0  0x00000160 in crash (fault.c:31)
    #1  0x00000172 in level2+0xc (fault.c:37)
    #2  0x000001c4 in tick+0x8 (fault.c:65)
        <- an exception's entry (EXC_RETURN 0xfffffff9): the code it interrupted
    #3  0x000001da in spin+0xa (fault.c:71)
    #4  0x00000234 in main+0x52 (fault.c:110)
    #5  0x000000a2 in reset+0x92
        (no call frame information for 0x000000a2: the unwinding stops here)
  handler: none -- the vector is 0x00000000, without the Thumb bit: the core cannot run it (it will fault again)
embsim: the core locked up at 0x00000000, in 0x00000000: the fault above had no handler
```

- **The status registers in words**: each CFSR bit set (MemManage's,
  BusFault's and UsageFault's), HFSR's FORCED and VECTTBL, and BFAR and
  MMFAR when their valid bits say they hold the address. On RISC-V,
  mcause by name, and mtval as what it is for that cause (the address,
  or the instruction's bits).
- **The stacked frame** -- r0-r3, r12, lr, pc and xPSR, read from the
  stack the EXC_RETURN in lr names -- and the other registers; on
  RISC-V, the 32 registers.
- **The faulting instruction**, disassembled (Thumb, RISC-V; a form the
  disassembler does not know is its encoding), with its symbol and line.
- **The backtrace**, by the image's `.debug_frame` (EmbCC writes it for
  Thumb and RISC-V with `-g`) from the registers at the fault. On a
  Cortex-M it goes through an exception's frame, where the CFI's return
  address is an EXC_RETURN, into the code the exception interrupted.
  Where no CFI covers the faulting pc, it is the calls EmbSim saw the
  core make (the call stack `--profile` follows), named by the symbol
  table.
- **The handler**: the one the vector table or mtvec gives, or that
  there is none -- a vector without the Thumb bit or nowhere near a
  function, an mtvec where nothing can be fetched -- in which case the
  core locks up next, and the run ends with status 3 and one more line.
- On a PIC24, a trap is the fault -- an address error, a stack error, a
  math error, an oscillator failure: what the instruction did ("a word
  read at the odd address 0x1001"), the instruction itself, disassembled,
  the W registers, SR and SPLIM as the trap found them, the calls EmbSim
  saw, and the trap's handler:

  ```text
  embsim: fault: address error trap (trap 1) at 0x00022e, in start+0x2e
    a word read at the odd address 0x1001
    instruction 0x00022e: 780011         mov [w1], w0
    registers:
      w0  0x0000  w1  0x1001  w2  0x0000  w3  0x0000
      ...
      w12 0x0000  w13 0x0000  w14 0x0000  w15 0x1200
      sr  0x0103  splim 0x1ff0  corcon 0x0004  rcount 0x0000
    backtrace (the calls EmbSim saw the core make):
      #0  0x0000022e in start+0x2e
    handler: 0x00025c, addrerr (the trap's vector, returning to 0x000230)
  ```
- An AVR has no faults; with `--fault-report` its lockup (an instruction
  the part does not have) gives the pc and the calls that led there.

It is an option, not the default when a run stops on an unhandled
fault, for two reasons. A run's stderr without new options is the same
as before, byte for byte, which scripts and the golden tests read. And
most firmware does not stop on its faults: its HardFault handler is a
`b .`, so EmbSim ends the run as an idle loop (status 0), or a handler
logs and resets. The report is taken at the fault's entry, so it is
there in both cases, and for every fault a program handles too (a test
that provokes faults gets one report each), which as a default would be
noise.

### Record and replay: `--input`, `--record`, `--replay`

`--input FILE` connects FILE (`-`: stdin) to the board's UART receiver:
the PL011 of the lm3s6965evb, the CMSDK UART of the MPS2 boards, virt's
NS16550A and the AVR's USART0.

| UART | A byte arriving | Its interrupt |
|---|---|---|
| PL011 | waits in DR; FR's RXFE clears; RIS's RXRIS sets | with IMSC's RXIM: IRQ 5; reading DR or ICR clears it |
| CMSDK | with CTRL's RX enable: waits in DATA; STATE's RX full sets | with CTRL's RX interrupt enable: INTSTATUS bit 1, IRQ 0 |
| NS16550A | waits in RBR; LSR's DR sets | none (virt's PLIC is not modelled): poll LSR |
| USART0 | with RXEN0: waits in UDR0; UCSR0A's RXC0 sets | with RXCIE0: vector 18 |

A byte arrives when the host has one: the receiver is offered the next
every 4096 steps while it is empty, and when the core would sleep with
nothing else to wake it, the run waits for the host instead of ending.
Typing into stdin makes an interactive console, and with `--input -` the
output is unbuffered: a program at the other end of a pipe reads each byte
as the UART sends it. Where the host's bytes
fall in the run depends on the host -- how fast the pipe delivers, when
a person types -- and a program that counts its own time sees different
counts each run:

```text
$ (printf ab; sleep 0.3; printf cd; sleep 0.3; printf q) | embsim rr.elf --input -
got a at tick 261
got b at tick 591
got c at tick 12463
got d at tick 12785
got q at tick 24650
```

That is what `--record FILE` is for. It writes everything that came from
outside the machine, each with the step it came at, and the run's end:

- the bytes the UART received (`rx`), and the core it woke when it had
  to wait for one (`wake`);
- semihosting's SYS_READC, a byte of the host's stdin (`readc`);
- a debugger's register and memory writes (`reg`, `mem`, gdb's `load`
  and flash erases among them), its `monitor reset` and `reload`, the
  core it wakes from a sleep nothing else would end, and its `kill`;
- every exception and interrupt the core entered, with its step and
  instruction count (`exc`), which a replay checks: a run of the same one
  at a constant stride -- a timer's tick waking an idle loop while the
  run waits for the host -- is one line, so the record grows with what
  the program does, not with how long a person takes to type;
- the machine: the image (and a hash of it), the board, the core, the
  RAM size, the SVD file, the device pack (`--dfp`), semihosting,
  `--max-insns`, `--until`, and the board's inputs (`--set`), which the
  replay takes from it;
- the end: the steps, the instructions and cycles, how the run ended
  and its status, a hash of the registers and memory, and the output's
  length and hash.

There is no time to record: every counter and timer EmbSim has runs on
the core's estimated cycles, never on the host's clock. A step is a
call of the core's step that did something; one a watchpoint stops
before its access does nothing, and a run without the debugger never
takes it.

```text
@1232 1231 exc 15 x 261 +11 +10
@4096 3835 rx 61
@4099 3837 exc 21
@4575 4312 exc 15 x 329 +11 +10
...
@1189 1189 mem 20000084 29000000
@1189 1189 reg 0 64000000
end 270912 246621 242911779 1 0 669cb360efc5d2fd 34 022fc4ec611bf39b
```

`--replay FILE` runs the recording again: the same machine (an option
that shapes it may be given only as the recording has it; the image is
the recording's unless named, and must hash the same), the recorded
inputs at the recorded steps, no host input and no debugger. Its output,
its `--count` and `--stats`, its exit status and its final state are the
recorded run's; it says so on stderr, or where the two went apart, with
status 5:

```text
embsim: replay: the run is the recording's (270912 steps, 246621 instructions, 17 events)
embsim: replay diverged at step 4097 (3836 instructions): rx: the recording is at 3835 instructions here
embsim: replay diverged: the run ended at
  steps, instructions, cycles, state, status, the state's hash, the output's bytes and hash:
  this run's:      164410 149801 146072424 1 0 c9f06dca0cedf591 99 a408e13e96d3cf61
  the recording's: 164410 149801 146072424 1 0 c9f06dca0cedf591 99 30e7262b9dcebd41
```

A recorded debugging session -- breakpoints, `set var`, a register
changed, a watchpoint -- replays without the debugger, so a run that
went wrong once under gdb can be run again, and `--trace`d, `--profile`d
or debugged again, exactly as it went.

## The code as text: `--disassemble`

`--disassemble` prints the image's code disassembled, and does not run
it (`--disassemble=FILE` to a file): an ELF file's executable segments,
a function's name above its first instruction; a PIC24's HEX image,
its program memory where it is not erased. It knows the instruction
sets the fault report shows -- Thumb, RISC-V, MIPS32 and the PIC24F
(in XC16's mnemonics; f and the addresses in hex, the literals in
decimal) -- which covers a PIC24 image when no `xc16-objdump` is at hand:

```text
$ embsim thermo.elf --board explorer16-32-pic24 --dfp $DFP --disassemble

_main:
  002d2c:  fa0014         lnk #20
  002d2e:  0702ba         rcall 0x0032a4
  002d30:  200100         mov #0x0010, w0
  002d32:  980710         mov w0, [w14+2]
```

Data in the code's segments (a PIC24's interrupt vector table, the
strings of `.const`) prints as `.word`, as does an encoding the
disassembler does not know.

## Debugging: `--gdb`

`--gdb PORT` serves the GDB remote protocol on `localhost:PORT` while the
image runs. `--gdb-wait` holds the core at reset until a debugger
connects, as QEMU's `-S` does. Without it the image runs, and a debugger
that connects stops it where it is; an image that ends first ends EmbSim
as without `--gdb`, but one that waits in a WFI or a loop nothing can
interrupt waits there for the debugger. `HOST:PORT` listens on another
address, and QEMU's `tcp::PORT` is accepted too.

```sh
embsim fw.elf --board mps2-an386 --gdb 1234 --gdb-wait &
gdb fw.elf -ex 'target remote localhost:1234'
lldb fw.elf -o 'gdb-remote localhost:1234'
embdbg fw.elf remote 1234
```

The server answers as QEMU's stub does, so a debugger sees the same
machine on either, and a script written for one runs on the other:

- **Registers.** On a Cortex-M: r0 to r12, sp, lr, pc and xpsr; d0 to
  d15 and fpscr on a core with an FPU (gdb shows s0 to s31 from them);
  and msp, psp, primask, control, and on ARMv7-M basepri and faultmask.
  The target description gdb reads has the FPU only when the core has
  one. On RISC-V: x0 to x31 and pc, f0 to f31 at 64 bits, priv, and the
  CSRs EmbSim has (fflags, frm and fcsr among them), each numbered as
  QEMU's stub numbers it (a CSR is 66 plus its number). QEMU's
  description lists the supervisor's and hypervisor's CSRs too, which
  EmbSim does not have, so `info all-registers` differs there. On the
  AVR: r0 to r31, SREG, SP and the PC (a byte address), and memory as
  gdb's AVR target numbers it -- flash at 0, the data space at
  `0x800000`.
- **Breakpoints**, software and hardware alike. The server keeps them;
  it never writes into the image, so they work in flash.
- **Watchpoints**: `watch`, `rwatch` and `awatch`. As on QEMU, the
  target stops before the access, and gdb steps it (on the AVR, after
  it, as gdb's AVR target expects; QEMU's AVR stub has no working
  watchpoints to referee them against).
- **Stepping**: `stepi` runs one instruction. An exception that is
  pending is taken first, so a step can stop at a handler's first
  instruction.
- **Interrupting** (Ctrl-C, the protocol's 0x03). The server looks for
  it every 16384 instructions.
- **Memory** reads and writes, and `load`. The server gives gdb a memory
  map from the board: flash, RAM, and the device space. gdb programs
  the flash with it, and uses hardware breakpoints there (it says so
  once: "automatically using hardware breakpoints").
- `kill` ends EmbSim with status 0. `detach` leaves the image running to
  its end, as a board does when the probe lets go; EmbSim then exits as
  without a debugger.

A run under a debugger ends differently:

| What happens | What the debugger sees |
|---|---|
| the image exits (semihosting, a reset request, `--until`, `--max-insns`) | the program exited, with the status of the table above |
| the core locks up | a stop with SIGSEGV, at the lockup |
| a WFI or a loop that nothing can interrupt | nothing: the core waits, as a part does, until the debugger interrupts it |

Time is the core's estimated cycles, with or without a debugger: a run
stopped at a breakpoint and continued has the counts of one that never
stopped. A WFI the debugger interrupts completes when it is resumed, as
on a part (halting wakes the core; QEMU's stays asleep).

### `monitor` commands

| Command | Action |
|---|---|
| `monitor reset` | reset the core and the devices, as the reset pin does. Memory keeps what is in it, so an image gdb loaded stays, and the counts start again |
| `monitor reload` | load IMAGE.elf into memory again, then reset |
| `monitor stats` | the instructions run and the estimated cycles |
| `monitor insns`, `monitor cycles` | each alone, as a number |
| `monitor help` | the list |

`monitor insns` at a breakpoint is the number `--max-insns` would stop
a run without a debugger at, one instruction before it.

## Not modelled yet

On the Cortex-M:
- The MPU (MemManage faults).
- ARMv8-M (TrustZone, the Cortex-M23/M33).
- FPv5's VSEL, VMAXNM, VMINNM and VRINT.
- The half-precision and fixed-point VCVT forms.
- The ARMv7E-M parallel add and subtract instructions.
- Peripherals beyond the UART, SysTick, the NVIC and DWT's cycle
  counter, except as an SVD file's registers (`--svd`) and the
  STM32F405's models.
- On the Pico: core 1 (it stays in the bootrom's wait for its vector,
  so `multicore_launch_core1` waits for ever); the bootrom's code and
  functions; the interpolators; DMA, PIO, PWM, the ADC, SPI, I2C, the RTC,
  USB and the XIP cache's controls, which are their registers alone; the
  ROSC's frequency controls, the PLLs' lock time, the clock generators'
  stop and start cycles, GPOUT and GPIN, the resus, SLEEP and DORMANT;
  the watchdog's selections of what it resets, and booting again after
  it; the pads' drive, slew and Schmitt trigger; the UARTs' baud rate
  timing and line format; SysTick's external reference (the microsecond
  tick: SysTick counts the core's clock); the SRAM's unstriped alias at
  `0x21000000`.
- On the nRF52840 DK: PPI and GPIOTE (events reach tasks only through
  SHORTS), the GPIO's LATCH, DETECT and SENSE, the radio, USB, NFC, the
  SAADC, SPI and TWI, the PWMs, QSPI, the crypto engines, the NVMC's
  writes, POWER's modes and the watchdog (their registers keep what is
  written); the UARTs' baud rate, flow control and errors; the TIMER's
  PCLK1M; the RTC's "may not trigger" CC writes; the crystal's power-up
  time.
- On the Arduino Zero: the XOSC and the FDPLL96M (0 Hz), the DFLL's
  USB recovery, its steps and its frequency before the lock, the
  generators' GCLK_IO pins, WRTLOCK, the clocks' on-demand and sleep
  behaviour; the WDT, RTC, EIC, EVSYS, DMAC, USB, ADC, DAC, AC, TCC,
  I2S and PTC, NVMCTRL's commands, the SERCOMs' SPI and I2C modes (their
  registers keep what is written); the TCs' waveform outputs, captures
  and events; the USARTs' baud timing, frame format, errors and flow
  control; the Arduino bootloader and the NVM rows' factory contents.
- On the Blue Pill: the F405's list below, and GPIO's LCKR sequence, the
  alternate functions' outputs but the USARTs' TX, EXTI and the pins'
  interrupts, the backup domain's (BKP, the RTC, LSE), the flash's
  programming, the clock security system; SysTick's HCLK / 8 reference
  (CLKSOURCE 0: SysTick counts HCLK).
- On the STM32F405: DMA, the ADC, SPI, I2C, EXTI and the rest are their
  registers alone; RCC's clock frequencies, its interrupts and its reset
  registers; GPIO's LCKR and pin inputs from outside; the USARTs'
  receivers and baud-rate timing; the timers' capture/compare,
  down-counting, center-aligned modes, repetition counter, slave modes
  and DMA requests.

Each instruction among these is a UsageFault, so a run that needs one
stops at a named fault rather than computing something wrong.

On the AVR:
- Timer/Counter0 and 2, SPI, TWI, the ADC, the analog comparator, EEPROM,
  the watchdog, the external and pin-change interrupts, and the power
  reduction register (the USART's receiver is, with `--input`). Their
  registers keep what
  is written (on QEMU they read as zero), and their interrupts never
  come.
- The output-compare pins and input capture from a pin.
- Self-programming (SPM) and the boot section; the other AVR parts (the
  ATmega2560's 3-byte program counter, ELPM and RAMPZ).
- The sleep modes beyond Idle.

On MIPS:
- The MCU ASE, microMIPS's FPU and DSP encodings (each ends the run as
  a lockup naming the instruction); paired singles.
- Big-endian images (mips-none-elf): EmbSim's MIPS core is
  little-endian.
- The caches (`cache` is a no-op), the watch registers (kept, not
  enforced), the performance counters, EJTAG's debug mode (sdbbp ends
  the run).
- `--stack-report` takes the one stack pointer as the program's single
  stack: an RTOS's task stacks, in its heap below `_end`, read as an
  overflow (give `--stack-limit` an address below them). The GDB
  server gives a MIPS core no target
  description and no memory map, as QEMU's stub does: gdb describes the
  core itself (registers 0-72: the GPRs, Status, LO, HI, BadVAddr,
  Cause, PC, the FPU's as 0), and would compare a map's kseg0 addresses
  with its own sign-extended ones.

On the PIC24F:
- The flash's programming (TBLWT and the NVM controller); the debug
  executive and BREAK.

On the dsPIC33E and PIC24E:
- The dsPIC33C's instructions (LAC.D, SAC.D, MAX, MIN, NORM, FLIM,
  BFEXT, BFINS, DIVF2, DIV2, CTXTSWP: each a lockup); the alternate
  vector table; a trap that stays pending while its flag and condition
  remain (it is taken once); the fixed and variable interrupt latency
  (CORCON.VAR).
- The part's peripherals beyond the interrupt controller, the
  oscillator, the PPS, the timers, the UARTs, the ports, the SPI units,
  ADC1's channel 0 and the PMP (their registers keep what is written).
- The cycle counts are the manual's, but no test holds them to it.
- The GDB server's registers are the PIC24's: the accumulators, DCOUNT,
  DOSTART and DOEND are read as memory.
- No gdb has a PIC24 target: the GDB server's registers are in EmbSim's
  own order (W0-W15, SPLIM, the PC in four bytes, SR, CORCON, DSRPAG,
  DSWPAG, RCOUNT, TBLPAG), which its target description names (feature
  `org.embcc.embsim.pic24.core`) for a client of the protocol.

On RISC-V:
- Supervisor mode and virtual memory, and the hypervisor.
- Physical memory protection: the PMP registers are kept, not
  enforced.
- The PLIC, and so external interrupts; virt's RTC, virtio and PCIe.
- Semihosting on RV64.
- mcountinhibit is kept but does not stop the counters.
- On the ESP32-C3: the ROM (its loader's work is done, its functions are
  not there) and so ESP-IDF; the flash, its cache and MMU; the
  watchdogs (TIMG's MWDT, RTC_CNTL's RWDT, the super watchdog: they
  never reset); the PLL's power-up and the clocks' switching time; the
  core's own timings (its cycle costs are EmbSim's RISC-V's), the
  interrupt controller's 4-cycle latency, debug mode and the trigger
  module (kept), the stall bits; the performance counter's hazard, load,
  store, jump, branch and compressed-instruction events; the GPIO
  matrix's peripheral signals (so the dedicated GPIO CSRs), the pins'
  interrupts, the pads' sleep settings, GPIO_STRAP; the UARTs' baud
  timing, line format, flow control and RXFIFO_TOUT; the timer groups,
  LEDC, RMT, SPI, I2C, I2S, TWAI, the ADC, the GDMA, the crypto
  accelerators, USB Serial/JTAG, Wi-Fi and Bluetooth, and SYSTEM's
  clock gates and peripheral resets (their registers keep what is
  written).
- Extensions beyond IMAFDC, Zicsr and Zifencei (an illegal instruction
  each, as above).

## How it is checked

`tests/golden/embsim.sh` uses QEMU as the referee.

**The exec corpus.** Every `tests/exec` program is built with lib/libc
and lib/rt and run on QEMU and on EmbSim. The output and the exit status
must be the same. It runs on five cores:
- the M3 at -O0;
- the M4 with soft float at -Os;
- the M4 with FPv4-SP at -O2;
- the M7 with FPv5 at -O2;
- the M0 at -O2.

On the first four, the instruction count and the estimated cycles must
be the same as well. QEMU's count comes from tools/bench's plugin. It is
high by a constant, because QEMU counts the boot's first blocks twice,
so the test requires every program to show exactly the constant that a
`main` that only returns shows.

**Exceptions.** `tests/golden/embsim/exc.c` exercises SVC, PendSV,
nested interrupts, PRIMASK and BASEPRI, a handled and an escalated
UsageFault, SysTick waking WFI, a thread on the PSP, and the FPU's
extended frame. Its output must be QEMU's, and the record in
`tests/golden/embsim/exc-*.txt`.

**The ends of a run.** The exit status, the lockup, the idle loop,
`--until` and `--max-insns`.

**The GDB server.** `tests/golden/embsim-gdb.sh` runs the same gdb
session, on the same image, on QEMU's stub and on EmbSim's server, on
the M3 and on the M4 with its FPU, and the transcripts must be the same:
breakpoints, steps, registers, memory, a variable changed, watchpoints,
and the run to its exit. It does the same with an interrupted run, with
lldb, with embdbg and with gdb's `load`, and checks the `monitor`
counts against a run without a debugger.

Writing that test found one QEMU bug. On the mps2-an386, QEMU resumes a
divide that trapped with the instructions before it in its translation
block undone, so the test does not print that quotient.

**SVD files.** `tests/golden/embsim-svd.sh` runs a program over a test
SVD with every access type, modifiedWriteValues and readAction, arrays,
clusters and derivedFrom, and compares each value with the CMSIS-SVD
specification's, the trace and the map with their records; and an
STM32F405 firmware, built from embsvd's header, startup and linker
script, that drives RCC, GPIO, USART1-3 and TIM2, on QEMU's
netduinoplus2 and on EmbSim: its values must be QEMU's where QEMU models
the register, and RM0090's (a record) where it does not or differs.
`tests/golden/embsim-svd-all.sh` maps every SVD file in `$EMBREF/svd`
(ST's and Nordic's) and holds each register's address and size to a
second reading of the file in Python.

**Analysis.** `tests/golden/embsim-coverage.sh` builds
`tests/golden/embsim-an/cov.c`, whose every line with code is marked
with the times it runs, at -O0 -g on the M3, RV32 and the AVR, and with
clang (DWARF 5); the report and the lcov tracefile must give exactly the
marks, line for line, the run must be the same with and without
`--coverage`, and an image without `-g` must give its functions.
`tests/golden/embsim-profile.sh` profiles `embsim-an/prof.c`, whose
calls are known (a loop's, recursion five deep, a call through a pointer,
a timer interrupt), on the M3, RV32 with and without compressed
instructions, and the AVR: the counts must add up to `--stats`' in the
report and in both collapsed forms, each function's self instructions
must be what the `--trace` of the run gives it by llvm-nm's symbols, and
the calls, the paths and the inclusive counts must be prof.c's; and
`exc.c`'s handlers must be entered as often as it counts.
`tests/golden/embsim-stack.sh` measures prof.c's stacks: each frame must
be the `.su` file's, the depth the sum of the deepest path's frames, and
the inclusive depths of the functions embrt bounds embrt's bounds; and it
overflows `ovf.c`'s recursion past `__stack_limit`, `--stack-limit` and
into `.bss`, where the run must stop at the push that crossed.
`tests/golden/embsim-fault.sh` takes `fault.c`'s faults on the M3 and
RV32 -- a BusFault handled, a divide by zero forced to a HardFault with
no handler, a fault in an interrupt handler, a load access fault and an
illegal instruction -- and holds each report to the fault: the status
registers' words, the faulting instruction's mnemonic against
llvm-objdump's, the backtrace against the source lines marked in
fault.c, the handler; and the run must be the same without the report.
`tests/golden/embsim-replay.sh` feeds `rr.c` bytes through a pipe with
pauses, on the PL011 (by interrupt, with SysTick and without), the
CMSDK UART, the NS16550A and the AVR's USART0, and through SYS_READC;
records each run, replays it with other bytes on stdin, and requires the
same output, `--count`, `--stats` and status; replays a gdb session's
writes and watchpoint without gdb; and refuses a record with a byte
changed, a byte a step late, another image or another board.

**The Pico.** No QEMU has an RP2040: `tests/golden/embsim-pico.sh`
builds `embsim-pico/pico.c` from the header, startup and linker script
embsvd writes from RP2040.svd -- embsvd's boot2 against llvm-mc's
assembly of its source and the bootrom's CRC -- and holds EmbSim's ROM
to llvm-mc's assembly of its source and 123 values to the RP2040's
datasheet: the boot as the bootrom and boot2 leave it, a peripheral held
in reset, the crystal's startup delay timed by the timer, the PLL and
clocks as the SDK's `clocks_init` sets them (FC0 measuring each),
UART0's divisors (the datasheet's 67 and 52 at 115200), a byte waiting
for clk_peri and one lost to a full transmitter, the atomic aliases, the
datasheet's `narrow_io_write` example, the timer's 64 bits, latch and
alarm, the watchdog's double count, the LED's blinks 500 us apart,
GPIO_STATUS through the overrides, the pads, an edge's interrupt, the
divider, spinlocks and FIFOs, the flash's IDs and data through the SSI.
And BOOTSEL, UART0's input, a bad boot2 refused, the watchdog's reset,
the Pico SDK's own `boot2_w25q080` (assembled by clang) booting through
the SSI and the flash's status registers; and the exec corpus from the
Pico's flash and boot against QEMU's micro:bit -- the same ARMv6-M --
some 250 programs to the instruction and the cycle, the boot's 28
instructions apart.

**The Blue Pill.** `tests/golden/embsim-bluepill.sh` builds
`embsim-bluepill/bluepill.c` from embsvd's header, startup and linker
script for ST's STM32F103xx.svd and runs it on EmbSim and on QEMU's
stm32vldiscovery: 10 values -- USART1's registers, its interrupt, a
reserved address's bus fault -- must be QEMU's, and 33 RM0008's: RCC
out of reset and the switch to 72 MHz (SW waiting for the PLL, the PLL
for HSE), a clock gate, the pins (pulls, the debug port's until SWJ_CFG
frees them, open-drain, BSRR and BRR, a USART's TX pin), TIM2's update
event and interrupts, SysTick against TIM2; the LED toggled by TIM2's
interrupt 1 ms apart at 72 MHz and 2 ms at 36; USART1's input. And the
exec corpus on the Blue Pill's memory (its SRAM 64 KiB) against QEMU's
netduino2, the same image on both: some 250 programs to the instruction
and the cycle.

**The nRF52840 DK.** `tests/golden/embsim-nrf52840dk.sh` builds
`embsim-nrf52840dk/dk.c` from embsvd's files for Nordic's SVD, as
ARMv6-M code with 16 KiB of RAM, so the same image runs on QEMU's
micro:bit: 14 values (UART0, GPIO, TIMER0 as a counter, INTENSET and
INTENCLR) must be what QEMU's nRF51 prints, and 28 the nRF52840 PS's:
the GPIO's input buffer, pulls and drive modes, P1, a button, the FICR,
the HFXO's 256 us debounce timed by TIMER2, the LFCLK, RTC0's compare
after 32 counts, its read-only PRESCALER, TICK only when enabled, an RTC
without the LFCLK, TRIGOVRFLW, TIMER1's 1 ms interrupt toggling LED1 at
the board's time, UARTE0's EasyDMA from RAM and its refusal from flash,
the input through UART0's RXD and into RAM by UARTE0. And the exec corpus
for the M4 with FPv4-SP on the DK's memory against QEMU's mps2-an386,
the same image on both: some 250 programs to the instruction and the
cycle.

**The Arduino Zero.** `tests/golden/embsim-arduino-zero.sh` builds
`embsim-arduino-zero/zero.c` from embsvd's files for Microchip's SVD and
holds 95 values to the SAM D21 datasheet: the reset values (GCLK's read
through their IDs), a SERCOM whose APB clock is masked, the CPU's clock
measured against OSCULP32K from 1 MHz to the DFLL48M in closed loop on
XOSC32K (CPUDIV; generator 0 staying on OSC8M until XOSC32K runs), a
generator's division with and without DIVSEL, XOSC32K's start-up, the
DFLL's lock with and without its reference (DFLLRCS), PORT's input
buffer, pulls, toggles, WRCONFIG and IOBUS, SERCOM5's enable-protected
registers, DRE without TXEN and nothing sent without its core clock,
TC4's MFRQ wrap and matches (as the counter leaves CCx), STOP,
RETRIGGER, one-shot, counting down and up from above the top, COUNT8
and COUNT32; L toggled by
TC3's interrupt every 1 ms of the DFLL's clock; the three warnings;
SERCOM5's input; erratum 9905's freeze. And the exec corpus on the
Zero's memory (64 KiB of SRAM) against QEMU's micro:bit, the same image:
some 250 programs to the instruction and the cycle.

**The ESP32-C3-DevKitM-1.** `tests/golden/embsim-esp32c3.sh` builds
`embsim-esp32c3/c3.c` from embsvd's files for Espressif's SVD (its
vector table must be 256-aligned in the image) and holds 118 values to
the TRM: the core's CSRs, the exceptions with mcause and mtval, UART0's
reset values and interrupt bits, the clocks at 20, 40, 80 and 160 MHz
against SYSTIMER's 16 MHz, SYSTIMER's loads and comparators (once in
target mode, at once when passed, the 2^51 - 1 boundary), the interrupt
matrix's priorities, threshold and order, an edge type's capture and
clearing, a level type taken again, WFI woken with MIE off, the pads'
reset values pin by pin, GPIO in and out, inverted and open drain; the
RGB LED's frames 1 ms apart from SYSTIMER's period interrupt, decoded
from GPIO8, the first at the time SYSTIMER's UNIT0 gives (the board's
time across four clock changes); UART0's input; the BOOT button. QEMU has no ESP32-C3, but
its virt with an rv32i,m,c,zicsr,zifencei core has the C3's instruction
set: the exec corpus linked for each (RISC-V semihosting for the output
and the exit) runs some 240 programs to the instruction and the cycle;
those that use lib/libc's stdio trap on both (its lock is LR/SC) and
are not counted.

**RISC-V.** `tests/golden/embsim-riscv.sh` does the same on virt with
qemu-system-riscv32 and -riscv64: the exec corpus on RV32 with ilp32
(-O0, -O2, -Os) and ilp32d (-O0, -O2), and on RV64 with lp64 (-O0,
-O2) and lp64d (-O0, -O2, -Os), 2425 programs, each with the output,
the test device's status, the instruction count and the cycles QEMU
gives. QEMU stops its guest some way after the test device asks it to,
so the plugin is told where the driver's last function starts and
counts to there. `tests/golden/embsim/rv-isa.c` prints the edges: M's
division by zero and overflow, the high multiplies, the compressed
forms, misaligned accesses, LR/SC and the AMOs, traps and their CSRs,
and every F and D operation in every rounding mode over a table of
values with its flags. And the ends of a run: the test device, the
lockup, the idle loop and WFI, the timer interrupt waking WFI.
`embsim-gdb.sh` runs its gdb session on RV32 and RV64 against QEMU's
virt stub too.

**AVR.** `tests/golden/embsim-avr.sh` does the same on QEMU's uno: the
exec corpus at -O0, -O2 and -Os, every program that builds for the AVR
without libc (some 580: a few more or fewer as QEMU finishes the longest
in its time), with the output and the instruction count QEMU gives. QEMU's AVR runs a block again from a data access it must redo,
so the plugin counts its instructions one at a time, skipping a repeat
of the same instruction (which only a jump to itself does honestly);
and it counts an instruction a skip passes over, which EmbSim counts
apart. QEMU has no cycle model, so `tests/golden/embsim/avr-cycles.S`
holds the cycles to the datasheet: each of its lines says what it
takes, and EmbSim's count must add up to it. `avr-isa.S` and
`avr-isa.c` print the edges -- SREG after every arithmetic instruction
over a table of operands, the multiplies, the 16-bit forms, the skips,
the branches, the addressing modes, LPM, Timer/Counter1's interrupt --
which must be QEMU's. And `embsim-gdb.sh` runs its gdb session on the
AVR against QEMU's uno stub.

**MIPS.** `tests/golden/embsim-mips.sh` runs the exec corpus at -O0,
-O2 and -Os on QEMU's malta and on EmbSim's (the output and the exit
status), `mips-isa.c`'s instruction edges, `mips-fpu.c`'s FPU edges,
`mips-dsp.c`'s DSP ASE edges, `mips-micro.s`'s microMIPS ones
(assembled by llvm-mc) and `mips-mips16.s`'s MIPS16e ones (built of
`.hword`s by its macros: llvm-mc has no MIPS16 assembler) against QEMU's
24Kc, P5600, 74Kf, M14Kc and 24Kc, and the ends of a run.

**PIC32.** No QEMU has a PIC32. `tests/golden/embsim-pic32.sh` holds the
register file the PIC32MZ2048EFH100's ATDF gives to the pack's own
header (every SFR and its CLR, SET and INV at their addresses), and an
EmbCC program on each PIC32 board to its record: the DSP ASE after
EmbCC's startup, the EVIC, shadow sets, the timers, the ports, the ADC,
the LCD on the PMP, SYSKEY on the Explorer; ANSELx, the switches and the
LEDs on the Curiosity.

**PIC32MX.** `tests/golden/embsim-pic32mx.sh` holds the EDC's register
file to the pack's device header (every SFR and its CLR, SET and INV
registers at the header's address; the header's other names aliases of
them), and runs EmbCC programs against their records: on the bare
PIC32MX795F512L, the M4K's identity, DEVID, FPBDIV, the interrupt
controller in single- and multi-vector modes (INTSTAT, the shadow set by
SS0 and by FSRSSEL, the order by vector rather than by request number,
subpriority, a vector three requests share), Timer1 on the peripheral
bus, the change notices, CNPUE, AD1PCFG and SYSKEY's lock, each by the
family reference manual; on `explorer16-32-pic32mx`, the buttons, LEDs,
ADC1's conversions of the board's sensors, the EEPROM on SPI2 and the
LCD on the PMP; and on the PIC32MX250F128B by name, its memory.

**dsPIC33E.** No QEMU has a dsPIC either: `tests/golden/embsim-dspic33.sh`
holds the dsPIC33EP512MU810's register file to the pack's linker
script, and the DSP engine and the PIC24E's instructions to 68 examples
of the programmer's reference manual and the data memory section's
modulo and bit-reversed buffers (`dspic33-isa.s`, generated as
`pic24-isa.s` is: a DO placed at its example's address, the data moved
into this part's RAM and Y space; the examples that contradict their
own instruction's description left out, and named). `dspic33-dsp.s`
holds what the examples leave out to the descriptions: the stack frame
flag through a CALL.L, the computed branches, compare and branch,
REPEAT's 15 and 16 bits, nested DO loops and the DO stack's overflow,
EDT, each saturation mode at its limit, rounding both ways, SATDW, the
multiplier's unsigned and mixed modes, the math error traps INTCON1
enables, the Y modulo buffer, the other space read as 0, an offset
address in an X modulo buffer, and what bit-reversed addressing leaves
alone. `--disassemble` gives each example the manual's text.
`dspic33-part.s` runs on parts whose memory differs (each FOSCSEL where
its EDC puts it, its RAM's end, its Y space's start; a PIC24EP's lack of
the DSP engine), and `dspic33-board.s` drives `explorer16-32-dspic33`
on the FRC with the PLL (the board's reports at its clock): Timer1's
interrupt, the LEDs, the buttons, the sensors through ADC1 (10 and 12
bits, a signed fraction), the LCD on the PMP, the EEPROM on SPI2.

**PIC24.** No QEMU has a PIC24, and no PIC24 assembler is at hand:
`tests/golden/embsim-pic24.sh` holds the EDC's register file to the
pack's linker script (every SFR at its address), and the instruction
set to the examples of the programmer's reference manual: 227 of them,
each case setting the example's Before state and printing its After
state (`pic24-isa.s`, assembled by `pic24-asm.awk`, a few instructions'
assembler, into an Intel HEX image), which must be what the manual
prints -- the examples that contradict their own instruction's
description are left out, and named. `pic24-board.s` drives the
peripherals and the Explorer 16/32's PIC24 PIM: Timer1's interrupt, the
LEDs, the buttons, the sensors through ADC1, the LCD through the EPMP,
the EEPROM through SPI1 and the Peripheral Pin Select. `pic24-periph.s`
holds the part's other peripherals to the data sheet: the
interrupt-on-change, the flash controller (its latches, the unlock, a
program and an erase read back by TBLRD, WRERR), the RTCC (its carries
from 23:59:58 on 31 December 2099 to 2000, its alarm, WRLOCK, a
timestamp), the DMA controller (a copy on CHREQ, DMAH's limit, a channel
Timer2 triggers), I2C1 on an empty bus, OC1's pulse and PWM captured
through the PPS by IC1 and IC2 synchronized to it, and SCCP5's PWM
captured by SCCP4 on the same pin.
