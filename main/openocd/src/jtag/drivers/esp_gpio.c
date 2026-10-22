/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include <transport/transport.h>
#include "bitbang.h"

#include "soc/gpio_sig_map.h"	/* SIG_GPIO_OUT_IDX */
#include "esp_gpio.h"

static uint32_t s_gpio_conf;

/*
 * 释放引脚时写进 OUT_SEL 的值: "普通 GPIO 输出"路由(定义见 soc/gpio_sig_map.h,
 * ESP32/S2/S3 为 256, C3 为 128)。配合 GPIO_ENABLE=0 即可让引脚真正进高阻。
 *
 * ★ 必须是**编译期常量**, 不能去读寄存器当前值 —— 踩过的坑(2026-09-19):
 *   最初实现是"在创建 bundle 之前读一次该引脚的 OUT_SEL_CFG, 存下来当空闲值用"。
 *   冷启动(按板上 RESET 键 = 硬件复位)时 GPIO 外设是复位默认值, 一切正常;
 *   但**网页触发的 esp_restart() 是 CPU 软复位, GPIO 外设寄存器不复位**。若上一次
 *   运行结束时该引脚正停在 dedicated 路由上(每笔事务结束时就是), 这次读到的
 *   "空闲值"就是 dedicated 路由本身 ⇒ 摘路由成了空操作 ⇒ 释放再次失效。
 *   现象: "网页重启之后就连不上, 必须按 ESP32 的 RESET 键才行"。
 *   直接证据: 同一次自检里 [A](用读来的值)= 0, 而 [I](显式写 SIG_GPIO_OUT_IDX)= 1。
 */
#define ESP_GPIO_PLAIN_OUT_SEL ((uint32_t)SIG_GPIO_OUT_IDX)
static int s_gpio_speed_khz;

gpio_dev_t *const s_gpio_dev = GPIO_LL_GET_HW(GPIO_PORT_0);
unsigned int s_gpio_delay;
int s_tck_gpio = GPIO_NUM_NC;
int s_tms_gpio = GPIO_NUM_NC;
int s_tdi_gpio = GPIO_NUM_NC;
int s_tdo_gpio = GPIO_NUM_NC;
int s_trst_gpio = GPIO_NUM_NC;
int s_srst_gpio = GPIO_NUM_NC;
int s_blink_gpio = GPIO_NUM_NC;
int s_swdio_gpio = GPIO_NUM_NC;
int s_swclk_gpio = GPIO_NUM_NC;

/*
 * TRST/SRST/BLINK 的 dedicated 通道位掩码(声明见 esp_gpio.h)。
 * 这三个在输出数组里是"按需追加"的, 通道位置随前面有几个可选引脚而变,
 * 所以只能在创建 bundle 时算出来。
 * 为 0 表示未配置 —— 并且**绝不能**用 0 去填充数组空槽位: 0 是合法引脚
 * GPIO0, dedic_gpio_new_bundle() 会真的占用它(见 esp_jtag_dedic_gpio_mode_set)。
 */
uint32_t s_trst_mask;
uint32_t s_srst_mask;
uint32_t s_blink_mask;

static int esp_gpio_khz(int khz, int *speed)
{
	int cycles, extra;

	if (!khz) {
		LOG_DEBUG("RCLK not supported");
		return ERROR_FAIL;
	}
	s_gpio_speed_khz = khz;

	/*
	 * 这个 bitbang 驱动不做分频, 降速只能靠在每个半周期里插一段空循环
	 * (见 esp_gpio.h 里 4 处 `for (i = 0; i < s_gpio_delay; i++)`)。
	 *
	 * 上游原本写死 `*speed = 0`, 于是 adapter speed 完全是个死旋钮 ——
	 * 不管 cfg 里写多少, SWCLK 都恒定跑在最快档(240 MHz CPU 下约 2 MHz)。
	 * 这里把它接通:
	 *   一个 SWCLK 周期 = 240000 / khz 个 CPU 周期;
	 *   空转时约 120 个周期(2 个半周期, 每个约 60);
	 *   一次空循环迭代约 4 个周期, 两个半周期共约 8 个。
	 * 所以补的迭代数 = (目标周期数 - 120) / 8。
	 *
	 * 校准点: 2000 kHz → 0(等同原行为); 1000 → 15; 500 → 45; 250 → 105。
	 *
	 * 副作用: cfg 里原有的 `adapter speed 1000` 从此会真的生效, 也就是把
	 * SWCLK 从 ~2 MHz 降到 ~1 MHz(时序余量翻倍, 对杜邦线这种边际链路是
	 * 好事)。想跑回原来的全速, 写 2000 或更大即可。
	 */
	cycles = 240000 / khz;          /* 一个 SWCLK 周期的目标 CPU 周期数 */
	extra = cycles - 120;           /* 扣掉 bitbang 本身的开销 */
	*speed = extra > 0 ? extra / 8 : 0;
	return ERROR_OK;
}

