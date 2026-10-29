#pragma once

/* 统一配置：整份运行配置存成一块 JSON 文本，放在 NVS 的一个 key 里。
 *
 * 设计要点
 *   - 唯一真相源就是 NVS 里那份 JSON；s_cfg 是它的内存镜像，
 *     g_app_params 是启动时喂给 OpenOCD / UI 的快照。
 *   - 网页表单、上传的 JSON、开机加载三者都改同一份 s_cfg，不再各写各的 NVS 键，
 *     否则会出现「文件一份、散键一份」的双真相。
 *   - 内容用文本编辑器在电脑上改，改完通过 POST /config.json 传回来；
 *     网页不做编辑器，只做导入导出。
 *
 * 注意：本头文件 include serial_bridge.h 只是为了拿到 serial_bridge_cfgs_t 类型。
 * serial_bridge.h 不反向 include 本文件，故不存在循环依赖。
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "types.h"
#include "serial_bridge/serial_bridge.h"

/* NVS 里存 JSON 用的 key（namespace 仍是 storage.c 里的 "nvs"） */
#define APP_CONFIG_NVS_KEY          "cfgjson"

/* 字符串字段容量。extra_command 原先是堆上不定长字符串，这里收成定长：
 * 上传的 JSON 长度不可控，必须有上限，超了直接拒绝而不是静默截断。 */
#define APP_CFG_TARGET_LEN          64
#define APP_CFG_RTOS_LEN            24
#define APP_CFG_FLASH_LEN           8
#define APP_CFG_CMD_LEN             192
#define APP_CFG_GPIO_JTAG_LEN       32
#define APP_CFG_GPIO_SWD_LEN        16

/* RTT 轮询间隔（ms）上下限。
 * 下限 5 ms 来自 OpenOCD 每轮的固定开销（控制块 24 B + 描述符 + 写回 RdOff，
 * 约 1.5~3 ms），再快也提不上吞吐。上限只是防手改出一个荒谬的值。 */
#define APP_CFG_RTT_POLL_MIN        5
#define APP_CFG_RTT_POLL_MAX        5000
#define APP_CFG_RTT_POLL_DEFAULT    250

/* 上传 JSON 的最大可接受长度。实测整份配置约 700 B，留足余量即可，
 * 目的是挡住「传了个几 KB 的东西进来把栈压垮」。 */
#define APP_CFG_JSON_MAX            2048

typedef struct {
    /* ---- OpenOCD 运行参数 ---- */
    char target[APP_CFG_TARGET_LEN];        /* 目标 cfg 文件名, 如 ti_mspm0.cfg */
    char iface;                             /* '0' = jtag, '1' = swd */
    char rtos[APP_CFG_RTOS_LEN];
    char flash_size[APP_CFG_FLASH_LEN];     /* "auto" 或 "0" */
    char debug_level;                       /* '1'..'4' */
    char dual_core;                         /* '1' = 单核, '3' = SMP */
    char extra_command[APP_CFG_CMD_LEN];    /* 追加的 OpenOCD -c 命令 */
    char gpio_jtag[APP_CFG_GPIO_JTAG_LEN];  /* "tck tms tdi tdo" */
    char gpio_swd[APP_CFG_GPIO_SWD_LEN];    /* "swclk swdio" */
    int  rtt_poll_ms;                       /* RTT 上行轮询间隔 */

    /* ---- WiFi ---- */
    char wifi_ssid[WIFI_SSID_LEN];
    char wifi_pass[WIFI_PASS_LEN];
    bool wifi_power_save;                   /* true = modem sleep, false = RF 常开 */

    /* ---- 串口桥（两路） ---- */
    serial_bridge_cfgs_t serial;
} app_config_t;

/* 开机调用一次：从 NVS 读 JSON 解析进内存镜像。
 * 读不到（首次升级）时从老的散装 NVS 键迁移一份并立即落盘，之后只认 JSON。 */
esp_err_t app_config_load(void);

/* 把内存镜像序列化后写回 NVS */
esp_err_t app_config_save(void);

/* 只读访问（开机加载后各模块都从这里取） */
const app_config_t *app_config_get(void);

/* 可变访问：仅限「改一个字段然后立刻 app_config_save()」的用法。
 * 返回非 const 指针意味着调用方要自己保证不破坏字段合法性。 */
app_config_t *app_config_mutable(void);

/* 序列化成 JSON 字符串，调用方负责 free()。
 * pretty = true 时带缩进换行——导出给人用文本编辑器改，可读性优先。 */
char *app_config_to_json(bool pretty);

/* 解析并校验一段 JSON，成功后替换内存镜像（**不落盘**，由调用方决定是否 save）。
 * 校验失败时把原因写进 err（可为 NULL），内存镜像保持不变。 */
esp_err_t app_config_from_json(const char *json, char *err, size_t err_len);

/* 恢复出厂默认值（只改内存镜像） */
void app_config_reset_defaults(void);
