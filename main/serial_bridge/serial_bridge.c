/* Serial <-> TCP bridge, multi channel.
 *
 * 每个通道一个 FreeRTOS 任务（钉 CPU1，优先级 5）：
 *   UARTn <-> TCP server(:port)
 *
 * 热应用设计：改引脚/波特率/端口时只对串口侧做“原地重配置”，TCP 监听与
 * 已建立的客户端连接全程保持——不存在“旧任务退出掐连接”的窗口，客户端
 * 不会收到 RST。仅当通道被禁用时才真正停止任务（优雅关闭客户端）。
 */

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "serial_bridge.h"
#include "app_config.h"

#define TAG              "serial-bridge"
#define RX_BUF_SZ        4096
#define TX_BUF_SZ        2048
#define TASK_STACK       4096
#define TASK_PRIO        5
#define TASK_CORE        1
/* select 超时 = 串口(UART→TCP)数据的轮询粒度：UART 绕回的数据不会唤醒
 * select(它只听 socket)，只能靠超时轮询读回。50ms 会让小包高频场景每包
 * 白等 ~50ms(实测 73ms/包)。10ms 把延迟压到 ~15ms 级，空闲 CPU 开销可忽略 */
#define SELECT_TIMEOUT_MS  10

static serial_bridge_cfgs_t s_cfgs;          /* 当前配置（含默认值/持久化读回） */

typedef struct {
    serial_bridge_channel_cfg_t cfg;
    volatile bool   stop;                    /* 请求任务彻底退出（通道被禁用） */
    volatile bool   reconf;                  /* 请求原地重配 UART（热应用） */
    volatile bool   running;                 /* 任务已启动 */
    TaskHandle_t    task;
    serial_bridge_channel_cfg_t applied;     /* task 私有：当前已生效配置（重配比较用） */
    volatile uint32_t c_tcp_rx;              /* TCP 收到字节 */
    volatile uint32_t c_uart_tx;             /* 写入 UART 字节 */
    volatile uint32_t c_uart_rx;             /* UART 读回字节 */
    volatile uint32_t c_tcp_tx;              /* 发出 TCP 字节 */
} channel_rt_t;

static channel_rt_t s_ch[SERIAL_BRIDGE_CHANNEL_NUM];

static const serial_bridge_cfgs_t s_defaults = {
    .ch = {
        { .enable = true,  .uart_num = 1, .tx_gpio = 4,  .rx_gpio = 5,  .baud = 115200, .port = 8888 },
        { .enable = false, .uart_num = 2, .tx_gpio = 6,  .rx_gpio = 7,  .baud = 115200, .port = 8889 },
    }
};

/* ------------------------------------------------------------------ */
/* 引脚合法性：排除被占用/专用引脚                                  */
static bool gpio_is_reserved(int gpio)
{
    /* 38/39 = SWD, 43/44 = UART0 控制台, 19/20 = USB, 26..32 Flash, 33..37 PSRAM */
    if (gpio < 0 || gpio > 48) return true;
    if (gpio == 38 || gpio == 39 || gpio == 43 || gpio == 44) return true;
    if (gpio == 19 || gpio == 20) return true;
    if (gpio >= 26 && gpio <= 37) return true;
    return false;
}

