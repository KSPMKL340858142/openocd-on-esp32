#include "sdkconfig.h"

#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_check.h"
#include "driver/uart.h"

#include "types.h"
#include "app_config.h"
#include "storage.h"
#include "network.h"
#include "web_server.h"
#include "ui.h"
#include "openocd.h"
#include "serial_bridge/serial_bridge.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#include "driver/uart_vfs.h"
#define vfs_dev_port_set_rx_line_endings uart_vfs_dev_port_set_rx_line_endings
#define vfs_dev_port_set_tx_line_endings uart_vfs_dev_port_set_tx_line_endings
#define vfs_dev_uart_use_driver uart_vfs_dev_use_driver
#else
#include "esp_vfs_dev.h"
#define vfs_dev_port_set_rx_line_endings esp_vfs_dev_uart_port_set_rx_line_endings
#define vfs_dev_port_set_tx_line_endings esp_vfs_dev_uart_port_set_tx_line_endings
#define vfs_dev_uart_use_driver esp_vfs_dev_uart_use_driver
#endif

static const char *TAG = "main";

app_params_t g_app_params;

static void init_console(void)
{
    /* Drain stdout before reconfiguring it */
    fflush(stdout);
    fsync(fileno(stdout));

    /* Disable buffering on stdin */
    setvbuf(stdin, NULL, _IONBF, 0);
    /* Configure line endings */
    vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_LF);
    vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    /* Configure UART. Note that REF_TICK is used so that the baud rate remains
     * correct while APB frequency is changing in light sleep mode.
     */
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
        .source_clk = UART_SCLK_REF_TICK,
#else
        .source_clk = UART_SCLK_XTAL,
#endif
    };
    /* Install UART driver for interrupt-driven reads and writes */
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config));

    /* Tell VFS to use UART driver */
    vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
}

static bool is_espressif_target(const char *cfg_file)
{
    const char *prefix = "esp32";
    size_t prefix_length = strlen(prefix);

    if (strlen(cfg_file) < prefix_length) {
        return false;
    }

    if (strncmp(cfg_file, prefix, prefix_length) == 0) {
        return true;
    }

    return false;
}

void run_openocd(void)
{
    setenv("OPENOCD_SCRIPTS", "/data", 1);
    const char *argv[20] = {
        "openocd",
        "-c", "bindto 0.0.0.0; set ESP_IDF_HOST 1"
    };
    /* Constant OpenOCD parameters takes 3 index */
    int argc = 3;

    char iface[32] = {0};
    sprintf(iface, "interface/esp_gpio_%s.cfg", g_app_params.interface == '0' ? "jtag" : "swd");
    argv[argc++] = "-f";
    argv[argc++] = iface;

    /* 用户可配 GPIO 引脚：注入在 interface cfg 之后（此时 esp_gpio driver 已注册，
     * 覆盖 cfg 内默认 38 39 40 41 / 38 39）。须在 openocd_main 之前保持有效，
     * 故 gpio_cmd 定义在函数级（与 command 同级）。 */
    char gpio_cmd[64] = {0};
    if (g_app_params.interface == '0') {
        sprintf(gpio_cmd, "esp_gpio_jtag_nums %s", g_app_params.gpio_jtag);
    } else {
        sprintf(gpio_cmd, "esp_gpio_swd_nums %s", g_app_params.gpio_swd);
    }
    argv[argc++] = "-c";
    argv[argc++] = gpio_cmd;

    char config[64] = {0};
    sprintf(config, "target/%s", g_app_params.config_file);

    char command[128] = {0};
    if (is_espressif_target(g_app_params.config_file)) {
        argv[argc++] = "-c";
        sprintf(command, "set ESP_FLASH_SIZE %s; set ESP_RTOS %s; set ESP_ONLYCPU %c",
                g_app_params.flash_size, g_app_params.rtos_type, g_app_params.dual_core);
        argv[argc++] = command;
    }
    if (strlen(g_app_params.command_arg)) {
        argv[argc++] = "-c";
        argv[argc++] = g_app_params.command_arg;
    }

    /* RTT 上行轮询间隔：cfg 侧写成 if {[info exists RTT_POLL_MS]} {
     * rtt polling_interval $RTT_POLL_MS }，所以必须在 -f target/xxx.cfg
     * 之前注入，否则 cfg 执行时还看不到这个变量。
     * 轮询间隔决定了 RTT 上行速率的配置上限
     * （min(目标缓冲, 1024) ÷ 轮询间隔），是唯一不用重编就能调的旋钮。 */
    char rtt_cmd[48] = {0};
    snprintf(rtt_cmd, sizeof(rtt_cmd), "set RTT_POLL_MS %d", app_config_get()->rtt_poll_ms);
    argv[argc++] = "-c";
    argv[argc++] = rtt_cmd;

    /* target config must be set after extra commands */
    argv[argc++] = "-f";
    argv[argc++] = config;

    char debug_level[8] = {0};
    sprintf(debug_level, "-d%c", g_app_params.debug_level);
    argv[argc++] = debug_level;

    // suppress gpio init logs
    esp_log_level_set("gpio", ESP_LOG_WARN);

    int ret = openocd_main(argc, (char **)argv);

    ESP_LOGI(TAG, "OpenOCD should not return! ret(%d)", ret);
}

