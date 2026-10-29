/* 统一配置：一份 JSON 存在 NVS 的单个 key 里。
 *
 * 存储形态：NVS namespace "nvs"，key "cfgjson"，value 是 JSON 文本（不是二进制结构体）。
 * 选 NVS 而不是 FatFS 的原因：idf.py flash 会整块重写 storage 分区，
 * 放 FatFS 的话每烧一次固件配置就没了；NVS 分区不在烧录列表里，能保住。
 *
 * 迁移：升级前配置散在 9 个 OpenOCD 键 + 3 个 WiFi 键 + 1 个串口桥二进制 blob 里。
 * 首次启动读不到 JSON 时，把这些散键读出来拼成一份 JSON 落盘；
 * 散键不删除，留作 JSON 意外损坏时的兜底（之后不再写入）。
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"

#include <cJSON.h>

#include "app_config.h"
#include "storage.h"
#include "types.h"

static const char *TAG = "app-config";

static app_config_t s_cfg;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */

static void set_str(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = 0;
        return;
    }

    size_t n = strlen(src);
    if (n > cap - 1) {
        n = cap - 1;
    }

    memcpy(dst, src, n);
    dst[n] = 0;
}

static esp_err_t fail(char *err, size_t err_len, const char *fmt, ...)
{
    if (err && err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }

    return ESP_ERR_INVALID_ARG;
}

static const char *json_str(cJSON *o, const char *key, const char *fallback)
{
    cJSON *v = cJSON_GetObjectItem(o, key);

    if (!v || v->type != cJSON_String || !v->valuestring) {
        return fallback;
    }

    return v->valuestring;
}

static int json_int(cJSON *o, const char *key, int fallback)
{
    cJSON *v = cJSON_GetObjectItem(o, key);

    if (!v || v->type != cJSON_Number) {
        return fallback;
    }

    return v->valueint;
}

static bool json_bool(cJSON *o, const char *key, bool fallback)
{
    cJSON *v = cJSON_GetObjectItem(o, key);

    if (!v) {
        return fallback;
    }

    return cJSON_IsTrue(v);
}

/* ------------------------------------------------------------------ */
/* 默认值                                                              */

void app_config_reset_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    set_str(s_cfg.target, sizeof(s_cfg.target), CONFIG_OPENOCD_TARGET_CONFIG_FILE);
    s_cfg.iface = (char)(CONFIG_OPENOCD_INTERFACE + '0');
    set_str(s_cfg.rtos, sizeof(s_cfg.rtos), CONFIG_ESP_RTOS);
    set_str(s_cfg.flash_size, sizeof(s_cfg.flash_size), CONFIG_ESP_FLASH_SIZE);
    s_cfg.debug_level = (char)(CONFIG_OPENOCD_DEBUG_LEVEL + '0');
    s_cfg.dual_core = (char)(CONFIG_ESP_ONLYCPU + '0');
    s_cfg.extra_command[0] = 0;

    set_str(s_cfg.gpio_jtag, sizeof(s_cfg.gpio_jtag), "38 39 40 41");
    set_str(s_cfg.gpio_swd, sizeof(s_cfg.gpio_swd), "38 39");

    s_cfg.rtt_poll_ms = APP_CFG_RTT_POLL_DEFAULT;

    /* WiFi 默认留空：留空代表「还没配过」，load_network_params() 会据此
     * 退回 menuconfig 值或进 AP 配网模式。若在这里填 CONFIG_ESP_WIFI_SSID，
     * 首次上电就会被当成「已配 WiFi」走 STA，把原来的配网流程挡掉。 */
    s_cfg.wifi_ssid[0] = 0;
    s_cfg.wifi_pass[0] = 0;
    s_cfg.wifi_power_save = true;

    serial_bridge_default_configs(&s_cfg.serial);
}

/* ------------------------------------------------------------------ */
/* 从升级前的散装 NVS 键迁移                                           */

