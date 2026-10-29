#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include <cJSON.h>

#include "web_server.h"
#include "app_config.h"
#include "storage.h"
#include "ui.h"
#include "types.h"
#include "serial_bridge/serial_bridge.h"
#include "network_mngr.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

static const char *TAG = "web-server";

/* 复制一个来自表单的字符串到配置字段。
 * 容量不够时返回 false，由调用方回 400——静默截断在 WiFi 密码上会变成
 * 「保存成功了但连不上」，最难查。 */
static bool copy_field(char *dst, size_t cap, const char *src)
{
    if (!src || strlen(src) >= cap) {
        return false;
    }

    snprintf(dst, cap, "%s", src);
    return true;
}

esp_err_t website_handler(httpd_req_t *req)
{
    extern const unsigned char website_html_start[] asm("_binary_website_html_start");
    extern const unsigned char website_html_end[]   asm("_binary_website_html_end");
    const size_t website_html_size = (website_html_end - website_html_start);

    return httpd_resp_send(req, (const char *)website_html_start, website_html_size);
}

esp_err_t esp_logo_handler(httpd_req_t *req)
{
    extern const unsigned char esp_logo_svg_start[] asm("_binary_esp_logo_svg_start");
    extern const unsigned char esp_logo_svg_end[]   asm("_binary_esp_logo_svg_end");
    const size_t esp_logo_size = (esp_logo_svg_end - esp_logo_svg_start);

    httpd_resp_set_type(req, "image/svg+xml");

    return httpd_resp_send(req, (const char *)esp_logo_svg_start, esp_logo_size);
}

esp_err_t favicon_handler(httpd_req_t *req)
{
    extern const unsigned char favicon_ico_start[] asm("_binary_favicon_ico_start");
    extern const unsigned char favicon_ico_end[]   asm("_binary_favicon_ico_end");
    const size_t favicon_ico_size = (favicon_ico_end - favicon_ico_start);

    httpd_resp_set_type(req, "image/x-icon");

    return httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_size);
}

static cJSON *set_handler_start(httpd_req_t *req, char *json_buf, uint32_t json_buf_sz)
{
    memset(json_buf, 0x00, json_buf_sz);

    /* Read data received in the request */
    int ret = httpd_req_recv(req, json_buf, json_buf_sz);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return NULL;
    }

    ESP_LOGI(TAG, "json : %s", json_buf);

    cJSON *root = cJSON_ParseWithLength(json_buf, json_buf_sz);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Json parse error");
        return NULL;
    }
    return root;
}

esp_err_t set_credentials_handler(httpd_req_t *req)
{
    char json[req->content_len];

    cJSON *root = set_handler_start(req, json, sizeof(json));
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "pass");
    /* pass 也要判 valuestring：空密码（开放网络）时前端可能只发 key 不发值，
     * 只判 !pass 会让下面 strlen(NULL) 直接崩。 */
    if (!pass || !ssid || !ssid->valuestring || !pass->valuestring) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Json parse error");
        return ESP_FAIL;
    }

    app_config_t *cfg = app_config_mutable();
    if (!copy_field(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), ssid->valuestring) ||
        !copy_field(cfg->wifi_pass, sizeof(cfg->wifi_pass), pass->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID 或密码超过长度上限");
        return ESP_FAIL;
    }
    cJSON_Delete(root);

    if (app_config_save() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "Wi-Fi 设置已保存，设备将自动重启。");

    ui_show_info_screen("Wifi credentials have been changed. Restarting in 2 seconds...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();

    return  ESP_OK;
}

