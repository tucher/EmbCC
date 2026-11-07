/* devices.h -- the cores and devices boards.c can build a board from.
 * Each device is a struct dev_ops and a create function returning its
 * context; each core a create function (struct core_type). */
#ifndef EMBSIM_DEVICES_H
#define EMBSIM_DEVICES_H

#include "sim.h"

/* cores */
struct cpu *cortexm_create(struct sim *s, const char *model,
                           const struct board_desc *bd);
struct cpu *riscv_create(struct sim *s, const char *model,
                         const struct board_desc *bd);
int riscv_is(const struct cpu *c);
struct cpu *avr_create(struct sim *s, const char *model,
                       const struct board_desc *bd);
int avr_is(const struct cpu *c);
struct cpu *mips_create(struct sim *s, const char *model,
                        const struct board_desc *bd);
struct cpu *pic24_create(struct sim *s, const char *model,
                         const struct board_desc *bd);

/* devices */
extern const struct dev_ops pl011_ops, cmsdk_uart_ops, nrf51_uart_ops;
extern const struct dev_ops systick_ops, scs_ops, dwt_ops, zero_ops;
void *pl011_create(struct sim *s, const struct dev_desc *d);
void *cmsdk_uart_create(struct sim *s, const struct dev_desc *d);
void *nrf51_uart_create(struct sim *s, const struct dev_desc *d);
void *systick_create(struct sim *s, const struct dev_desc *d);
void *scs_create(struct sim *s, const struct dev_desc *d);
void *dwt_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops clint_ops, ns16550a_ops, sifive_test_ops;
extern const struct dev_ops virt_rom_ops;
void *clint_create(struct sim *s, const struct dev_desc *d);
void *ns16550a_create(struct sim *s, const struct dev_desc *d);
void *sifive_test_create(struct sim *s, const struct dev_desc *d);
void *virt_rom_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops avr_io_ops, avr_usart_ops, avr_timer16_ops;
void *avr_io_create(struct sim *s, const struct dev_desc *d);
void *avr_usart_create(struct sim *s, const struct dev_desc *d);
void *avr_timer16_create(struct sim *s, const struct dev_desc *d);
extern const struct dev_ops ns16550a_8_ops, malta_fpga_ops, malta_rom_ops;
void *malta_fpga_create(struct sim *s, const struct dev_desc *d);
void *malta_rom_create(struct sim *s, const struct dev_desc *d);

/* the STM32's peripheral models, over its SVD's registers (svd-map.h) */
struct rf_periph;
extern const struct dev_ops stm32_rcc_ops, stm32_gpio_ops, stm32_usart_ops;
extern const struct dev_ops stm32_tim_ops;
void *stm32_rcc_create(struct sim *s, struct rf_periph *p);
void *stm32_gpio_create(struct sim *s, struct rf_periph *p);
void *stm32_usart_create(struct sim *s, struct rf_periph *p);
void *stm32_tim_create(struct sim *s, struct rf_periph *p);
/* an STM32F1's GPIO (CRL/CRH), and the board's pins changing under it */
extern const struct dev_ops stm32f1_gpio_ops;
void *stm32f1_gpio_create(struct sim *s, struct rf_periph *p);
void stm32f1_pins_changed(struct sim *s);

/* a PIC32's peripheral models, over its ATDF's registers (pic32.h) */
extern const struct dev_ops pic32_evic_ops, pic32_cfg_ops, pic32_cru_ops;
extern const struct dev_ops pic32_tmr_ops, pic32_gpio_ops, pic32_uart_ops;
extern const struct dev_ops pic32_pmp_ops, pic32_adc_ops;
void *pic32_evic_create(struct sim *s, struct rf_periph *p);
void *pic32_cfg_create(struct sim *s, struct rf_periph *p);
void *pic32_cru_create(struct sim *s, struct rf_periph *p);
void *pic32_tmr_create(struct sim *s, struct rf_periph *p);
void *pic32_gpio_create(struct sim *s, struct rf_periph *p);
void *pic32_uart_create(struct sim *s, struct rf_periph *p);
void *pic32_pmp_create(struct sim *s, struct rf_periph *p);
void *pic32_adc_create(struct sim *s, struct rf_periph *p);
extern const struct dev_ops pic32_spi_ops, pic32_adc10_ops;
void *pic32_spi_create(struct sim *s, struct rf_periph *p);
void *pic32_adc10_create(struct sim *s, struct rf_periph *p);
/* a dsPIC33E's or PIC24E's: the PMP (pic32-pmp.c's, its EDC's names), the
 * SPI units, ADC1 */
extern const struct dev_ops pic24e_pmp_ops, pic24e_spi_ops, pic24e_adc_ops, pic24e_i2c_ops;
void *pic24e_spi_create(struct sim *s, struct rf_periph *p);
void *pic24e_i2c_create(struct sim *s, struct rf_periph *p);
void *pic24e_adc_create(struct sim *s, struct rf_periph *p);