static esp_err_t channel_validate(const serial_bridge_channel_cfg_t *c)
{
    if (c->uart_num != 1 && c->uart_num != 2) return ESP_ERR_INVALID_ARG;
    if (c->baud < 1200 || c->baud > 3000000)  return ESP_ERR_INVALID_ARG;
    if (c->port < 1024 || c->port > 65535)    return ESP_ERR_INVALID_ARG;
    if (gpio_is_reserved(c->tx_gpio) || gpio_is_reserved(c->rx_gpio)) return ESP_ERR_INVALID_ARG;
    if (c->tx_gpio == c->rx_gpio)             return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t cfgs_validate(const serial_bridge_cfgs_t *cfgs)
{
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        if (!cfgs->ch[i].enable) continue;
        /* uart_num 是通道身份（ch[i] 固定 = UART i+1），配置不可改变它——
         * 变了会破坏"原地重配只动参数/引脚"的前提（驱动装在哪个 UART 号上） */
        if (cfgs->ch[i].uart_num != i + 1) return ESP_ERR_INVALID_ARG;
        for (int j = i + 1; j < SERIAL_BRIDGE_CHANNEL_NUM; j++) {
            if (!cfgs->ch[j].enable) continue;
            if (cfgs->ch[i].uart_num == cfgs->ch[j].uart_num) return ESP_ERR_INVALID_ARG;
            if (cfgs->ch[i].port == cfgs->ch[j].port)         return ESP_ERR_INVALID_ARG;
            /* 跨通道也不允许共享/交叉使用同一 GPIO */
            if (cfgs->ch[i].tx_gpio == cfgs->ch[j].tx_gpio ||
                cfgs->ch[i].tx_gpio == cfgs->ch[j].rx_gpio ||
                cfgs->ch[i].rx_gpio == cfgs->ch[j].tx_gpio ||
                cfgs->ch[i].rx_gpio == cfgs->ch[j].rx_gpio) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        esp_err_t e = channel_validate(&cfgs->ch[i]);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}

/* 应用参数与引脚（初次安装与原地重配共用；含清空 RX 缓冲，防切换后
 * 把旧引脚收的残留数据当新数据转发） */
static esp_err_t uart_apply_pins(channel_rt_t *rt)
{
    serial_bridge_channel_cfg_t *c = &rt->cfg;
    int uart = c->uart_num == 1 ? UART_NUM_1 : UART_NUM_2;

    const uart_config_t uart_cfg = {
        .baud_rate  = c->baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(uart, &uart_cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(uart, c->tx_gpio, c->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    /* RX 内部上拉：空闲电平钳高，防止引脚浮空把噪声当数据收 */
    gpio_set_pull_mode(c->rx_gpio, GPIO_PULLUP_ONLY);
    uart_flush_input(uart);                  /* 丢弃 RX 缓冲里旧引脚收的残留 */
    return ESP_OK;
}

/* 初次启动：安装 UART 驱动（每个通道仅一次）。热应用不再卸载/重装驱动——
 * 反复 delete/install 会损坏驱动内部状态，表现为收发全失效且需重启恢复 */
static esp_err_t uart_setup(channel_rt_t *rt)
{
    int uart = rt->cfg.uart_num == 1 ? UART_NUM_1 : UART_NUM_2;
    esp_err_t err = uart_driver_install(uart, RX_BUF_SZ, TX_BUF_SZ, 0, NULL, 0);
    if (err != ESP_OK) return err;
    return uart_apply_pins(rt);
}

static int uart_num_of(channel_rt_t *rt)
{
    return rt->cfg.uart_num == 1 ? UART_NUM_1 : UART_NUM_2;
}

/* ------------------------------------------------------------------ */
/* 优雅关闭客户端：先排空待读数据、发 FIN 再 close，
 * 避免带未读数据 close() 触发 lwIP 发 RST（客户端报 10054） */
static void ch_close_client(int *cli)
{
    if (*cli < 0) return;
    char tmp[256];
    while (recv(*cli, tmp, sizeof(tmp), MSG_DONTWAIT) > 0) { }
    shutdown(*cli, SHUT_WR);
    close(*cli);
    *cli = -1;
}

static void channel_task(void *arg)
{
    channel_rt_t *rt = (channel_rt_t *)arg;
    serial_bridge_channel_cfg_t *c = &rt->cfg;
    rt->applied = *c;                        /* 首次生效配置 = 启动配置 */

    int srv = -1, cli = -1;
    int last_log_s = 0;

    ESP_LOGI(TAG, "ch%d: UART%d(GPIO%d/%d) baud=%d -> tcp:%d",
             rt - s_ch + 1, c->uart_num, c->tx_gpio, c->rx_gpio, c->baud, c->port);

    if (uart_setup(rt) != ESP_OK) {
        ESP_LOGE(TAG, "ch%d: uart setup failed", rt - s_ch + 1);
        rt->running = false;
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* 热应用：原地更新参数/引脚，不卸载 UART 驱动（驱动只在任务启动时
         * install 一次）。TCP 监听与已建立连接全程保持。 */
        if (rt->reconf) {
            rt->reconf = false;
            const serial_bridge_channel_cfg_t *a = &rt->applied;
            /* 释放被换掉的旧引脚（恢复默认，清残留上拉/信号路由） */
            if (c->tx_gpio != a->tx_gpio) gpio_reset_pin(a->tx_gpio);
            if (c->rx_gpio != a->rx_gpio) gpio_reset_pin(a->rx_gpio);
            if (uart_apply_pins(rt) == ESP_OK) {
                ESP_LOGI(TAG, "ch%d: uart reconfigured to UART%d(GPIO%d/%d) baud=%d",
                         rt - s_ch + 1, c->uart_num, c->tx_gpio, c->rx_gpio, c->baud);
                /* 端口变更：当前客户端无法跟随，断开并重建监听（下次连新端口） */
                if (c->port != a->port) {
                    ch_close_client(&cli);
                    if (srv >= 0) { close(srv); srv = -1; }
                    ESP_LOGI(TAG, "ch%d: port changed %d -> %d", rt - s_ch + 1, a->port, c->port);
                }
                rt->applied = *c;
            } else {
                ESP_LOGE(TAG, "ch%d: uart reconfig failed", rt - s_ch + 1);
            }
        }

        if (rt->stop) break;

        if (srv < 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(c->port);
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            srv = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if (srv >= 0 && (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0
                          || listen(srv, 1) != 0)) {
                close(srv);
                srv = -1;
                ESP_LOGE(TAG, "ch%d: bind/listen :%d failed, retry in 2s", rt - s_ch + 1, c->port);
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        /* 等连接或数据（带 50ms 超时，保证能及时响应 stop/reconf） */
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (srv >= 0) { FD_SET(srv, &rfds); maxfd = srv; }
        if (cli >= 0) { FD_SET(cli, &rfds); maxfd = cli > maxfd ? cli : maxfd; }

        struct timeval tv = { .tv_sec = 0, .tv_usec = SELECT_TIMEOUT_MS * 1000 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            ESP_LOGW(TAG, "ch%d: select err (errno=%d)", rt - s_ch + 1, errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (cli < 0 && srv >= 0 && FD_ISSET(srv, &rfds)) {
            cli = accept(srv, NULL, NULL);
            if (cli < 0) {
                ESP_LOGW(TAG, "ch%d: accept failed (errno=%d)", rt - s_ch + 1, errno);
            } else {
                /* 不给 cli 设 SO_RCVTIMEO：select 已保证"可读才 recv"，
                 * recv 不会阻塞；lwIP 上 socket 接收超时与 select 组合可能
                 * 产生误判路径，导致空闲连接被误关（客户端莫名收到 FIN）。
                 * SNDTIMEO 兜底防 send 永久卡死，但 20ms 太短——回环大流量
                 * 下客户端"先发后收"会让 TCP 窗口短暂为 0，send 频繁被
                 * 20ms 误杀断连。2s 只在真死连接时才触发 */
                struct timeval so = { .tv_sec = 2, .tv_usec = 0 };
                setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &so, sizeof(so));
                ESP_LOGI(TAG, "ch%d: client connected (fd=%d)", rt - s_ch + 1, cli);
            }
        }

        if (cli >= 0 && FD_ISSET(cli, &rfds)) {
            char buf[512];
            int r = recv(cli, buf, sizeof(buf), 0);
            if (r > 0) {
                rt->c_tcp_rx += r;
                uart_write_bytes(uart_num_of(rt), buf, r);
                rt->c_uart_tx += r;
            } else {
                if (r < 0 && errno == EAGAIN) continue;   /* 超时非断开 */
                /* r==0 是客户端正常断开（FIN），属日常事件；r<0 才是异常 */
                if (r < 0) {
                    ESP_LOGW(TAG, "ch%d: recv closed (r=%d errno=%d)", rt - s_ch + 1, r, errno);
                } else {
                    ESP_LOGI(TAG, "ch%d: client disconnected", rt - s_ch + 1);
                }
                ch_close_client(&cli);
            }
        }

        /* UART -> TCP */
        if (cli >= 0) {
            char buf[512];
            int r;
            while ((r = uart_read_bytes(uart_num_of(rt), buf, sizeof(buf), 0)) > 0) {
                rt->c_uart_rx += r;
                int w = send(cli, buf, r, 0);
                if (w <= 0) { ESP_LOGW(TAG, "ch%d: send fail (w=%d errno=%d)", rt - s_ch + 1, w, errno); ch_close_client(&cli); break; }
                rt->c_tcp_tx += r;
            }
        }

        /* 逐层计数器仅在 DEBUG 级别打印（定位字节卡在哪一层用，默认关闭） */
        if (cli >= 0) {
            int now_s = (int)(xTaskGetTickCount() / configTICK_RATE_HZ);
            if (now_s - last_log_s >= 5) {
                last_log_s = now_s;
                ESP_LOGD(TAG, "ch%d: [tcp_rx=%u uart_tx=%u uart_rx=%u tcp_tx=%u]",
                         rt - s_ch + 1,
                         (unsigned)rt->c_tcp_rx, (unsigned)rt->c_uart_tx,
                         (unsigned)rt->c_uart_rx, (unsigned)rt->c_tcp_tx);
            }
        }
    }

    ch_close_client(&cli);          /* 优雅关闭，不 RST */
    if (srv >= 0) close(srv);
    uart_driver_delete(uart_num_of(rt));
    rt->running = false;
    ESP_LOGI(TAG, "ch%d: task stopped", rt - s_ch + 1);
    vTaskDelete(NULL);
}

/* 停止（仅禁用通道时）：置位并等任务自清理，最多 600ms */
static void channel_stop(channel_rt_t *rt)
{
    if (!rt->running || rt->task == NULL) return;
    rt->stop = true;
    int wait = 0;
    while (rt->running && wait < 600) { vTaskDelay(pdMS_TO_TICKS(10)); wait += 10; }
    rt->stop = false;
    rt->task = NULL;
}

static void channel_start(channel_rt_t *rt)
{
    if (!rt->cfg.enable) return;
    rt->stop = false;
    rt->reconf = false;
    rt->running = true;
    BaseType_t ok = xTaskCreatePinnedToCore(channel_task, "ser_bridge", TASK_STACK,
                                            rt, TASK_PRIO, &rt->task, TASK_CORE);
    if (ok != pdPASS) { rt->running = false; ESP_LOGE(TAG, "ch: task create failed"); }
}

static void cfgs_load(void)
{
    /* 配置的唯一来源是 app_config（NVS 里那份 JSON）。
     * app_config_load() 已在 app_main 里、本函数之前跑过, 这里直接取内存镜像。 */
    memcpy(&s_cfgs, &app_config_get()->serial, sizeof(s_cfgs));
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        s_ch[i].cfg = s_cfgs.ch[i];
    }
}

/* ------------------------------------------------------------------ */
esp_err_t serial_bridge_start(void)
{
    cfgs_load();
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        if (s_cfgs.ch[i].enable) channel_start(&s_ch[i]);
    }
    return ESP_OK;
}

esp_err_t serial_bridge_get_configs(serial_bridge_cfgs_t *out)
{
    memcpy(out, &s_cfgs, sizeof(s_cfgs));
    return ESP_OK;
}

void serial_bridge_default_configs(serial_bridge_cfgs_t *out)
{
    if (!out) {
        return;
    }
    memcpy(out, &s_defaults, sizeof(*out));
}

esp_err_t serial_bridge_validate(const serial_bridge_cfgs_t *cfgs)
{
    if (!cfgs) {
        return ESP_ERR_INVALID_ARG;
    }
    return cfgs_validate(cfgs);
}

esp_err_t serial_bridge_set_configs(const serial_bridge_cfgs_t *cfg)
{
    esp_err_t e = cfgs_validate(cfg);
    if (e != ESP_OK) return e;

    /* 写回 app_config（整份 JSON）而不是原来的独立二进制 blob:
     * 串口桥不再是独立的真相源, 导出 config 时能一并带出去。 */
    memcpy(&app_config_mutable()->serial, cfg, sizeof(*cfg));
    if (app_config_save() != ESP_OK) {
        ESP_LOGE(TAG, "config save failed");
        return ESP_FAIL;
    }

    memcpy(&s_cfgs, cfg, sizeof(s_cfgs));
    for (int i = 0; i < SERIAL_BRIDGE_CHANNEL_NUM; i++) {
        const serial_bridge_channel_cfg_t *nc = &cfg->ch[i];
        channel_rt_t *rt = &s_ch[i];

        bool need = nc->enable;
        bool same = (rt->cfg.enable == nc->enable && rt->cfg.uart_num == nc->uart_num &&
                     rt->cfg.tx_gpio == nc->tx_gpio && rt->cfg.rx_gpio == nc->rx_gpio &&
                     rt->cfg.baud == nc->baud && rt->cfg.port == nc->port);

        if (rt->running && !need) {
            channel_stop(rt);                      /* 禁用：停任务（优雅关客户端） */
        } else if (!rt->running && need) {
            rt->cfg = *nc;                         /* 启用：起新任务 */
            channel_start(rt);
        } else if (rt->running && !same) {
            rt->cfg = *nc;                         /* 热应用：原地重配 UART，TCP 不断 */
            rt->reconf = true;
        }
    }
    return ESP_OK;
}