static int esp_gpio_speed_div(int speed, int *khz)
{
	*khz = s_gpio_speed_khz;
	return ERROR_OK;
}

static int esp_gpio_speed(int speed)
{
	s_gpio_delay = speed;
	/* 打印出来, 启动日志里就能确认"降速到底有没有生效" */
	LOG_INFO("esp_gpio: requested %d kHz -> bitbang delay %d (%s)",
		s_gpio_speed_khz, speed, speed ? "throttled" : "fastest");
	return ERROR_OK;
}

static bool esp_gpio_jtag_mode_possible(void)
{
	if (!GPIO_IS_VALID_OUTPUT_GPIO(s_tck_gpio))
		return 0;
	if (!GPIO_IS_VALID_OUTPUT_GPIO(s_tms_gpio))
		return 0;
	if (!GPIO_IS_VALID_OUTPUT_GPIO(s_tdi_gpio))
		return 0;
	if (!GPIO_IS_VALID_GPIO(s_tdo_gpio))
		return 0;
	return 1;
}

static bool esp_gpio_swd_mode_possible(void)
{
	if (!GPIO_IS_VALID_OUTPUT_GPIO(s_swclk_gpio))
		return 0;
	if (!GPIO_IS_VALID_GPIO(s_swdio_gpio))
		return 0;
	return 1;
}

/*
 * 把一个引脚从 dedicated GPIO 输出通道上"摘下来", 换回普通 GPIO 路由。
 *
 * ★ 这是本驱动最关键的一步(2026-09-19 用板载自检实测定位):
 *
 * ESP32-S3 的 SOC_DEDIC_GPIO_OUT_AUTO_ENABLE = 1 —— 只要 OUT_SEL 指向 dedicated
 * GPIO 通道, 引脚的输出就是"自动使能"的, 清 GPIO_ENABLE 位**根本关不掉它**。
 * 实测(SWCLK/SWDIO 均无外部连线):
 *     dedicated 路由 + 关输出 + 开内部上拉  => 读回 0   (引脚仍被驱动, 上拉顶不住)
 *     普通 GPIO 路由 + 关输出 + 开内部上拉  => 读回 1   (真的释放了, 上拉把线顶高)
 *     把"驱动高后释放"与"驱动低后释放"相比 => 分别读到 1 和 0, 说明引脚
 *     "记住了最后一次驱动的值" = 输出从来没关掉。
 *
 * 后果: bitbang 在读事务里写入 SWDIO 的值恒为 0
 *       (bitbang_swd_exchange: `int swdio = !rnw && (buf[bytec] & bcval);`, rnw=true),
 * 于是只要释放失效, 主机在整个 ACK+DATA 窗口都会把 SWDIO 硬拉低:
 *   - 主机自己读回的就是 0 => JUNK + 0x00000000;
 *   - 目标抢不到总线, 也永远收不到"SWDIO 持续为高 50 个时钟"的线复位
 *     => 永远不进入 SWD 模式 => 永不应答 => cannot read IDR。
 * 这也解释了另外三个现象: 与"完全不接线"的读数逐字相同、与时钟速度无关
 * (2M/1M/250k 完全一致)、以及两次 LA 抓包里 ACK 窗口内数据线一次跳变都没有。
 */
static void esp_gpio_pin_detach_dedic_route(int gpio)
{
	/* 显式写成"普通 GPIO 输出"路由。写成编译期常量而不是读寄存器残留值的原因,
	 * 见 ESP_GPIO_PLAIN_OUT_SEL 的注释(CPU 软复位不进复位 GPIO 外设)。 */
	if (gpio >= 0)
		REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + (gpio * 4), ESP_GPIO_PLAIN_OUT_SEL);
}

