/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef OPENOCD_JTAG_DRIVERS_ESP_GPIO_H
#define OPENOCD_JTAG_DRIVERS_ESP_GPIO_H

#include <driver/gpio.h>
#include "hal/gpio_ll.h"

extern gpio_dev_t *const s_gpio_dev;
extern unsigned int s_gpio_delay;
extern int s_tck_gpio;
extern int s_tms_gpio;
extern int s_tdi_gpio;
extern int s_tdo_gpio;
extern int s_trst_gpio;
extern int s_srst_gpio;
extern int s_blink_gpio;
extern int s_swdio_gpio;
extern int s_swclk_gpio;

/*
 * TRST / SRST / BLINK 的 dedicated 通道位掩码。
 *
 * JTAG 输出数组的前三个(TCK/TDI/TMS)位置固定, 掩码可以写成常量; 但这三个是
 * "按需追加"的 —— 通道位置随前面有几个可选引脚而变, 所以只能在创建 bundle
 * 的时候算出来存到这里。未配置的项保持 0(调用方据此直接跳过)。
 */
extern uint32_t s_trst_mask;
extern uint32_t s_srst_mask;
extern uint32_t s_blink_mask;

/* GPIO setup functions */
static inline void gpio_mode_input_output_set(int g)
{
	gpio_ll_input_enable(s_gpio_dev, g);
	gpio_ll_output_enable(s_gpio_dev, g);
}

static inline void gpio_set(int g)
{
	gpio_ll_set_level(s_gpio_dev, g, 1);
}

static inline void gpio_clear(int g)
{
	gpio_ll_set_level(s_gpio_dev, g, 0);
}

#if CONFIG_SOC_DEDICATED_GPIO_SUPPORTED

/* esp-idf includes */
#include <driver/dedic_gpio.h>
#include <hal/dedic_gpio_cpu_ll.h>

/*
 * 掩码值 = 该引脚在 bundle 数组里的**索引**(对应 dedicated 通道的一个位)。
 *
 * ★ SWD 数组布局(2026-09-19 重排, 理由见下):
 *     输出 bundle[0] = SWCLK  -> 0x01
 *     输出 bundle[1] = SWDIO  -> 0x02   ← 恒定在此索引, 与 blink 有无无关
 *     输出 bundle[2] = blink  -> 0x04   (可选; 不配就不追加, 前两位不受影响)
 *     输入 bundle[0] = SWDIO  -> 0x01   (in 通道)
 *
 *   旧布局是 "输出 bundle = {SWCLK, blink}" + "io bundle = {SWDIO}", 于是 SWDIO
 *   的输出通道偏移取决于"输出数组里有几项"。而数组用 `= { 0 }` 初始化、blink 未
 *   配置时那一项就**保持 0** —— 0 是合法引脚 GPIO0! dedic_gpio_new_bundle() 会照样
 *   把 GPIO0 连到 dedicated 输出通道, 而 S3 的该通路输出是自动使能的 ⇒ GPIO0 被
 *   持续驱动为 0 且关不掉(与 SWDIO 那个故障同一机理)。
 *   实测旁证: 旧布局下 SWDIO 拿到的是 PRO_ALONEGPIO_OUT2_IDX(223) 而非 OUT1(222),
 *   说明通道 1 确实被那个空洞(GPIO0)占掉了。
 *   新布局把 SWDIO 放进输出数组的固定索引 1 ⇒ 不会再有空洞, 掩码也不再依赖数组大小。
 */
#define GPIO_SWCLK_MASK			0x01	/* bundle_out_gpios[0] */
#define GPIO_SWDIO_OUT_MASK		0x02	/* bundle_out_gpios[1] */
#define GPIO_SWD_BLINK_MASK		0x04	/* bundle_out_gpios[2], 可选 */
#define GPIO_SWD_OUT_MAX		3		/* 输出数组最大长度(含可选 blink) */

/* SWD 输入: 输入 bundle 只含 SWDIO, 位于 in 通道 0 */
#define GPIO_SWDIO_IN_MASK		0x01

/* JTAG input pins mask values */
#define GPIO_TDO_MASK			0x01	/* bundle_in_gpios[0] */
#define GPIO_JTAG_IN_MAX		1

/*
 * JTAG output pins mask values.
 * TCK/TDI/TMS 恒定在前三位, 掩码可以写死; TRST/SRST/BLINK 按需追加,
 * 位置会变 ⇒ 掩码在运行时算出(s_trst_mask / s_srst_mask / s_blink_mask)。
 */
#define GPIO_TCK_MASK			0x01
#define GPIO_TDI_MASK			0x02
#define GPIO_TMS_MASK			0x04
#define GPIO_JTAG_OUT_MAX		6

static inline int esp_gpio_swdio_read(void)
{
	/*
	 * ★ 必须显式取位: dedic_gpio_cpu_ll_read_in() 返回的是**整个** CPU dedicated
	 *   输入寄存器, 不是一位。当前只有 SWDIO 一个 in 通道, 恰好等价; 但一旦别的
	 *   组件也占用 dedicated in 通道、或通道偏移不是 0, `if (read())` 就会把别人
	 *   的电平当成 SWDIO(只要非 0 就判为高) ⇒ 按掩码取位才是稳的写法。
	 */
	return dedic_gpio_cpu_ll_read_in() & GPIO_SWDIO_IN_MASK;
}