esp_err_t set_openocd_config_handler(httpd_req_t *req)
{
    char json[req->content_len];

    cJSON *root = set_handler_start(req, json, sizeof(json));
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *target = cJSON_GetObjectItem(root, "target");
    cJSON *interface = cJSON_GetObjectItem(root, "interface");
    cJSON *rtos = cJSON_GetObjectItem(root, "rtos");
    cJSON *debug = cJSON_GetObjectItem(root, "debug");
    cJSON *dualCore = cJSON_GetObjectItem(root, "dualCore");
    cJSON *flash = cJSON_GetObjectItem(root, "flash");
    cJSON *cParam = cJSON_GetObjectItem(root, "cParam");

    // Print the values
    ESP_LOGI(TAG, "Target: %s", target->valuestring);
    ESP_LOGI(TAG, "Interface: %s", interface->valuestring[0] == '0' ? "jtag" : "swd");
    ESP_LOGI(TAG, "RTOS: %s", rtos->valuestring);
    ESP_LOGI(TAG, "Debug: %s", debug->valuestring);
    ESP_LOGI(TAG, "Dual Core: %s", dualCore->type == cJSON_True ? "true" : "false");
    ESP_LOGI(TAG, "Flash: %s", flash->type == cJSON_True ? "true" : "false");
    ESP_LOGI(TAG, "C Param: %s", cParam->valuestring);

    app_config_t *cfg = app_config_mutable();

    if (!copy_field(cfg->target, sizeof(cfg->target), target->valuestring) ||
        !copy_field(cfg->rtos, sizeof(cfg->rtos), rtos->valuestring) ||
        !copy_field(cfg->extra_command, sizeof(cfg->extra_command), cParam->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target / rtos / cParam 超过长度上限");
        return ESP_FAIL;
    }

    cfg->iface = (interface->valuestring[0] == '0') ? '0' : '1';
    cfg->debug_level = debug->valuestring[0];
    cfg->dual_core = (dualCore->type == cJSON_True) ? '3' : '1';
    copy_field(cfg->flash_size, sizeof(cfg->flash_size),
               (flash->type == cJSON_True) ? "auto" : "0");

    // Cleanup
    cJSON_Delete(root);

    if (app_config_save() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "调试配置已保存，设备将自动重启。");

    ui_show_info_screen("OpenOCD parameters have been changed. Restarting in 2 seconds...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();

    return ESP_OK;
}

esp_err_t get_openocd_config_handler(httpd_req_t *req)
{
    // Create the config list array
    cJSON *config_list = cJSON_CreateArray();
    for (size_t i = 0; i < g_app_params.target_count; ++i) {
        cJSON_AddItemToArray(config_list, cJSON_CreateString(g_app_params.target_list[i]));
    }

    // Create the rtos list array
    cJSON *rtos_list = cJSON_CreateArray();
    for (size_t i = 0; i < g_app_params.rtos_count; ++i) {
        cJSON_AddItemToArray(rtos_list, cJSON_CreateString(g_app_params.rtos_list[i]));
    }

    // Create the JSON object and add the arrays as properties
    cJSON *json_object = cJSON_CreateObject();
    cJSON_AddItemToObject(json_object, "configList", config_list);
    cJSON_AddItemToObject(json_object, "rtosList", rtos_list);

    /* 当前生效值。
     * 只回列表的话，网页上这些控件显示的是 HTML 里写死的默认值
     * （debug=2、dual-core/flash 默认勾选、interface 默认 jtag），
     * 跟程序实际在用的配置对不上，改配置时很容易误判。 */
    const app_config_t *c = app_config_get();
    cJSON *cur = cJSON_CreateObject();
    if (cur) {
        char dbg[2] = { c->debug_level, 0 };
        cJSON_AddStringToObject(cur, "target", c->target);
        cJSON_AddStringToObject(cur, "interface", c->iface == '0' ? "0" : "1");
        cJSON_AddStringToObject(cur, "rtos", c->rtos);
        cJSON_AddStringToObject(cur, "debug", dbg);
        cJSON_AddBoolToObject(cur, "dualCore", c->dual_core == '3');
        cJSON_AddBoolToObject(cur, "flash", strcmp(c->flash_size, "0") != 0);
        cJSON_AddStringToObject(cur, "cParam", c->extra_command);
        cJSON_AddItemToObject(json_object, "current", cur);
    }

    // Convert the JSON object to a string
    char *json_string = cJSON_PrintUnformatted(json_object);

    ESP_LOGD(TAG, "openocd_config json : %s", json_string);

    cJSON_Delete(json_object);

    esp_err_t err = httpd_resp_send(req, json_string, HTTPD_RESP_USE_STRLEN);

    free(json_string);

    return err;
}

/* 取 URI 最后一段作为文件名。
 * 必须带容量：原来是无脑 strcpy 到 64 字节的栈数组，
 * 而 httpd 的 URI 上限是 512 字节，构造一个超长名字就能写穿栈。
 * 只取最后一段也顺带免疫了 ../ 路径穿越。 */
static void get_filename_from_path(const char *path, char *filename, size_t cap)
{
    const char *last_slash = strrchr(path, '/');
    const char *src = last_slash ? last_slash + 1 : path;
    size_t n = strlen(src);

    if (n >= cap) {
        n = cap - 1;
    }

    memcpy(filename, src, n);
    filename[n] = 0;
}

static esp_err_t file_upload_handler(httpd_req_t *req)
{
    struct stat file_stat;
    char filename[64] = {0};
    char filepath[128] = {0};

    get_filename_from_path(req->uri, filename, sizeof(filename));
    if (filename[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s%s", CFG_FILE_PATH, filename);

    if (stat(filepath, &file_stat) == 0) {
        ESP_LOGE(TAG, "File already exists : %s", filepath);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File already exists!");
        return ESP_FAIL;
    }

    // Open the file for writing
    FILE *fp = fopen(filepath, "w");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    char buf[1024];
    size_t remaining = req->content_len;
    const uint8_t poll_period = 10; /* ms */
    uint16_t try_count = 1000 / poll_period; /* 1 second timeout */

    while (try_count) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);

        // Read a chunk of data from the request
        int received = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }

            /* In case of unrecoverable error, close and delete the unfinished file*/
            fclose(fp);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        /* Write buffer content to file on storage */
        if (received != fwrite(buf, 1, received, fp)) {
            /* Couldn't write everything to file! Storage may be full? */
            fclose(fp);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            return ESP_FAIL;
        }

        /* Keep track of remaining size of the file left to be uploaded */
        remaining -= received;
        if (!remaining) {
            break;
        }

        /* If still there is remaining data, something might be wrong. */
        try_count--;
        vTaskDelay(pdMS_TO_TICKS(poll_period));
    }

    // Close the file
    fclose(fp);
    ESP_LOGI(TAG, "File received: %s", filepath);

    storage_update_target_struct();

    // Send the response indicating success
    httpd_resp_sendstr(req, "文件上传成功。");

    return ESP_OK;
}

static esp_err_t file_delete_handler(httpd_req_t *req)
{
    char filepath[128] = {0};
    char filename[64] = {0};

    get_filename_from_path(req->uri, filename, sizeof(filename));

    /* 空名字会让 filepath 变成目录本身（/data/target/）：
     * stat() 对已存在的目录返回成功，接着 unlink() 必然失败却不检查，
     * 最后还回一句「删除成功」——纯误导。 */
    if (filename[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少文件名");
        return ESP_FAIL;
    }

    snprintf(filepath, sizeof(filepath), "%s%s", CFG_FILE_PATH, filename);

    ESP_LOGI(TAG, "filepath:%s", filepath);

    struct stat file_stat;

    if (stat(filepath, &file_stat) != 0) {
        ESP_LOGE(TAG, "File not exists : %s", filepath);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File not exists!");
        return ESP_FAIL;
    }

    /* 原来不检查返回值：对目录、只读等失败情形都会照样回「删除成功」 */
    if (unlink(filepath) != 0) {
        ESP_LOGE(TAG, "unlink failed (%s): %s", strerror(errno), filepath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "删除失败");
        return ESP_FAIL;
    }

    /* 删掉的如果正是当前在用的 target，必须把配置改指向剩下的某个文件。
     * 否则配置里留着一条指向已删文件的悬挂引用，重启后 run_openocd()
     * 去加载 target/xxx.cfg 直接失败，OpenOCD 起不来。 */
    bool was_current = (strcmp(app_config_get()->target, filename) == 0);

    if (storage_update_target_struct() != ESP_OK) {
        ESP_LOGW(TAG, "target list refresh failed");
    }

    if (was_current) {
        app_config_t *c = app_config_mutable();

        if (g_app_params.target_list && g_app_params.target_count > 0) {
            snprintf(c->target, sizeof(c->target), "%s", g_app_params.target_list[0]);
            ESP_LOGW(TAG, "current target deleted, switched to %s", c->target);
        } else {
            /* 一个都不剩：留空。启动时会明确报 target 不可用，比指向幽灵文件好查 */
            c->target[0] = 0;
            ESP_LOGW(TAG, "no target cfg left after deletion");
        }

        if (app_config_save() != ESP_OK) {
            ESP_LOGE(TAG, "failed to persist target change");
        }
    }

    // Send the response indicating success
    httpd_resp_sendstr(req, "文件删除成功。");

    return ESP_OK;

}

/* ---------- 串口透传配置 REST ---------- */

static void channel_to_json(cJSON *arr, int idx, const serial_bridge_channel_cfg_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "enable", cJSON_CreateBool(c->enable));
    cJSON_AddItemToObject(o, "uart",   cJSON_CreateNumber(c->uart_num));
    cJSON_AddItemToObject(o, "tx",     cJSON_CreateNumber(c->tx_gpio));
    cJSON_AddItemToObject(o, "rx",     cJSON_CreateNumber(c->rx_gpio));
    cJSON_AddItemToObject(o, "baud",   cJSON_CreateNumber(c->baud));
    cJSON_AddItemToObject(o, "port",   cJSON_CreateNumber(c->port));
    cJSON_AddItemToArray(arr, o);
}

static esp_err_t get_serial_config_handler(httpd_req_t *req)
{
    serial_bridge_cfgs_t cfgs;
    if (serial_bridge_get_configs(&cfgs) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read serial config");
        return ESP_FAIL;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        channel_to_json(arr, i, &cfgs.ch[i]);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "serial", arr);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static int json_num(cJSON *o, const char *k, int fallback)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (!v || v->type != cJSON_Number) return fallback;
    return v->valueint;
}

static bool json_bool(cJSON *o, const char *k, bool fallback)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (!v) return fallback;
    return cJSON_IsTrue(v);
}