void load_openocd_params(void)
{
    /* 全部取自统一配置（NVS 里那份 JSON）。这里的指针指向 app_config 内部的
     * 静态字段、生命周期贯穿整个程序，所以不再需要原来的堆分配（那些内存
     * 分配后从不释放，属于泄漏式写法）。 */
    const app_config_t *c = app_config_get();

    g_app_params.config_file = c->target;
    g_app_params.interface   = c->iface;
    g_app_params.rtos_type   = c->rtos;
    g_app_params.flash_size  = c->flash_size;
    g_app_params.dual_core   = c->dual_core;
    g_app_params.debug_level = c->debug_level;
    g_app_params.command_arg = c->extra_command;

    strncpy(g_app_params.gpio_jtag, c->gpio_jtag, sizeof(g_app_params.gpio_jtag) - 1);
    g_app_params.gpio_jtag[sizeof(g_app_params.gpio_jtag) - 1] = 0;
    strncpy(g_app_params.gpio_swd, c->gpio_swd, sizeof(g_app_params.gpio_swd) - 1);
    g_app_params.gpio_swd[sizeof(g_app_params.gpio_swd) - 1] = 0;

    /* 回填 UI 控件（与配置保持一致的显示） */
    ui_update_interface_dropdown(g_app_params.interface - '0');
    ui_update_flash_checkbox(strcmp(g_app_params.flash_size, "0") != 0);
    ui_update_dual_core_checkbox(g_app_params.dual_core != '1');
    ui_update_debug_level_dropdown(g_app_params.debug_level - '0' - 1);

    ESP_LOGI(TAG, "config file (%s)", g_app_params.config_file);
    ESP_LOGI(TAG, "rtos type (%s)", g_app_params.rtos_type);
    ESP_LOGI(TAG, "flash size (%s)", g_app_params.flash_size);
    ESP_LOGI(TAG, "dual core (%c)", g_app_params.dual_core);
    ESP_LOGI(TAG, "interface (%s)", g_app_params.interface == '0' ? "jtag" : "swd");
    ESP_LOGI(TAG, "command arg (%s)", g_app_params.command_arg);
    ESP_LOGI(TAG, "debug_level (%c)", g_app_params.debug_level);
    ESP_LOGI(TAG, "gpio jtag (%s) swd (%s)", g_app_params.gpio_jtag, g_app_params.gpio_swd);
    ESP_LOGI(TAG, "rtt poll (%d ms)", c->rtt_poll_ms);
}

/*
 * If there is no wifi settings stored into nvs, we will look at the menuconfig.
 * If menuconfig still has default values, we will start AP mode
 * If menuconfig has user settings, we will start STA mode
*/
void load_network_params(void)
{
    /* WiFi 凭据也来自统一配置。空串 = 还没配过，
     * 保持升级前的行为：回落到 menuconfig 值，或进 AP 配网模式。 */
    const app_config_t *c = app_config_get();

    ESP_LOGI(TAG, "ssid_len:%d", (int)strlen(c->wifi_ssid));

    if (strlen(c->wifi_ssid) == 0) {
        ESP_LOGW(TAG, "No WiFi credentials in config.");
        if (!strcmp(CONFIG_ESP_WIFI_SSID, WIFI_AP_SSSID)) {
            ESP_LOGW(TAG, "Default values read from menuconfig. AP mode will be activated!");
            g_app_params.mode = APP_MODE_AP;
            strcpy(g_app_params.wifi_ssid, WIFI_AP_SSSID);
            strcpy(g_app_params.wifi_pass, WIFI_AP_PASSW);
        } else {
            ESP_LOGW(TAG, "User values read from menuconfig. STA mode will be activated!");
            g_app_params.mode = APP_MODE_STA;
            strcpy(g_app_params.wifi_ssid, CONFIG_ESP_WIFI_SSID);
            strcpy(g_app_params.wifi_pass, CONFIG_ESP_WIFI_PASSWORD);
        }
    } else {
        g_app_params.mode = APP_MODE_STA;
        strncpy(g_app_params.wifi_ssid, c->wifi_ssid, sizeof(g_app_params.wifi_ssid) - 1);
        g_app_params.wifi_ssid[sizeof(g_app_params.wifi_ssid) - 1] = 0;
        strncpy(g_app_params.wifi_pass, c->wifi_pass, sizeof(g_app_params.wifi_pass) - 1);
        g_app_params.wifi_pass[sizeof(g_app_params.wifi_pass) - 1] = 0;
    }

    if (g_app_params.mode == APP_MODE_AP) {
        g_app_params.net_adapter_name = "wifiap";
    } else if (g_app_params.mode == APP_MODE_STA) {
        g_app_params.net_adapter_name = "wifista";
    }

    ESP_LOGI(TAG, "app mode (%s)", g_app_params.mode == APP_MODE_AP ? "Access Point" : "Station");
    ESP_LOGI(TAG, "wifi ssid (%s)", g_app_params.wifi_ssid);
    ESP_LOGI(TAG, "wifi pass (%s)", g_app_params.wifi_pass);
}

