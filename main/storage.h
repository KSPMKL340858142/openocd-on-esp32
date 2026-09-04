#pragma once

#include "esp_err.h"

/* OpenOCD params */
#define OOCD_CFG_FILE_KEY           "file"
#define OOCD_RTOS_TYPE_KEY          "rtos"
#define OOCD_DUAL_CORE_KEY          "smp"
#define OOCD_FLASH_SUPPORT_KEY      "flash"
#define OOCD_INTERFACE_KEY          "interface"
#define OOCD_CMD_LINE_ARGS_KEY      "command"
#define OOCD_DBG_LEVEL_KEY          "debug"
/* OpenOCD esp_gpio 引脚（文本，空格分隔）。缺省 = cfg 内置 38 39 40 41 / 38 39 */
#define OOCD_GPIO_JTAG_KEY          "gpioj"   /* "tck tms tdi tdo" */
#define OOCD_GPIO_SWD_KEY           "gpios"   /* "swclk swdio" */

/* Network params */
#define WIFI_SSID_KEY               "ssid"
#define WIFI_PASS_KEY               "pass"
/* WiFi 省电开关：'1'=modem sleep(省电) / '0'或缺失=RF常开(低延迟, 费电升温) */
#define WIFI_PS_KEY                 "ps"

#define CFG_FILE_PATH               "/data/target/"

esp_err_t storage_init_filesystem(void);
esp_err_t storage_write(const char *key, const char *value, size_t len);
esp_err_t storage_read(const char *key, char *value, size_t len);
esp_err_t storage_erase_key(const char *key);
size_t storage_get_value_length(const char *key);
bool storage_is_key_exist(const char *key);
esp_err_t storage_erase_all(void);
esp_err_t storage_alloc_and_read(char *key, char **value);
esp_err_t storage_update_target_struct(void);
esp_err_t storage_update_rtos_struct(void);