static inline int esp_gpio_swd_write(int swclk, int swdio)
{
	dedic_gpio_cpu_ll_write_mask(GPIO_SWCLK_MASK, swclk ? GPIO_SWCLK_MASK : 0);
	dedic_gpio_cpu_ll_write_mask(GPIO_SWDIO_OUT_MASK, swdio ? GPIO_SWDIO_OUT_MASK : 0);

	for (unsigned int i = 0; i < s_gpio_delay; i++)
		asm volatile ("");

	return ESP_OK;
}

static inline int esp_gpio_swd_blink(int on)
{
	if (!s_blink_mask)
		return ERROR_OK;
	dedic_gpio_cpu_ll_write_mask(s_blink_mask, on ? s_blink_mask : 0);
	return ERROR_OK;
}

static inline int esp_gpio_jtag_read(void)
{
	/* 同 esp_gpio_swdio_read(): 必须按掩码取位, 不能直接返回整个寄存器 */
	return dedic_gpio_cpu_ll_read_in() & GPIO_TDO_MASK;
}

static inline int esp_gpio_jtag_write(int tck, int tms, int tdi)
{
	dedic_gpio_cpu_ll_write_mask(GPIO_TMS_MASK, tms ? GPIO_TMS_MASK : 0);
	dedic_gpio_cpu_ll_write_mask(GPIO_TDI_MASK, tdi ? GPIO_TDI_MASK : 0);
	dedic_gpio_cpu_ll_write_mask(GPIO_TCK_MASK, tck ? GPIO_TCK_MASK : 0);

	for (unsigned int i = 0; i < s_gpio_delay; i++)
		asm volatile ("");

	return ERROR_OK;
}

static int esp_gpio_jtag_blink(int on)
{
	if (!s_blink_mask)
		return ERROR_OK;
	dedic_gpio_cpu_ll_write_mask(s_blink_mask, on ? s_blink_mask : 0);
	return ERROR_OK;
}

/* (1) assert or (0) deassert reset lines */
static inline int esp_gpio_jtag_reset(int trst, int srst)
{
	/* 掩码为 0 表示该引脚没配(也就没进 bundle), 直接跳过 */
	if (s_trst_mask)
		dedic_gpio_cpu_ll_write_mask(s_trst_mask, trst ? s_trst_mask : 0);

	if (s_srst_mask)
		dedic_gpio_cpu_ll_write_mask(s_srst_mask, srst ? s_srst_mask : 0);

	return ERROR_OK;
}

#else

static inline int esp_gpio_swdio_read(void)
{
	return gpio_ll_get_level(s_gpio_dev, s_swdio_gpio);
}

static inline int esp_gpio_swd_write(int swclk, int swdio)
{
	gpio_ll_set_level(s_gpio_dev, s_swclk_gpio, swclk ? 1 : 0);
	gpio_ll_set_level(s_gpio_dev, s_swdio_gpio, swdio ? 1 : 0);

	for (unsigned int i = 0; i < s_gpio_delay; i++)
		asm volatile ("");

	return ESP_OK;
}

static inline int esp_gpio_swd_blink(int on)
{
	gpio_ll_set_level(s_gpio_dev, s_blink_gpio, on ? 1 : 0);
	return ERROR_OK;
}

static inline int esp_gpio_jtag_read(void)
{
	return gpio_ll_get_level(s_gpio_dev, s_tdo_gpio);
}

static inline int esp_gpio_jtag_write(int tck, int tms, int tdi)
{
	gpio_ll_set_level(s_gpio_dev, s_tms_gpio, tms ? 1 : 0);
	gpio_ll_set_level(s_gpio_dev, s_tdi_gpio, tdi ? 1 : 0);
	gpio_ll_set_level(s_gpio_dev, s_tck_gpio, tck ? 1 : 0);

	for (unsigned int i = 0; i < s_gpio_delay; i++)
		asm volatile ("");

	return ERROR_OK;
}

static int esp_gpio_jtag_blink(int on)
{
	gpio_ll_set_level(s_gpio_dev, s_blink_gpio, on ? 1 : 0);
	return ERROR_OK;
}

/* (1) assert or (0) deassert reset lines */
static inline int esp_gpio_jtag_reset(int trst, int srst)
{
	if (s_trst_gpio != GPIO_NUM_NC)
		gpio_ll_set_level(s_gpio_dev, s_trst_gpio, trst ? 1 : 0);

	if (s_srst_gpio != GPIO_NUM_NC)
		gpio_ll_set_level(s_gpio_dev, s_srst_gpio, srst ? 1 : 0);

	return ERROR_OK;
}

#endif /* CONFIG_SOC_DEDICATED_GPIO_SUPPORTED */

#endif /* OPENOCD_JTAG_DRIVERS_ESP_GPIO_H */