static void esp_gpio_swdio_drive(bool is_output)
{
	/* 引脚号非法就直接返回: 下面全是 LL 层直接写寄存器, 不会替我们做参数检查
	 * (esp_gpio_quit() 里对 SWCLK/SWDIO 也是这么防的, 保持一致) */
	if (!GPIO_IS_VALID_GPIO(s_swdio_gpio))
		return;

	/*
	 * SWDIO 的状态只由这个函数定义, 两个分支都把它设成完全确定的状态,
	 * 不依赖复位默认值、也不依赖上一次调用留下的残留。
	 *
	 * 涉及的四个位:
	 *   OE(GPIO_ENABLE) 输出使能 / IE(GPIO_ENABLE 的输入位) 输入使能
	 *   PU(GPIO_PU)     内部上拉    / PD(GPIO_PD)           内部下拉
	 *   OUT_SEL         引脚输出信号来源(dedicated GPIO 通道)
	 */
	if (is_output) {
		/*
		 * 驱动态: 上下拉一律关掉 —— 引脚电平完全由输出数据寄存器决定,
		 * 不受任何残留上拉/下拉影响。恢复 dedicated GPIO 的输出路由,
		 * 再把输出使能打开。输入使能同时保持, 以便随时回读真实引脚电平。
		 */
		gpio_ll_pulldown_dis(s_gpio_dev, s_swdio_gpio);
		gpio_ll_pullup_dis(s_gpio_dev, s_swdio_gpio);
		gpio_ll_input_enable(s_gpio_dev, s_swdio_gpio);
		REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + (s_swdio_gpio * 4), s_gpio_conf);
		gpio_ll_output_enable(s_gpio_dev, s_swdio_gpio);
	} else {
		/*
		 * 释放态: 先关输出, 再把引脚从 dedicated 通道上摘下来, 最后打开内部上拉。
		 *
		 * ★ 摘路由这一步不能省: 在 dedicated 路由下 GPIO_ENABLE 位关不掉输出
		 *   (见 esp_gpio_pin_detach_dedic_route 的注释), 只做 output_disable
		 *   的话引脚仍被 dedicated 通道驱动 —— 这正是"目标永不应答"的根因。
		 *
		 * SWD 协议里 turn-around 期间双方都是高阻, 总线上必须有上拉给出确定
		 * 的高电平(ARM 规范要求 >=100k, 芯片内部上拉即为此)。打开上拉不会妨碍
		 * 目标: 目标应答是推挽输出, 会盖过这颗弱上拉。
		 */
		gpio_ll_output_disable(s_gpio_dev, s_swdio_gpio);
		esp_gpio_pin_detach_dedic_route(s_swdio_gpio);
		gpio_ll_pulldown_dis(s_gpio_dev, s_swdio_gpio);
		gpio_ll_pullup_en(s_gpio_dev, s_swdio_gpio);
		gpio_ll_input_enable(s_gpio_dev, s_swdio_gpio);
	}
}

static struct bitbang_interface esp_gpio_bitbang = {
	.read = esp_gpio_jtag_read,
	.write = esp_gpio_jtag_write,
	.swdio_read = esp_gpio_swdio_read,
	.swdio_drive = esp_gpio_swdio_drive,
	.swd_write = esp_gpio_swd_write,
	.blink = NULL
};