static esp_err_t set_serial_config_handler(httpd_req_t *req)
{
    char json[req->content_len + 1];
    int ret = httpd_req_recv(req, json, req->content_len);
    if (ret <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error"); return ESP_FAIL; }
    json[ret] = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Json parse error"); return ESP_FAIL; }

    serial_bridge_cfgs_t cfgs;
    cJSON *arr = cJSON_GetObjectItem(root, "serial");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing serial array");
        return ESP_FAIL;
    }
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        if (!o) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need 2 channels"); return ESP_FAIL; }
        serial_bridge_channel_cfg_t *c = &cfgs.ch[i];
        c->enable    = json_bool(o, "enable", false);
        c->uart_num  = json_num(o, "uart", i == 0 ? 1 : 2);
        c->tx_gpio   = json_num(o, "tx", i == 0 ? 4 : 6);
        c->rx_gpio   = json_num(o, "rx", i == 0 ? 5 : 7);
        c->baud      = json_num(o, "baud", 115200);
        c->port      = json_num(o, "port", i == 0 ? 8888 : 8889);
    }
    cJSON_Delete(root);

    esp_err_t err = serial_bridge_set_configs(&cfgs);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid serial config");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "Serial config applied");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* WiFi 省电开关：GET /wifi_config -> {"ps":bool}
 *                  POST /wifi_config body {"ps":bool} -> 存储 + 热应用 */
