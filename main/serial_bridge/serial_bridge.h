#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_BRIDGE_CHANNEL_NUM  2   /* UART1 / UART2 (UART0 保留给控制台) */

/* 升级前的独立 NVS key（内容是 serial_bridge_cfgs_t 的二进制 blob）。
 * 现在配置统一存在 app_config 的 JSON 里，这个 key 只在首次启动迁移时读一次，
 * 迁移完成后不再写入。名字保持原样，否则老设备的配置读不出来。 */
#define SERIAL_BRIDGE_NVS_KEY  "serbridge"

typedef struct {
    bool     enable;
    int      uart_num;   /* 1 或 2 */
    int      tx_gpio;
    int      rx_gpio;
    int      baud;
    int      port;       /* TCP 监听端口 */
} serial_bridge_channel_cfg_t;

typedef struct {
    serial_bridge_channel_cfg_t ch[SERIAL_BRIDGE_CHANNEL_NUM];
} serial_bridge_cfgs_t;

/* 开机时调用：从 app_config 取配置（无则默认），启动所有启用的通道 */
esp_err_t serial_bridge_start(void);

/* 读取当前生效的配置 */
esp_err_t serial_bridge_get_configs(serial_bridge_cfgs_t *out);

/* 校验 -> 写回 app_config（落盘）-> 热应用（重建被改动/开关的通道，OpenOCD 不受影响） */
esp_err_t serial_bridge_set_configs(const serial_bridge_cfgs_t *cfg);

/* 出厂默认值。供 app_config 组装默认配置 / 校验上传内容用，
 * 避免默认值在 serial_bridge.c 和 app_config.c 里各写一份。 */
void serial_bridge_default_configs(serial_bridge_cfgs_t *out);

/* 完整合法性检查（引脚是否被占用、通道间引脚/端口冲突、波特率范围等）。
 * app_config 在接收上传的 JSON 时也会调它。 */
esp_err_t serial_bridge_validate(const serial_bridge_cfgs_t *cfgs);

#ifdef __cplusplus
}
#endif