static int esp_jtag_gpio_mode_set(void)
{
	if (esp_gpio_jtag_mode_possible()) {
		gpio_config_t conf = {
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = BIT_ULL(s_tdi_gpio) | BIT_ULL(s_tck_gpio) | BIT_ULL(s_tms_gpio),
			.pull_down_en = 0,
			.pull_up_en = 0,
		};
		if (s_trst_gpio != GPIO_NUM_NC)
			conf.pin_bit_mask |= BIT_ULL(s_trst_gpio);
		if (s_srst_gpio != GPIO_NUM_NC)
			conf.pin_bit_mask |= BIT_ULL(s_srst_gpio);
		if (s_blink_gpio != GPIO_NUM_NC) {
			conf.pin_bit_mask |= BIT_ULL(s_blink_gpio);
			esp_gpio_bitbang.blink = esp_gpio_jtag_blink;
		}
		gpio_config(&conf);

		conf.mode = GPIO_MODE_INPUT;
		conf.pin_bit_mask = BIT_ULL(s_tdo_gpio);
		gpio_config(&conf);

		/*
		 * ⚠️ 可选引脚必须先判 GPIO_NUM_NC: 否则参数 -1 会一路传到
		 * gpio_ll_set_level() 里的 `1u << gpio_num` —— 移位量 0xFFFFFFFF 是 UB,
		 * 实际按位宽取模后变成 bit31 ⇒ 会误写 GPIO31/32 的输出数据位。
		 */
		if (s_blink_gpio != GPIO_NUM_NC)
			gpio_clear(s_blink_gpio);
		if (s_trst_gpio != GPIO_NUM_NC)
			gpio_set(s_trst_gpio);
		if (s_srst_gpio != GPIO_NUM_NC)
			gpio_set(s_srst_gpio);
		gpio_clear(s_tdi_gpio);
		gpio_clear(s_tck_gpio);
		gpio_set(s_tms_gpio);

		return ERROR_OK;
	}

	LOG_ERROR("some JTAG pins are not set");

	return ERROR_FAIL;
}

static int esp_swd_gpio_mode_set(void)
{
	if (esp_gpio_swd_mode_possible()) {
		gpio_config_t conf = {
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = BIT_ULL(s_swclk_gpio),
			.pull_down_en = 0,
			.pull_up_en = 0,
		};
		if (s_blink_gpio != GPIO_NUM_NC) {
			conf.pin_bit_mask |= BIT_ULL(s_blink_gpio);
			esp_gpio_bitbang.blink = esp_gpio_swd_blink;
		}
		gpio_config(&conf);

		gpio_mode_input_output_set(s_swdio_gpio);
		/*
		 * SWDIO 上拉: 主要是给不支持 dedicated GPIO 的芯片用(那条路径没有别处设它);
		 * dedicated 芯片上 esp_swd_dedic_gpio_mode_set() 结尾还会显式再设一次。
		 * (更正 2026-09-19: 原先注释说这里会被 gpio_func_sel() 冲掉 —— 实际不会,
		 *  gpio_ll_func_sel() 只动 MCU_SEL, 不碰 PU/PD。)
		 */
		gpio_set_pull_mode(s_swdio_gpio, GPIO_PULLUP_ONLY);

		return ERROR_OK;
	}

	LOG_ERROR("some SWD pins are not set");

	return ERROR_FAIL;
}

#if CONFIG_SOC_DEDICATED_GPIO_SUPPORTED

static dedic_gpio_bundle_handle_t s_gpio_in_bundle;
static dedic_gpio_bundle_handle_t s_gpio_out_bundle;
static dedic_gpio_bundle_handle_t s_gpio_io_bundle;