static bool wifi_ps_current(void)
{
    return app_config_get()->wifi_power_save;   /* 缺省 = 省电开（低温低功耗） */
}

static esp_err_t get_wifi_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ps", wifi_ps_current());
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, s);
    free(s);
    return err;
}

static esp_err_t set_wifi_config_handler(httpd_req_t *req)
{
    char json[req->content_len + 1];
    int ret = httpd_req_recv(req, json, req->content_len);
    if (ret <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error"); return ESP_FAIL; }
    json[ret] = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Json parse error"); return ESP_FAIL; }
    cJSON *ps = cJSON_GetObjectItem(root, "ps");
    bool on = (ps && cJSON_IsTrue(ps)) ? true : false;
    cJSON_Delete(root);

    app_config_mutable()->wifi_power_save = on;
    if (app_config_save() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
        return ESP_FAIL;
    }
    network_mngr_set_power_save(on);       /* 热应用，无需重启 */
    httpd_resp_sendstr(req, on ? "Power save ON (modem sleep)"
                               : "Power save OFF (RF always on, low latency)");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* OpenOCD esp_gpio 引脚配置：GET /openocd_gpio 返回当前引脚，
 * POST 保存（需重启生效，与调试配置保存行为一致）。 */

/* 保留脚：0-48 范围内排除 UART0 console(43/44)、USB(19/20)、flash/PSRAM(26-37) */
static bool oocd_gpio_reserved(int gpio)
{
    if (gpio < 0 || gpio > 48) return true;
    if (gpio == 43 || gpio == 44) return true;
    if (gpio == 19 || gpio == 20) return true;
    if (gpio >= 26 && gpio <= 37) return true;
    return false;
}

/* 把配置里的引脚文本（"38 39"）解析到 out（cnt 个），空/缺省用 def。
 * 原先是直接从 NVS 读，现在文本来自 app_config；strtok 会改缓冲区，
 * 所以先拷一份。 */
static void oocd_gpio_parse(const char *text, int *out, int cnt, const int *def)
{
    if (!text || !strlen(text)) {
        for (int i = 0; i < cnt; i++) out[i] = def[i];
        return;
    }

    char buf[32] = {0};
    snprintf(buf, sizeof(buf), "%s", text);

    int idx = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ", &save); tok && idx < cnt; tok = strtok_r(NULL, " ", &save)) {
        out[idx++] = atoi(tok);
    }
    /* 数量不足补默认（防手改文件破坏格式） */
    for (int i = idx; i < cnt; i++) out[i] = def[i];
}

static esp_err_t get_openocd_gpio_handler(httpd_req_t *req)
{
    int jtag[4], swd[2];
    const int jtag_def[4] = {38, 39, 40, 41};
    const int swd_def[2] = {38, 39};
    const app_config_t *cfg = app_config_get();
    oocd_gpio_parse(cfg->gpio_jtag, jtag, 4, jtag_def);
    oocd_gpio_parse(cfg->gpio_swd, swd, 2, swd_def);

    cJSON *root = cJSON_CreateObject();
    cJSON *j = cJSON_CreateIntArray(jtag, 4);
    cJSON *s = cJSON_CreateIntArray(swd, 2);
    cJSON_AddItemToObject(root, "jtag", j);
    cJSON_AddItemToObject(root, "swd", s);
    char *str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, str);
    free(str);
    return err;
}