/* a PIC24's peripheral models, over its EDC's registers (pic24.h) */
extern const struct dev_ops pic24_intc_ops, pic24_adc_ops, pic24_epmp_ops;
extern const struct dev_ops pic24_spi_ops, pic24_uart_ops, pic24_tmr_ops;
extern const struct dev_ops pic24_osc_ops, pic24_pps_ops, pic24_nvm_ops;
extern const struct dev_ops pic24_rtcc_ops;
void *pic24_intc_create(struct sim *s, struct rf_periph *p);
void *pic24_uart_create(struct sim *s, struct rf_periph *p);
void *pic24_tmr_create(struct sim *s, struct rf_periph *p);
void *pic24_osc_create(struct sim *s, struct rf_periph *p);
void *pic24_pps_create(struct sim *s, struct rf_periph *p);
void *pic24_adc_create(struct sim *s, struct rf_periph *p);
void *pic24_epmp_create(struct sim *s, struct rf_periph *p);
void *pic24_spi_create(struct sim *s, struct rf_periph *p);
void *pic24_nvm_create(struct sim *s, struct rf_periph *p);
void *pic24_rtcc_create(struct sim *s, struct rf_periph *p);
/* the hardware sets interrupt irq's flag (IFSx), as the EDC numbers it;
 * the flag; an interrupt's number by the EDC's name (T1, ADC1), or -1 */
void pic24_irq(struct sim *s, int irq);
/* and at time `when` (struct sim's cycles), within the instruction: a
 * model that keeps time by the cycle (the capture and compare modules) */
void pic24_irq_at(struct sim *s, int irq, u64 when);
int pic24_irq_flag(struct sim *s, int irq);
int pic24_irq_enabled(struct sim *s, int irq);
int pic24_irq_named(struct sim *s, const char *name);
/* fn told of each event as it happens: an interrupt's (its number,
 * whatever its flag: the DMA's triggers), and the sync outputs of the
 * output compare and CCP modules (PIC24_EV_OC_SYNC + n, its timer
 * matching OCnRS; PIC24_EV_CCP_SYNC + n, its time base resetting), the
 * sources the capture and compare modules synchronize to. pic24_event
 * tells them of one, with its time; pic24_event_named names it as
 * SYNCSEL's tables do (T2, IC1, CCP4, ADC1; OC1sync, CCP2sync) */
#define PIC24_EV_OC_SYNC 1000
#define PIC24_EV_CCP_SYNC 1100
void pic24_irq_listen(struct sim *s, void (*fn)(void *ctx, int ev, u64 when),
                      void *ctx);
void pic24_event(struct sim *s, int ev, u64 when);
int pic24_event_named(struct sim *s, const char *name);
/* Timer n's (1 to 5) instruction cycles a count while it runs on the
 * instruction clock, else 0 (pic24-tmr.c) */
u32 pic24_timer_div(struct sim *s, int n);
/* a PIC24's pins (pic32-gpio.c): a pin by a name the EDC gives it (RG6,
 * OCM1A, RP21, IC4) as port * 32 + bit, or -1; the level it is at; the
 * pin the PPS maps an input to (its RPINRx field: "IC1R"), or -1 */
int pic24_pin(struct sim *s, const char *name);
int pic24_pin_level(struct sim *s, int pin);
int pic24_pin_of_input(struct sim *s, const char *field);
/* a peripheral's output, from time `when` (struct sim's cycles): the PPS
 * output function fn (the data sheet's Table 11-4 numbers it) or a pin
 * of the peripheral's own at level 0 or 1, or -1 when it lets the pin go.
 * Driven, the pin is the peripheral's, whatever TRISx and LATx say */
void pic24_pps_out(struct sim *s, int fn, int level, u64 when);
void pic24_pin_out(struct sim *s, int pin, int level, u64 when);
/* fn told when a pin may have changed level (the time it did), and the
 * ports told of a change the PPS made */
void pic24_pin_watch(struct sim *s, void (*fn)(void *ctx, u64 when), void *ctx);
void pic24_pins_changed(struct sim *s);
extern const struct dev_ops pic24_dma_ops;
void *pic24_dma_create(struct sim *s, struct rf_periph *p);
extern const struct dev_ops pic24_i2c_ops, pic24_ic_ops, pic24_oc_ops, pic24_ccp_ops;
void *pic24_i2c_create(struct sim *s, struct rf_periph *p);
void *pic24_ic_create(struct sim *s, struct rf_periph *p);
void *pic24_oc_create(struct sim *s, struct rf_periph *p);
void *pic24_ccp_create(struct sim *s, struct rf_periph *p);

/* the PIC32 and PIC24 boards' own: LEDs, buttons, the Explorer 16/32's
 * sensors, LCD and EEPROM (a dev_desc's type names the board) */
extern const struct dev_ops pic_board_ops;
void *pic_board_create(struct sim *s, const struct dev_desc *d);

