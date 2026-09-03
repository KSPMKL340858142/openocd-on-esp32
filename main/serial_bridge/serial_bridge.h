#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERIAL_BRIDGE_CHANNEL_NUM  2   /* UART1 / UART2 (UART0 保留给控制台) */

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

/* 开机时调用：从 NVS 读配置（无则默认），启动所有启用的通道 */
esp_err_t serial_bridge_start(void);

/* 读取当前生效的配置 */
esp_err_t serial_bridge_get_configs(serial_bridge_cfgs_t *out);

/* 校验 -> 保存到 NVS -> 热应用（重建被改动/开关的通道，OpenOCD 不受影响） */
esp_err_t serial_bridge_set_configs(const serial_bridge_cfgs_t *cfg);

#ifdef __cplusplus
}
#endif