static esp_err_t set_openocd_gpio_handler(httpd_req_t *req)
{
    char json[req->content_len + 1];
    int ret = httpd_req_recv(req, json, req->content_len);
    if (ret <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error"); return ESP_FAIL; }
    json[ret] = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Json parse error"); return ESP_FAIL; }
    cJSON *jtag = cJSON_GetObjectItem(root, "jtag");
    cJSON *swd = cJSON_GetObjectItem(root, "swd");
    if (!jtag || !cJSON_IsArray(jtag) || cJSON_GetArraySize(jtag) != 4 ||
        !swd || !cJSON_IsArray(swd) || cJSON_GetArraySize(swd) != 2) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need jtag[4] and swd[2]");
        return ESP_FAIL;
    }
    /* 校验：范围 0-48 且非保留脚 */
    for (int i = 0; i < 4; i++) {
        int g = cJSON_GetArrayItem(jtag, i)->valueint;
        if (oocd_gpio_reserved(g)) {
            cJSON_Delete(root);
            char msg[64];
            snprintf(msg, sizeof(msg), "GPIO %d invalid or reserved (USB/flash/console)", g);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
            return ESP_FAIL;
        }
    }
    for (int i = 0; i < 2; i++) {
        int g = cJSON_GetArrayItem(swd, i)->valueint;
        if (oocd_gpio_reserved(g)) {
            cJSON_Delete(root);
            char msg[64];
            snprintf(msg, sizeof(msg), "GPIO %d invalid or reserved (USB/flash/console)", g);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
            return ESP_FAIL;
        }
    }

    app_config_t *cfg = app_config_mutable();
    snprintf(cfg->gpio_jtag, sizeof(cfg->gpio_jtag), "%d %d %d %d",
             cJSON_GetArrayItem(jtag, 0)->valueint, cJSON_GetArrayItem(jtag, 1)->valueint,
             cJSON_GetArrayItem(jtag, 2)->valueint, cJSON_GetArrayItem(jtag, 3)->valueint);
    snprintf(cfg->gpio_swd, sizeof(cfg->gpio_swd), "%d %d",
             cJSON_GetArrayItem(swd, 0)->valueint, cJSON_GetArrayItem(swd, 1)->valueint);
    cJSON_Delete(root);

    if (app_config_save() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "GPIO 已保存，设备将自动重启。");
    ui_show_info_screen("OpenOCD GPIO pins changed. Restarting in 2 seconds...");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 统一配置文件的导入导出。
 *
 * 只做「下载 / 上传」两个动作，网页本身不做文本编辑器——在电脑上用任意
 * 文本编辑器改这份 JSON，改完传回来即可。
 *
 *   下载：浏览器访问 http://esp32-ocd.local/config.json
 *   上传：curl -X POST --data-binary @config.json http://esp32-ocd.local/config.json
 */

static esp_err_t get_config_file_handler(httpd_req_t *req)
{
    char *json = app_config_to_json(true);      /* 带缩进，便于人读和手改 */
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    /* 让浏览器当附件下载，而不是在页面里展开 */
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"config.json\"");

    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t set_config_file_handler(httpd_req_t *req)
{
    /* content_len 是 size_t，别写 <= 0（会触发无符号比较恒假的警告） */
    if (req->content_len == 0 || req->content_len > APP_CFG_JSON_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "config json missing or too large");
        return ESP_FAIL;
    }

    /* 一次性读完：整份配置约 1 KB，分块拼反而容易出错 */
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        got += (size_t)r;
    }

    char err[96] = {0};
    esp_err_t e = app_config_from_json(body, err, sizeof(err));
    free(body);

    if (e != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
        return ESP_FAIL;
    }

    if (app_config_save() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config save failed");
        return ESP_FAIL;
    }

    /* 回完响应就重启：整份配置已经换掉，但运行态（g_app_params / OpenOCD /
     * WiFi）还是旧的。不重启的话网页读出来是新值、设备跑的是老值，
     * 正好又变成「显示与实际不一致」。必须先 sendstr 再重启，否则前端收不到 200。 */
    httpd_resp_sendstr(req, "配置已导入，设备将在 2 秒后重启并应用。");
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    esp_restart();
    return ESP_OK;
}