void init_idf_components(void)
{
    ESP_LOGI(TAG, "Setting up...");
    init_console();
    ESP_ERROR_CHECK(storage_init_filesystem());
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

void app_main(void)
{
    init_idf_components();

    /* 统一配置必须最先加载：下面的 UI / OpenOCD / WiFi / 串口桥都从它取值。
     * 读不到 JSON 时（首次升级）会从老的散装 NVS 键迁移一份并落盘。 */
    app_config_load();

    ui_init();
    load_openocd_params();
    load_network_params();

    if (storage_update_target_struct() != ESP_OK) {
        ui_show_info_screen("Target list can not be read. Please check tcl-lite directory");
        goto _wait;
    }
    storage_update_rtos_struct();

#if CONFIG_UI_ENABLE
    if (g_app_params.mode == APP_MODE_AP) {
        g_app_params.net_adapter_name = "wifiprov";
        ui_show_qr_screen();
    } else {
        ui_show_info_screen("Waiting for the network connection...");
    }
#endif

    /* We may need http_handle in the next steps. So starting here makes sense. */
    httpd_handle_t http_handle;
    if (web_server_start(&http_handle) != ESP_OK) {
        ui_show_info_screen("Web server couldn't be started!");
        goto _wait;
    }

    struct network_init_config config = {
        .adapter_name = g_app_params.net_adapter_name,
        .ssid = g_app_params.wifi_ssid,
        .pass = g_app_params.wifi_pass,
        .http_handle = &http_handle
    };

    if (network_start(&config) == ESP_OK) {

        if (!strcmp(config.adapter_name, "wifiprov")) {
            network_get_sta_credentials(g_app_params.wifi_ssid, g_app_params.wifi_pass);

            /* 配网拿到的凭据写进统一配置（原来的两个散键不再写） */
            app_config_t *cc = app_config_mutable();
            strncpy(cc->wifi_ssid, g_app_params.wifi_ssid, sizeof(cc->wifi_ssid) - 1);
            cc->wifi_ssid[sizeof(cc->wifi_ssid) - 1] = 0;
            strncpy(cc->wifi_pass, g_app_params.wifi_pass, sizeof(cc->wifi_pass) - 1);
            cc->wifi_pass[sizeof(cc->wifi_pass) - 1] = 0;
            if (app_config_save() != ESP_OK) {
                ESP_LOGE(TAG, "Failed to persist WiFi credentials");
            }

            ui_show_info_screen("Wifi credentials have been changed. Restarting in 2 seconds...");
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            esp_restart();
        }

        network_get_my_ip(g_app_params.my_ip);
        ui_update_ip_info(g_app_params.my_ip);

        serial_bridge_start();          /* 多路无线串口透传（每个启用通道一个任务，CPU1） */

        ui_show_info_screen("OpenOCD has been launched.");
        run_openocd();
    } else {
        ui_show_info_screen("Network connection can not be establised. Please check your wifi credentials!");
        goto _wait;
    }

    ESP_LOGW(TAG, "You can reset the board or send the new config from the web interface");

    ui_show_info_screen("OpenOCD stopped running or couldn't communicate with the target. Please check your configuration parameters.");

_wait:
    while (true) {
        ESP_LOGI(TAG, "Waiting for the new config...");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
    esp_restart();
}
