#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    NETWORK_MNGR_CONNECTED,
    NETWORK_MNGR_DISCONNECTED,
    NETWORK_MNGR_STATUS_NOT_CHANGED,
    NETWORK_MNGR_ERROR
} network_mngr_states_t;

esp_err_t network_mngr_init(void);
esp_err_t network_mngr_init_ap(const char *ssid, const char *pass);
esp_err_t network_mngr_connect_ap(unsigned int max_retry);
esp_err_t network_mngr_init_sta(const char *ssid, const char *pass);
esp_err_t network_mngr_connect_sta(unsigned int max_retry);
esp_err_t network_mngr_init_prov(const char *ssid, const char *pass, const void *http_handle);
esp_err_t network_mngr_connect_prov(unsigned int max_retry);
network_mngr_states_t network_mngr_state(unsigned int timeout);
esp_err_t network_mngr_get_sta_credentials(char **ssid, char **pass);
esp_err_t network_mngr_get_sta_ip(char **ip);
esp_err_t network_mngr_get_ap_ip(char **ip);
/* WiFi 省电模式运行时开关（需在 esp_wifi_start 之后调用）：
 * ps_on=true  → modem sleep 省电（空闲省 ~60-80mA，但小包收包延迟可能到 ~100-400ms）
 * ps_on=false → RF 常开（空闲多耗 ~60-80mA 会升温，但小包延迟稳定，示波器流畅）
 * 返回 esp_wifi_set_ps 的结果。 */
esp_err_t network_mngr_set_power_save(bool ps_on);