httpd_uri_t uri_get_config_file = {
    .uri       = "/config.json",
    .method    = HTTP_GET,
    .handler   = get_config_file_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_set_config_file = {
    .uri       = "/config.json",
    .method    = HTTP_POST,
    .handler   = set_config_file_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_get_main_page = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = website_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_get_logo = {
    .uri = "/esp_logo.svg",
    .method = HTTP_GET,
    .handler = esp_logo_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_get_favicon = {
    .uri = "/favicon.ico",
    .method = HTTP_GET,
    .handler = favicon_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_set_credentials = {
    .uri = "/set_credentials",
    .method = HTTP_POST,
    .handler = set_credentials_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_set_openocd_config = {
    .uri = "/set_openocd_config",
    .method = HTTP_POST,
    .handler = set_openocd_config_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_get_openocd_config = {
    .uri = "/get_openocd_config",
    .method = HTTP_GET,
    .handler = get_openocd_config_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_file_upload = {
    .uri       = "/upload/*",
    .method    = HTTP_POST,
    .handler   = file_upload_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_file_delete = {
    .uri       = "/delete/*",
    .method    = HTTP_POST,
    .handler   = file_delete_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_get_serial_config = {
    .uri       = "/serial_config",
    .method    = HTTP_GET,
    .handler   = get_serial_config_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_set_serial_config = {
    .uri       = "/serial_config",
    .method    = HTTP_POST,
    .handler   = set_serial_config_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_get_wifi_config = {
    .uri       = "/wifi_config",
    .method    = HTTP_GET,
    .handler   = get_wifi_config_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_get_openocd_gpio = {
    .uri       = "/openocd_gpio",
    .method    = HTTP_GET,
    .handler   = get_openocd_gpio_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_set_openocd_gpio = {
    .uri       = "/openocd_gpio",
    .method    = HTTP_POST,
    .handler   = set_openocd_gpio_handler,
    .user_ctx  = NULL
};

httpd_uri_t uri_set_wifi_config = {
    .uri       = "/wifi_config",
    .method    = HTTP_POST,
    .handler   = set_wifi_config_handler,
    .user_ctx  = NULL
};

esp_err_t web_server_start(httpd_handle_t *http_handle)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Running web server on other core to get better response time */
    config.core_id = 1;
    config.uri_match_fn = httpd_uri_match_wildcard;
    /* 目前注册 16 个；留点余量，否则以后加端点会静默注册失败 */
    config.max_uri_handlers = 20;
    /* 默认 4096 不够宽裕：/config.json 上传时栈上要放 app_config_t(~500 B)
     * + 错误缓冲，还要叠加 cJSON 解析帧；现有 handler 里还有按 content_len
     * 开 VLA 的写法。提到 8 KB 换掉这类偶发栈溢出。 */
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(http_handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server!");
        return ESP_FAIL;
    }

    httpd_register_uri_handler(*http_handle, &uri_get_main_page);
    httpd_register_uri_handler(*http_handle, &uri_get_logo);
    httpd_register_uri_handler(*http_handle, &uri_get_favicon);
    httpd_register_uri_handler(*http_handle, &uri_set_credentials);
    httpd_register_uri_handler(*http_handle, &uri_set_openocd_config);
    httpd_register_uri_handler(*http_handle, &uri_get_openocd_config);
    httpd_register_uri_handler(*http_handle, &uri_file_upload);
    httpd_register_uri_handler(*http_handle, &uri_file_delete);
    httpd_register_uri_handler(*http_handle, &uri_get_serial_config);
    httpd_register_uri_handler(*http_handle, &uri_set_serial_config);
    httpd_register_uri_handler(*http_handle, &uri_get_wifi_config);
    httpd_register_uri_handler(*http_handle, &uri_set_wifi_config);
    httpd_register_uri_handler(*http_handle, &uri_get_openocd_gpio);
    httpd_register_uri_handler(*http_handle, &uri_set_openocd_gpio);
    httpd_register_uri_handler(*http_handle, &uri_get_config_file);
    httpd_register_uri_handler(*http_handle, &uri_set_config_file);

    return ESP_OK;
}