static void legacy_migrate(void)
{
    char *p = NULL;

    /* OpenOCD 目标 cfg 文件名 */
    if (storage_alloc_and_read(OOCD_CFG_FILE_KEY, &p) == ESP_OK && p) {
        set_str(s_cfg.target, sizeof(s_cfg.target), p);
        free(p);
        p = NULL;
    }

    if (storage_read(OOCD_INTERFACE_KEY, &s_cfg.iface, 1) != ESP_OK) {
        s_cfg.iface = (char)(CONFIG_OPENOCD_INTERFACE + '0');
    }

    if (storage_alloc_and_read(OOCD_RTOS_TYPE_KEY, &p) == ESP_OK && p) {
        set_str(s_cfg.rtos, sizeof(s_cfg.rtos), p);
        free(p);
        p = NULL;
    }

    if (storage_alloc_and_read(OOCD_CMD_LINE_ARGS_KEY, &p) == ESP_OK && p) {
        set_str(s_cfg.extra_command, sizeof(s_cfg.extra_command), p);
        free(p);
        p = NULL;
    } else if (strncmp(s_cfg.target, "esp32", 5) != 0) {
        /* 保持升级前的行为：非 esp32 目标才用 menuconfig 里的自定义命令，
         * esp32 目标强制为空（原有判断就是这么写的，照搬免得改变行为）。 */
        set_str(s_cfg.extra_command, sizeof(s_cfg.extra_command), CONFIG_OPENOCD_CUSTOM_COMMAND);
    }

    if (storage_alloc_and_read(OOCD_FLASH_SUPPORT_KEY, &p) == ESP_OK && p) {
        set_str(s_cfg.flash_size, sizeof(s_cfg.flash_size), p);
        free(p);
        p = NULL;
    }

    if (storage_read(OOCD_DUAL_CORE_KEY, &s_cfg.dual_core, 1) != ESP_OK) {
        s_cfg.dual_core = (char)(CONFIG_ESP_ONLYCPU + '0');
    }

    if (storage_read(OOCD_DBG_LEVEL_KEY, &s_cfg.debug_level, 1) != ESP_OK) {
        s_cfg.debug_level = (char)(CONFIG_OPENOCD_DEBUG_LEVEL + '0');
    }

    if (storage_alloc_and_read(OOCD_GPIO_JTAG_KEY, &p) == ESP_OK && p && strlen(p)) {
        set_str(s_cfg.gpio_jtag, sizeof(s_cfg.gpio_jtag), p);
    }
    free(p);
    p = NULL;

    if (storage_alloc_and_read(OOCD_GPIO_SWD_KEY, &p) == ESP_OK && p && strlen(p)) {
        set_str(s_cfg.gpio_swd, sizeof(s_cfg.gpio_swd), p);
    }
    free(p);
    p = NULL;

    /* RTT 轮询间隔：升级前写死在 cfg 里，没有可迁移的旧值，用默认。 */
    s_cfg.rtt_poll_ms = APP_CFG_RTT_POLL_DEFAULT;

    /* WiFi：只有 NVS 里确实有 ssid 才迁移。没有就保持空串，
     * 让 load_network_params() 走原来的「menuconfig 默认值 -> AP 配网」分支。 */
    size_t ssid_len = storage_get_value_length(WIFI_SSID_KEY);
    if (ssid_len > 0 && ssid_len < sizeof(s_cfg.wifi_ssid)) {
        char buf[WIFI_SSID_LEN] = {0};
        if (storage_read(WIFI_SSID_KEY, buf, ssid_len) == ESP_OK) {
            set_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), buf);
        }
    }
    if (strlen(s_cfg.wifi_ssid)) {
        size_t pass_len = storage_get_value_length(WIFI_PASS_KEY);
        if (pass_len > 0 && pass_len < sizeof(s_cfg.wifi_pass)) {
            char buf[WIFI_PASS_LEN] = {0};
            if (storage_read(WIFI_PASS_KEY, buf, pass_len) == ESP_OK) {
                set_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), buf);
            }
        }
    }

    char ps = '1';
    if (storage_is_key_exist(WIFI_PS_KEY)) {
        storage_read(WIFI_PS_KEY, &ps, 1);
    }
    s_cfg.wifi_power_save = (ps == '1');

    /* 串口桥：升级前是一个二进制结构体 blob，长度对上才认 */
    size_t sb_len = storage_get_value_length(SERIAL_BRIDGE_NVS_KEY);
    if (sb_len == sizeof(serial_bridge_cfgs_t)) {
        if (storage_read(SERIAL_BRIDGE_NVS_KEY, (char *)&s_cfg.serial, sb_len) != ESP_OK) {
            serial_bridge_default_configs(&s_cfg.serial);
        }
    }

    ESP_LOGI(TAG, "migrated legacy NVS keys into config json");
}

/* ------------------------------------------------------------------ */
/* JSON 序列化                                                         */

static void serial_to_json(cJSON *root)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return;
    }

    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        const serial_bridge_channel_cfg_t *c = &s_cfg.serial.ch[i];
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            break;
        }
        cJSON_AddBoolToObject(o, "enable", c->enable);
        cJSON_AddNumberToObject(o, "uart", c->uart_num);
        cJSON_AddNumberToObject(o, "tx", c->tx_gpio);
        cJSON_AddNumberToObject(o, "rx", c->rx_gpio);
        cJSON_AddNumberToObject(o, "baud", c->baud);
        cJSON_AddNumberToObject(o, "port", c->port);
        cJSON_AddItemToArray(arr, o);
    }

    cJSON_AddItemToObject(root, "serial", arr);
}

