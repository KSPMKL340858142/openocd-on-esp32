#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"

#include "lvgl.h"

#include "ui.h"
#include "app_config.h"
#include "storage.h"
#include "types.h"

static const char *TAG = "ui-events";

void ui_event_qr_screen(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_GESTURE &&  lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_LEFT) {
        lv_scr_load_anim(g_ui_openocd_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
    }
}

void ui_event_info_screen(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_GESTURE &&  lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_LEFT) {
        lv_scr_load_anim(g_ui_openocd_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
    }
}

void ui_event_openocd_screen(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_GESTURE &&  lv_indev_get_gesture_dir(lv_indev_get_act()) == LV_DIR_RIGHT) {
        if (g_app_params.mode == APP_MODE_STA) {
            lv_scr_load_anim(g_ui_info_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        } else {
            lv_scr_load_anim(g_ui_qr_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        }
    }
}

void ui_event_reset_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        storage_erase_all();
        esp_restart();
    }
}

void ui_event_run_button(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code == LV_EVENT_CLICKED) {
        /* 所有改动写进统一配置（一份 JSON），不再各写各的 NVS 键 */
        app_config_t *cfg = app_config_mutable();

        /* save selected target */
        uint16_t selected_index = lv_dropdown_get_selected(g_ui_target_dropdown);
        ESP_LOGI(TAG, "save selected target: %s", g_app_params.target_list[selected_index]);
        snprintf(cfg->target, sizeof(cfg->target), "%s", g_app_params.target_list[selected_index]);

        /* save RTOS type */
        selected_index = lv_dropdown_get_selected(g_ui_rtos_dropdown);
        ESP_LOGI(TAG, "save selected rtos: %s", g_app_params.rtos_list[selected_index]);
        snprintf(cfg->rtos, sizeof(cfg->rtos), "%s", g_app_params.rtos_list[selected_index]);

        /* save Interface type */
        selected_index = lv_dropdown_get_selected(g_ui_interface_dropdown);
        ESP_LOGI(TAG, "save selected interface: %s", selected_index == 0 ? "jtag" : "swd");
        cfg->iface = (char)(selected_index + '0');

        /* save Flash Support */
        const char *flash_size = lv_obj_get_state(g_ui_flash_checkbox) & LV_STATE_CHECKED ? "auto" : "0";
        ESP_LOGI(TAG, "save selected flash support: %s", flash_size);
        snprintf(cfg->flash_size, sizeof(cfg->flash_size), "%s", flash_size);

        /* save Dual Core Support */
        const char *dual_core = lv_obj_get_state(g_ui_dual_core_checkbox) & LV_STATE_CHECKED ? "3" : "1";
        ESP_LOGI(TAG, "save selected dual core support: %s", dual_core);
        cfg->dual_core = dual_core[0];

        /* save debug_level */
        selected_index = lv_dropdown_get_selected(g_ui_debug_level_dropdown);
        cfg->debug_level = (char)(selected_index + '1');
        ESP_LOGI(TAG, "save selected debug_level: %c", cfg->debug_level);

        if (app_config_save() != ESP_OK) {
            ESP_LOGE(TAG, "save config failed");
        }
        esp_restart();
    }
}