static int esp_jtag_dedic_gpio_mode_set(void)
{
	/*
	 * ★ 数组必须**按实际用到的引脚条数**来填, 不能预留空槽位。
	 *
	 * 旧写法是 `int arr[N] = { 0 }` 再按需赋值 —— 未配置的槽位就保持 0,
	 * 而 0 是合法引脚 GPIO0。dedic_gpio_new_bundle() 完全不区分"这是占位还是真引脚",
	 * 会照样把 GPIO0 连到 dedicated 输出通道(TRST/SRST/BLINK 都不配时就占用 3 次),
	 * 而该通路的输出在 S2/S3 上是硬件自动使能的 ⇒ **GPIO0 被持续驱动为 0 且关不掉**,
	 * 与 SWDIO 那个故障完全同一机理。
	 * 用计数器只放真实引脚即可根治; 顺便把三个可选掩码算出来。
	 */
	int bundle_out_gpios[GPIO_JTAG_OUT_MAX];
	int bundle_in_gpios[GPIO_JTAG_IN_MAX];
	unsigned int out_cnt = 0;

	/* 前三位恒为 TCK/TDI/TMS, 掩码 0x01/0x02/0x04 是编译期常量 */
	bundle_out_gpios[out_cnt++] = s_tck_gpio;
	bundle_out_gpios[out_cnt++] = s_tdi_gpio;
	bundle_out_gpios[out_cnt++] = s_tms_gpio;

	s_trst_mask = 0;
	if (s_trst_gpio != GPIO_NUM_NC) {
		s_trst_mask = 1u << out_cnt;
		bundle_out_gpios[out_cnt++] = s_trst_gpio;
	}

	s_srst_mask = 0;
	if (s_srst_gpio != GPIO_NUM_NC) {
		s_srst_mask = 1u << out_cnt;
		bundle_out_gpios[out_cnt++] = s_srst_gpio;
	}

	s_blink_mask = 0;
	if (s_blink_gpio != GPIO_NUM_NC) {
		s_blink_mask = 1u << out_cnt;
		bundle_out_gpios[out_cnt++] = s_blink_gpio;
	}

	bundle_in_gpios[0] = s_tdo_gpio;

	dedic_gpio_bundle_config_t out_bundle_config = {
		.gpio_array = bundle_out_gpios,
		.array_size = out_cnt,
		.flags = {
			.out_en = 1,
		},
	};

	dedic_gpio_bundle_config_t in_bundle_config = {
		.gpio_array = bundle_in_gpios,
		.array_size = ARRAY_SIZE(bundle_in_gpios),
		.flags = {
			.in_en = 1,
		},
	};

	dedic_gpio_new_bundle(&out_bundle_config, &s_gpio_out_bundle);
	dedic_gpio_new_bundle(&in_bundle_config, &s_gpio_in_bundle);

	return ERROR_OK;
}

static int esp_swd_dedic_gpio_mode_set(void)
{
	/*
	 * 新布局(掩码的详细理由见 esp_gpio.h):
	 *     输出 bundle = { SWCLK, SWDIO [, blink] }   ← SWDIO 固定在索引 1(掩码 0x02)
	 *     输入 bundle = { SWDIO }                    ← in 通道 0
	 *
	 * SWDIO 同时出现在两个 bundle 里是允许的: dedic_gpio_new_bundle() 的 out/in
	 * 通道占用表是分开的, 同一个引脚可以一边占 out 通道、一边占 in 通道。
	 *
	 * 相比旧布局的两个改善:
	 *   1. 数组里不再有空槽位 ⇒ 不会误把 GPIO0 拖进 dedicated 输出通道
	 *      (旧布局里 SWDIO 拿到的是 OUT2=223 而不是 OUT1=222, 就是空洞占了通道 1 的实证);
	 *   2. SWDIO 的输出掩码恒为 0x02, 与 blink 配不配无关。
	 *
	 * s_gpio_io_bundle 这个名字保留(改动最小), 它现在只负责 SWDIO 的输入通道。
	 */
	int bundle_out_gpios[GPIO_SWD_OUT_MAX];
	int bundle_in_gpios[1];
	unsigned int out_cnt = 0;

	bundle_out_gpios[out_cnt++] = s_swclk_gpio;	/* 索引 0 -> 掩码 0x01 */
	bundle_out_gpios[out_cnt++] = s_swdio_gpio;	/* 索引 1 -> 掩码 0x02, 恒定 */

	s_blink_mask = 0;
	if (s_blink_gpio != GPIO_NUM_NC) {
		s_blink_mask = 1u << out_cnt;		/* 索引 2 -> 掩码 0x04 */
		bundle_out_gpios[out_cnt++] = s_blink_gpio;
	}

	bundle_in_gpios[0] = s_swdio_gpio;

	dedic_gpio_bundle_config_t out_bundle_config = {
		.gpio_array = bundle_out_gpios,
		.array_size = out_cnt,
		.flags = {
			.out_en = 1,
		},
	};

	dedic_gpio_bundle_config_t io_bundle_config = {
		.gpio_array = bundle_in_gpios,
		.array_size = ARRAY_SIZE(bundle_in_gpios),
		.flags = {
			.in_en = 1,
		},
	};

	dedic_gpio_new_bundle(&out_bundle_config, &s_gpio_out_bundle);
	dedic_gpio_new_bundle(&io_bundle_config, &s_gpio_io_bundle);
	s_gpio_conf = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + (s_swdio_gpio * 4));

	/*
	 * 创建 bundle 之后再显式设一次 SWDIO 的上拉。
	 *
	 * 说明(2026-09-19 更正): 这里原先写的是"上拉被 gpio_func_sel() 冲掉了",
	 * 不准确 —— gpio_ll_func_sel() 只改 IO_MUX 的 MCU_SEL 字段(切成 GPIO 功能),
	 * 并不动 PU/PD 位(自检 [A] 时刻读到 IO_MUX 的 PU=1 即为实证)。
	 * 保留这一动作的真正理由是: 让"释放期间必须有上拉"由本驱动显式定义, 不去依赖
	 * esp_swd_gpio_mode_set() 里那次 gpio_set_pull_mode() 有没有被谁改过。
	 * SWD 的 turn-around 期间双方都高阻, 总线上需要一个确定电平(ARM 规范要求
	 * >=100k, 芯片内部上拉正为此); 没有它, 主机读回的就是悬空噪声。
	 *
	 * 用 LL 层只动上下拉寄存器, 不碰 IO_MUX 功能选择和 OUT_SEL 路由
	 * (s_gpio_conf 刚刚读出来的那条路由不能被改写)。
	 */
	gpio_ll_pulldown_dis(s_gpio_dev, s_swdio_gpio);
	gpio_ll_pullup_en(s_gpio_dev, s_swdio_gpio);

	return ERROR_OK;
}