char *app_config_to_json(bool pretty)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *o = cJSON_CreateObject();
    if (o) {
        cJSON_AddStringToObject(o, "target", s_cfg.target);
        cJSON_AddStringToObject(o, "interface", s_cfg.iface == '0' ? "jtag" : "swd");
        cJSON_AddStringToObject(o, "rtos", s_cfg.rtos);
        cJSON_AddStringToObject(o, "flash_size", s_cfg.flash_size);
        cJSON_AddNumberToObject(o, "debug_level", s_cfg.debug_level - '0');
        cJSON_AddBoolToObject(o, "dual_core", s_cfg.dual_core == '3');
        cJSON_AddStringToObject(o, "extra_command", s_cfg.extra_command);
        cJSON_AddStringToObject(o, "gpio_jtag", s_cfg.gpio_jtag);
        cJSON_AddStringToObject(o, "gpio_swd", s_cfg.gpio_swd);
        cJSON_AddNumberToObject(o, "rtt_poll_ms", s_cfg.rtt_poll_ms);
        cJSON_AddItemToObject(root, "openocd", o);
    }

    cJSON *w = cJSON_CreateObject();
    if (w) {
        cJSON_AddStringToObject(w, "ssid", s_cfg.wifi_ssid);
        cJSON_AddStringToObject(w, "pass", s_cfg.wifi_pass);
        cJSON_AddBoolToObject(w, "power_save", s_cfg.wifi_power_save);
        cJSON_AddItemToObject(root, "wifi", w);
    }

    serial_to_json(root);

    char *s = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    return s;
}

/* ------------------------------------------------------------------ */
/* JSON 反序列化 + 校验                                                */

/* 复制一个字符串字段，超长直接报错（不静默截断——手改了 JSON 却
 * 悄悄丢掉后半截，比报错更难查）。 */
static esp_err_t take_str(char *dst, size_t cap, const char *src,
                          const char *name, char *err, size_t err_len)
{
    if (strlen(src) >= cap) {
        return fail(err, err_len, "%s 太长（上限 %u 字符）", name, (unsigned)(cap - 1));
    }

    set_str(dst, cap, src);
    return ESP_OK;
}