/* the RP2040's ROM (its boot), and its models over RP2040.svd (rp2040.h) */
extern const struct dev_ops rp2040_rom_ops, rp2040_resets_ops, rp2040_clocks_ops;
extern const struct dev_ops rp2040_xosc_ops, rp2040_rosc_ops, rp2040_pll_ops;
extern const struct dev_ops rp2040_psm_ops, rp2040_watchdog_ops, rp2040_timer_ops;
extern const struct dev_ops rp2040_io_ops, rp2040_pads_ops, rp2040_sio_ops;
extern const struct dev_ops rp2040_uart_ops, rp2040_ssi_ops;
void *rp2040_rom_create(struct sim *s, const struct dev_desc *d);
void *rp2040_resets_create(struct sim *s, struct rf_periph *p);
void *rp2040_clocks_create(struct sim *s, struct rf_periph *p);
void *rp2040_xosc_create(struct sim *s, struct rf_periph *p);
void *rp2040_rosc_create(struct sim *s, struct rf_periph *p);
void *rp2040_pll_create(struct sim *s, struct rf_periph *p);
void *rp2040_psm_create(struct sim *s, struct rf_periph *p);
void *rp2040_watchdog_create(struct sim *s, struct rf_periph *p);
void *rp2040_timer_create(struct sim *s, struct rf_periph *p);
void *rp2040_io_create(struct sim *s, struct rf_periph *p);
void *rp2040_pads_create(struct sim *s, struct rf_periph *p);
void *rp2040_sio_create(struct sim *s, struct rf_periph *p);
void *rp2040_uart_create(struct sim *s, struct rf_periph *p);
void *rp2040_ssi_create(struct sim *s, struct rf_periph *p);

/* the nRF52840's models over nrf52840.svd, and its FICR (nrf52.c) */
extern const struct dev_ops nrf_clock_ops, nrf_gpio_ops, nrf_uart_ops;
extern const struct dev_ops nrf_timer_ops, nrf_rtc_ops, nrf52_ficr_ops;
void *nrf_clock_create(struct sim *s, struct rf_periph *p);
void *nrf_gpio_create(struct sim *s, struct rf_periph *p);
void *nrf_uart_create(struct sim *s, struct rf_periph *p);
void *nrf_timer_create(struct sim *s, struct rf_periph *p);
void *nrf_rtc_create(struct sim *s, struct rf_periph *p);
void *nrf52_ficr_create(struct sim *s, const struct dev_desc *d);
void nrf_pins_changed(struct sim *s);

/* the ESP32-C3's models over esp32c3.svd (esp32c3.c): its interrupt
 * matrix, which a source's level reaches through esp32c3_source; the CPU
 * clock, at which the board keeps its time */
extern const struct dev_ops esp32c3_matrix_ops, esp32c3_system_ops;
extern const struct dev_ops esp32c3_systimer_ops, esp32c3_uart_ops;
extern const struct dev_ops esp32c3_gpio_ops, esp32c3_iomux_ops, esp32c3_rtc_ops;
void *esp32c3_matrix_create(struct sim *s, struct rf_periph *p);
void *esp32c3_system_create(struct sim *s, struct rf_periph *p);
void *esp32c3_systimer_create(struct sim *s, struct rf_periph *p);
void *esp32c3_uart_create(struct sim *s, struct rf_periph *p);
void *esp32c3_gpio_create(struct sim *s, struct rf_periph *p);
void *esp32c3_iomux_create(struct sim *s, struct rf_periph *p);
void *esp32c3_rtc_create(struct sim *s, struct rf_periph *p);
void esp32c3_source(struct sim *s, int src, int level);
u32 esp32c3_cpu_hz(struct sim *s);
void esp32c3_pins_changed(struct sim *s);

/* the SAM D21's models over its SVD (samd21.c): the clock tree (SYSCTRL,
 * GCLK, PM) and the board's time, NVMCTRL, PORT and its IOBUS, the
 * SERCOMs as USARTs, TC3-5 */
extern const struct dev_ops samd21_sysctrl_ops, samd21_gclk_ops, samd21_pm_ops;
extern const struct dev_ops samd21_nvmctrl_ops, samd21_port_ops, samd21_iobus_ops;
extern const struct dev_ops samd21_sercom_ops, samd21_tc_ops;
void *samd21_sysctrl_create(struct sim *s, struct rf_periph *p);
void *samd21_gclk_create(struct sim *s, struct rf_periph *p);
void *samd21_pm_create(struct sim *s, struct rf_periph *p);
void *samd21_nvmctrl_create(struct sim *s, struct rf_periph *p);
void *samd21_port_create(struct sim *s, struct rf_periph *p);
void *samd21_sercom_create(struct sim *s, struct rf_periph *p);
void *samd21_tc_create(struct sim *s, struct rf_periph *p);
u32 samd21_gclk_hz(struct sim *s, int id);
u32 samd21_cpu_hz(struct sim *s);
void samd21_pins_changed(struct sim *s);

/* the boards around the microcontrollers: LEDs and buttons (mcu-boards.c;
 * a dev_desc's type names the board) */
extern const struct dev_ops mcu_board_ops;
void *mcu_board_create(struct sim *s, const struct dev_desc *d);

#endif