#else

/*
 * 非 dedicated 芯片(经典 ESP32 等): 上面两个函数是空宏, 于是 s_gpio_conf 在这条
 * 路径上永远不会被赋值 —— 所以 esp_gpio_init() 里**必须**把它预置成"普通 GPIO 输出"
 * 路由。若留 0, esp_gpio_swdio_drive(true) 会把 0 当作外设信号写进 OUT_SEL_CFG,
 * 引脚的输出源就不再是 GPIO_OUT 寄存器 ⇒ gpio_ll_set_level() 完全失效 ⇒
 * SWDIO 驱动不了。旧代码正是如此(非 dedicated 芯片上 SWD 从未工作过), 现在已修。
 */
#define esp_jtag_dedic_gpio_mode_set() do {} while (0)
#define esp_swd_dedic_gpio_mode_set() do {} while (0)

#endif /* CONFIG_SOC_DEDICATED_GPIO_SUPPORTED */


static int esp_gpio_init(void)
{
	int khz = 5000;
	int speed = 0;

	/*
	 * "空闲/释放"所依赖的输出路由先设成"普通 GPIO 输出"。
	 * 对 dedicated 芯片这只是个初值(bundle 创建后会被它自己的路由值覆盖);
	 * 对**非 dedicated 芯片**则是唯一正确的值 —— 那条路径没有别处给 s_gpio_conf
	 * 赋值, 留 0 会被当成"外设信号 0"写进 OUT_SEL_CFG, SWDIO 就再也驱不动了。
	 */
	s_gpio_conf = ESP_GPIO_PLAIN_OUT_SEL;

	/*
	 * 让这里的默认速度真正生效。
	 * 旧代码只取了 s_gpio_speed_khz(供 speed_div 回报), 把算出来的 speed 丢掉, 于是
	 * s_gpio_delay 在 OpenOCD 应用 adapter speed 之前一直是 0(最快), 而 speed_div
	 * 回报的却是 5000 —— 报的值和实际跑的值对不上。
	 * 这里直接写 s_gpio_delay 而不调 esp_gpio_speed(), 免得启动日志里多出一行
	 * 与最终生效速度无关的 "requested 5000 kHz"。
	 */
	if (esp_gpio_khz(khz, &speed) == ERROR_OK)
		s_gpio_delay = speed;

	/*
	 * Configure TDO as an input, and TDI, TCK, TMS, TRST, SRST
	 * as outputs.  Drive TDI and TCK low, and TMS/TRST/SRST high.
	 */
	if (transport_is_jtag()) {
		LOG_INFO("esp_gpio JTAG bitbang driver");

		if (esp_jtag_gpio_mode_set() != ERROR_OK)
			return ERROR_FAIL;

		esp_jtag_dedic_gpio_mode_set();

	} else if (transport_is_swd()) {
		LOG_INFO("esp_gpio SWD bitbang driver");

		if (esp_swd_gpio_mode_set() != ERROR_OK)
			return ERROR_FAIL;

		esp_swd_dedic_gpio_mode_set();

		/*
		 * 明确设定"进入第一个事务之前"的空闲态, 不依赖任何复位默认值:
		 *   SWCLK -> 输出数据寄存器写 0(驱动低, 与常见 debug 适配器的空闲电平一致)
		 *   SWDIO -> 交给 esp_gpio_swdio_drive(false) 统一定义为"释放 + 上拉"
		 * 这两行保证上电后、第一个 SWD 事务之前, 两条线的状态就是确定的,
		 * 而不是停留在引脚复位默认值或上一次运行留下的残值上。
		 */
		esp_gpio_swd_write(0, 0);
		esp_gpio_swdio_drive(false);
	}

	bitbang_interface = &esp_gpio_bitbang;

	return ERROR_OK;
}