esp_err_t app_config_from_json(const char *json, char *err, size_t err_len)
{
    if (!json) {
        return fail(err, err_len, "empty body");
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return fail(err, err_len, "JSON 语法错误");
    }

    /* 在副本上改：校验失败时内存镜像保持原样，不会留下改了一半的配置 */
    app_config_t tmp = s_cfg;
    esp_err_t e = ESP_OK;

    cJSON *o = cJSON_GetObjectItem(root, "openocd");
    if (o && cJSON_IsObject(o)) {
        const char *s;

        s = json_str(o, "target", NULL);
        if (s) {
            e = take_str(tmp.target, sizeof(tmp.target), s, "openocd.target", err, err_len);
        }

        s = json_str(o, "interface", NULL);
        if (!e && s) {
            if (!strcmp(s, "jtag")) {
                tmp.iface = '0';
            } else if (!strcmp(s, "swd")) {
                tmp.iface = '1';
            } else {
                e = fail(err, err_len, "openocd.interface 只能是 \"jtag\" 或 \"swd\"");
            }
        }

        s = json_str(o, "rtos", NULL);
        if (!e && s) {
            e = take_str(tmp.rtos, sizeof(tmp.rtos), s, "openocd.rtos", err, err_len);
        }

        s = json_str(o, "flash_size", NULL);
        if (!e && s) {
            e = take_str(tmp.flash_size, sizeof(tmp.flash_size), s, "openocd.flash_size", err, err_len);
        }

        s = json_str(o, "extra_command", NULL);
        if (!e && s) {
            e = take_str(tmp.extra_command, sizeof(tmp.extra_command), s,
                         "openocd.extra_command", err, err_len);
        }

        s = json_str(o, "gpio_jtag", NULL);
        if (!e && s) {
            e = take_str(tmp.gpio_jtag, sizeof(tmp.gpio_jtag), s, "openocd.gpio_jtag", err, err_len);
        }

        s = json_str(o, "gpio_swd", NULL);
        if (!e && s) {
            e = take_str(tmp.gpio_swd, sizeof(tmp.gpio_swd), s, "openocd.gpio_swd", err, err_len);
        }

        if (!e && cJSON_GetObjectItem(o, "debug_level")) {
            int d = json_int(o, "debug_level", tmp.debug_level - '0');
            if (d < 1 || d > 4) {
                e = fail(err, err_len, "openocd.debug_level 必须在 1..4");
            } else {
                tmp.debug_level = (char)(d + '0');
            }
        }

        if (!e && cJSON_GetObjectItem(o, "dual_core")) {
            tmp.dual_core = json_bool(o, "dual_core", false) ? '3' : '1';
        }

        if (!e && cJSON_GetObjectItem(o, "rtt_poll_ms")) {
            int p = json_int(o, "rtt_poll_ms", APP_CFG_RTT_POLL_DEFAULT);
            if (p < APP_CFG_RTT_POLL_MIN || p > APP_CFG_RTT_POLL_MAX) {
                e = fail(err, err_len, "openocd.rtt_poll_ms 必须在 %d..%d",
                         APP_CFG_RTT_POLL_MIN, APP_CFG_RTT_POLL_MAX);
            } else {
                tmp.rtt_poll_ms = p;
            }
        }
    }

    cJSON *w = cJSON_GetObjectItem(root, "wifi");
    if (!e && w && cJSON_IsObject(w)) {
        const char *s;

        s = json_str(w, "ssid", NULL);
        if (s) {
            e = take_str(tmp.wifi_ssid, sizeof(tmp.wifi_ssid), s, "wifi.ssid", err, err_len);
        }

        s = json_str(w, "pass", NULL);
        if (!e && s) {
            e = take_str(tmp.wifi_pass, sizeof(tmp.wifi_pass), s, "wifi.pass", err, err_len);
        }

        if (!e && cJSON_GetObjectItem(w, "power_save")) {
            tmp.wifi_power_save = json_bool(w, "power_save", true);
        }
    }

    cJSON *arr = cJSON_GetObjectItem(root, "serial");
    if (!e && arr && cJSON_IsArray(arr)) {
        if (cJSON_GetArraySize(arr) != SERIAL_BRIDGE_CHANNEL_NUM) {
            e = fail(err, err_len, "serial 必须是 %d 个元素的数组", SERIAL_BRIDGE_CHANNEL_NUM);
        } else {
            for (int i = 0; !e && i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
                cJSON *c = cJSON_GetArrayItem(arr, i);
                serial_bridge_channel_cfg_t *dst = &tmp.serial.ch[i];

                if (!cJSON_IsObject(c)) {
                    e = fail(err, err_len, "serial[%d] 不是对象", i);
                    break;
                }

                dst->enable   = json_bool(c, "enable", dst->enable);
                dst->uart_num = json_int(c, "uart", dst->uart_num);
                dst->tx_gpio  = json_int(c, "tx", dst->tx_gpio);
                dst->rx_gpio  = json_int(c, "rx", dst->rx_gpio);
                dst->baud     = json_int(c, "baud", dst->baud);
                dst->port     = json_int(c, "port", dst->port);
            }

            /* 复用 serial_bridge 的完整检查：引脚是否被占用、通道间冲突、波特率范围 */
            if (!e && serial_bridge_validate(&tmp.serial) != ESP_OK) {
                e = fail(err, err_len, "serial 配置不合法（引脚被占用 / 通道间冲突 / 端口或波特率越界）");
            }
        }
    }

    cJSON_Delete(root);

    if (e != ESP_OK) {
        return e;
    }

    s_cfg = tmp;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 读写 NVS                                                            */

esp_err_t app_config_save(void)
{
    char *json = app_config_to_json(false);
    if (!json) {
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = storage_write(APP_CONFIG_NVS_KEY, json, strlen(json));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "save failed (%s)", esp_err_to_name(e));
    }

    free(json);
    return e;
}

esp_err_t app_config_load(void)
{
    app_config_reset_defaults();

    size_t len = storage_get_value_length(APP_CONFIG_NVS_KEY);
    if (len > 0 && len <= APP_CFG_JSON_MAX) {
        char *buf = calloc(1, len + 1);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }

        if (storage_read(APP_CONFIG_NVS_KEY, buf, len) == ESP_OK) {
            char err[96] = {0};
            if (app_config_from_json(buf, err, sizeof(err)) == ESP_OK) {
                free(buf);
                ESP_LOGI(TAG, "loaded from nvs (%u bytes)", (unsigned)len);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "stored json rejected (%s), falling back to legacy keys", err);
        }

        free(buf);
    }

    /* 首次升级（或 JSON 损坏）：从老的散键迁移一份，立刻落盘固化 */
    legacy_migrate();
    if (app_config_save() != ESP_OK) {
        ESP_LOGW(TAG, "could not persist migrated config");
    }

    return ESP_OK;
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

app_config_t *app_config_mutable(void)
{
    return &s_cfg;
}