static int esp_gpio_quit(void)
{
	/*
	 * 退出前把 SWD 两条线都放成"纯高阻输入"(不驱动、不带上下拉)。
	 *
	 * 原实现是空的, 于是 OpenOCD 返回后引脚仍保持最后一次的输出状态:
	 * SWCLK 停在驱动高, SWDIO 停在驱动低。后果有两个:
	 *   1. 连接失败后从外部用万用表/示波器量到的电平全是 ESP32 自己输出的,
	 *      看不出线路另一端(接线/目标)的真实状态, 静态测量完全失去意义;
	 *   2. 目标侧被主机一直驱动着, 任何对照实验都不干净。
	 * 放成高阻后, ESP32 的引脚会直接呈现线路另一端的状态, 这样"某个脚到底是
	 * 几伏"才有诊断价值。
	 *
	 * ⚠️ 同样必须先"摘掉 dedicated 路由"再关输出: 否则引脚仍被 dedicated 通道
	 *    驱动, 打印出来的 "released to high-Z" 就是假的(实测过, 见
	 *    esp_gpio_pin_detach_dedic_route 的注释)。
	 */
	if (transport_is_swd()) {
		if (GPIO_IS_VALID_GPIO(s_swclk_gpio)) {
			gpio_ll_output_disable(s_gpio_dev, s_swclk_gpio);
			esp_gpio_pin_detach_dedic_route(s_swclk_gpio);
			gpio_ll_pullup_dis(s_gpio_dev, s_swclk_gpio);
			gpio_ll_pulldown_dis(s_gpio_dev, s_swclk_gpio);
			gpio_ll_input_enable(s_gpio_dev, s_swclk_gpio);
		}
		if (GPIO_IS_VALID_GPIO(s_swdio_gpio)) {
			gpio_ll_output_disable(s_gpio_dev, s_swdio_gpio);
			esp_gpio_pin_detach_dedic_route(s_swdio_gpio);
			gpio_ll_pullup_dis(s_gpio_dev, s_swdio_gpio);
			gpio_ll_pulldown_dis(s_gpio_dev, s_swdio_gpio);
			gpio_ll_input_enable(s_gpio_dev, s_swdio_gpio);
		}
		LOG_INFO("esp_gpio: SWCLK/SWDIO released to high-Z");
	}
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionums)
{
	if (CMD_ARGC == 4) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_tck_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], s_tms_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[2], s_tdi_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[3], s_tdo_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD,
		"esp_gpio GPIO config: tck = %d, tms = %d, tdi = %d, tdo = %d",
		s_tck_gpio, s_tms_gpio, s_tdi_gpio, s_tdo_gpio);

	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_tck)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_tck_gpio);

	command_print(CMD, "esp_gpio GPIO config: tck = %d", s_tck_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_tms)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_tms_gpio);

	command_print(CMD, "esp_gpio GPIO config: tms = %d", s_tms_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_tdo)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_tdo_gpio);

	command_print(CMD, "esp_gpio GPIO config: tdo = %d", s_tdo_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_tdi)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_tdi_gpio);

	command_print(CMD, "esp_gpio GPIO config: tdi = %d", s_tdi_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_srst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_srst_gpio);

	command_print(CMD, "esp_gpio GPIO config: srst = %d", s_srst_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_jtag_gpionum_trst)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_trst_gpio);

	command_print(CMD, "esp_gpio GPIO config: trst = %d", s_trst_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_gpionum_blink)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_blink_gpio);

	command_print(CMD, "esp_gpio GPIO config: blink = %d", s_blink_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_swd_gpionums)
{
	if (CMD_ARGC == 2) {
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_swclk_gpio);
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[1], s_swdio_gpio);
	} else if (CMD_ARGC != 0) {
		return ERROR_COMMAND_SYNTAX_ERROR;
	}

	command_print(CMD,
			"esp_gpio GPIO nums: swclk = %d, swdio = %d",
			s_swclk_gpio, s_swdio_gpio);

	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_swd_gpionum_swclk)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_swclk_gpio);

	command_print(CMD, "esp_gpio num: swclk = %d", s_swclk_gpio);
	return ERROR_OK;
}

COMMAND_HANDLER(esp_gpio_handle_swd_gpionum_swdio)
{
	if (CMD_ARGC == 1)
		COMMAND_PARSE_NUMBER(int, CMD_ARGV[0], s_swdio_gpio);

	command_print(CMD, "esp_gpio num: swdio = %d", s_swdio_gpio);
	return ERROR_OK;
}

static const struct command_registration esp_gpio_command_handlers[] = {
	{
		.name = "esp_gpio_jtag_nums",
		.handler = &esp_gpio_handle_jtag_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for tck, tms, tdi, tdo. (in that order)",
		.usage = "[tck tms tdi tdo]",
	},
	{
		.name = "esp_gpio_tck_num",
		.handler = &esp_gpio_handle_jtag_gpionum_tck,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tck.",
		.usage = "[tck]",
	},
	{
		.name = "esp_gpio_tms_num",
		.handler = &esp_gpio_handle_jtag_gpionum_tms,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tms.",
		.usage = "[tms]",
	},
	{
		.name = "esp_gpio_tdo_num",
		.handler = &esp_gpio_handle_jtag_gpionum_tdo,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdo.",
		.usage = "[tdo]",
	},
	{
		.name = "esp_gpio_tdi_num",
		.handler = &esp_gpio_handle_jtag_gpionum_tdi,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for tdi.",
		.usage = "[tdi]",
	},
	{
		.name = "esp_gpio_srst_num",
		.handler = &esp_gpio_handle_jtag_gpionum_srst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for srst.",
		.usage = "[srst]",
	},
	{
		.name = "esp_gpio_trst_num",
		.handler = &esp_gpio_handle_jtag_gpionum_trst,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for trst.",
		.usage = "[trst]",
	},
	{
		.name = "esp_gpio_blink_num",
		.handler = &esp_gpio_handle_gpionum_blink,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for blink.",
		.usage = "[blink]",
	},
	{
		.name = "esp_gpio_swd_nums",
		.handler = &esp_gpio_handle_swd_gpionums,
		.mode = COMMAND_CONFIG,
		.help = "gpio numbers for swclk, swdio. (in that order)",
		.usage = "[swclk swdio]",
	},
	{
		.name = "esp_gpio_swclk_num",
		.handler = &esp_gpio_handle_swd_gpionum_swclk,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swclk.",
		.usage = "[swclk]",
	},
	{
		.name = "esp_gpio_swdio_num",
		.handler = &esp_gpio_handle_swd_gpionum_swdio,
		.mode = COMMAND_CONFIG,
		.help = "gpio number for swdio.",
		.usage = "[swdio]",
	},

	COMMAND_REGISTRATION_DONE
};

static const char *const s_esp_gpio_transports[] = { "jtag", "swd", NULL };

static struct jtag_interface s_esp_gpio_jtag_interface = {
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = bitbang_execute_queue,
};

struct adapter_driver esp_gpio_adapter_driver = {
	.name = "esp_gpio",
	.transports = s_esp_gpio_transports,
	.speed = esp_gpio_speed,
	.khz = esp_gpio_khz,
	.speed_div = esp_gpio_speed_div,
	.commands = esp_gpio_command_handlers,
	.init = esp_gpio_init,
	.quit = esp_gpio_quit,
	.reset = esp_gpio_jtag_reset,
	.jtag_ops = &s_esp_gpio_jtag_interface,
	.swd_ops = &bitbang_swd,
};
