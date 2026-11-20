/*
 * JD9365 MIPI-DSI LCD panel driver for ESP32-P4.
 *
 * Init sequence and pin constants from:
 *   sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6 (src/lcd/esp_lcd_jd9365.c)
 *
 * Driver structure follows the esp-bsp esp_lcd_ili9881c pattern:
 *   - esp_lcd_new_panel_dpi() creates the DPI panel (handles draw_bitmap)
 *   - we override init/reset/mirror/del with JD9365-specific DCS commands
 */

#include <stdlib.h>
#include <string.h>
#include "esp_lcd_jd9365.h"
#include "esp_lcd_panel_interface.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "jd9365";

/* -----------------------------------------------------------------------
 * Init command table
 * --------------------------------------------------------------------- */

typedef struct {
    uint8_t        cmd;
    const uint8_t *data;
    uint8_t        data_bytes;
    uint32_t       delay_ms;
} jd9365_lcd_init_cmd_t;

/* Full init sequence — sourced verbatim from Guition JC8012P4A1C demo */
static const jd9365_lcd_init_cmd_t vendor_specific_init[] = {
    /* Page 0 — unlock */
    {0xE0, (const uint8_t[]){0x00}, 1, 0},
    {0xE1, (const uint8_t[]){0x93}, 1, 0},
    {0xE2, (const uint8_t[]){0x65}, 1, 0},
    {0xE3, (const uint8_t[]){0xF8}, 1, 0},
    {0x80, (const uint8_t[]){0x01}, 1, 0},  /* 2 DSI lanes */

    /* Page 1 — timing / power */
    {0xE0, (const uint8_t[]){0x01}, 1, 0},
    {0x00, (const uint8_t[]){0x00}, 1, 0},
    {0x01, (const uint8_t[]){0x39}, 1, 0},
    {0x03, (const uint8_t[]){0x10}, 1, 0},
    {0x04, (const uint8_t[]){0x41}, 1, 0},
    {0x0C, (const uint8_t[]){0x74}, 1, 0},
    {0x17, (const uint8_t[]){0x00}, 1, 0},
    {0x18, (const uint8_t[]){0xD7}, 1, 0},
    {0x19, (const uint8_t[]){0x00}, 1, 0},
    {0x1A, (const uint8_t[]){0x00}, 1, 0},
    {0x1B, (const uint8_t[]){0xD7}, 1, 0},
    {0x1C, (const uint8_t[]){0x00}, 1, 0},
    {0x24, (const uint8_t[]){0xFE}, 1, 0},
    {0x35, (const uint8_t[]){0x26}, 1, 0},
    {0x37, (const uint8_t[]){0x69}, 1, 0},
    {0x38, (const uint8_t[]){0x05}, 1, 0},
    {0x39, (const uint8_t[]){0x06}, 1, 0},
    {0x3A, (const uint8_t[]){0x08}, 1, 0},
    {0x3C, (const uint8_t[]){0x78}, 1, 0},
    {0x3D, (const uint8_t[]){0xFF}, 1, 0},
    {0x3E, (const uint8_t[]){0xFF}, 1, 0},
    {0x3F, (const uint8_t[]){0xFF}, 1, 0},
    {0x40, (const uint8_t[]){0x06}, 1, 0},
    {0x41, (const uint8_t[]){0xA0}, 1, 0},
    {0x43, (const uint8_t[]){0x14}, 1, 0},
    {0x44, (const uint8_t[]){0x0B}, 1, 0},
    {0x45, (const uint8_t[]){0x30}, 1, 0},
    {0x4B, (const uint8_t[]){0x04}, 1, 0},
    {0x55, (const uint8_t[]){0x02}, 1, 0},
    {0x57, (const uint8_t[]){0x89}, 1, 0},
    {0x59, (const uint8_t[]){0x0A}, 1, 0},
    {0x5A, (const uint8_t[]){0x28}, 1, 0},
    {0x5B, (const uint8_t[]){0x15}, 1, 0},
    /* Gamma positive */
    {0x5D, (const uint8_t[]){0x50}, 1, 0},
    {0x5E, (const uint8_t[]){0x37}, 1, 0},
    {0x5F, (const uint8_t[]){0x29}, 1, 0},
    {0x60, (const uint8_t[]){0x1E}, 1, 0},
    {0x61, (const uint8_t[]){0x1D}, 1, 0},
    {0x62, (const uint8_t[]){0x12}, 1, 0},
    {0x63, (const uint8_t[]){0x1A}, 1, 0},
    {0x64, (const uint8_t[]){0x08}, 1, 0},
    {0x65, (const uint8_t[]){0x25}, 1, 0},
    {0x66, (const uint8_t[]){0x26}, 1, 0},
    {0x67, (const uint8_t[]){0x28}, 1, 0},
    {0x68, (const uint8_t[]){0x49}, 1, 0},
    {0x69, (const uint8_t[]){0x3A}, 1, 0},
    {0x6A, (const uint8_t[]){0x43}, 1, 0},
    {0x6B, (const uint8_t[]){0x3A}, 1, 0},
    {0x6C, (const uint8_t[]){0x3B}, 1, 0},
    {0x6D, (const uint8_t[]){0x32}, 1, 0},
    {0x6E, (const uint8_t[]){0x1F}, 1, 0},
    {0x6F, (const uint8_t[]){0x0E}, 1, 0},
    /* Gamma negative */
    {0x70, (const uint8_t[]){0x50}, 1, 0},
    {0x71, (const uint8_t[]){0x37}, 1, 0},
    {0x72, (const uint8_t[]){0x29}, 1, 0},
    {0x73, (const uint8_t[]){0x1E}, 1, 0},
    {0x74, (const uint8_t[]){0x1D}, 1, 0},
    {0x75, (const uint8_t[]){0x12}, 1, 0},
    {0x76, (const uint8_t[]){0x1A}, 1, 0},
    {0x77, (const uint8_t[]){0x08}, 1, 0},
    {0x78, (const uint8_t[]){0x25}, 1, 0},
    {0x79, (const uint8_t[]){0x26}, 1, 0},
    {0x7A, (const uint8_t[]){0x28}, 1, 0},
    {0x7B, (const uint8_t[]){0x49}, 1, 0},
    {0x7C, (const uint8_t[]){0x3A}, 1, 0},
    {0x7D, (const uint8_t[]){0x43}, 1, 0},
    {0x7E, (const uint8_t[]){0x3A}, 1, 0},
    {0x7F, (const uint8_t[]){0x3B}, 1, 0},
    {0x80, (const uint8_t[]){0x32}, 1, 0},
    {0x81, (const uint8_t[]){0x1F}, 1, 0},
    {0x82, (const uint8_t[]){0x0E}, 1, 0},

    /* Page 2 — GIP / mux routing */
    {0xE0, (const uint8_t[]){0x02}, 1, 0},
    {0x00, (const uint8_t[]){0x1F}, 1, 0},
    {0x01, (const uint8_t[]){0x1F}, 1, 0},
    {0x02, (const uint8_t[]){0x52}, 1, 0},
    {0x03, (const uint8_t[]){0x51}, 1, 0},
    {0x04, (const uint8_t[]){0x50}, 1, 0},
    {0x05, (const uint8_t[]){0x4B}, 1, 0},
    {0x06, (const uint8_t[]){0x4A}, 1, 0},
    {0x07, (const uint8_t[]){0x49}, 1, 0},
    {0x08, (const uint8_t[]){0x48}, 1, 0},
    {0x09, (const uint8_t[]){0x47}, 1, 0},
    {0x0A, (const uint8_t[]){0x46}, 1, 0},
    {0x0B, (const uint8_t[]){0x45}, 1, 0},
    {0x0C, (const uint8_t[]){0x44}, 1, 0},
    {0x0D, (const uint8_t[]){0x40}, 1, 0},
    {0x0E, (const uint8_t[]){0x41}, 1, 0},
    {0x0F, (const uint8_t[]){0x1F}, 1, 0},
    {0x10, (const uint8_t[]){0x1F}, 1, 0},
    {0x11, (const uint8_t[]){0x1F}, 1, 0},
    {0x12, (const uint8_t[]){0x1F}, 1, 0},
    {0x13, (const uint8_t[]){0x1F}, 1, 0},
    {0x14, (const uint8_t[]){0x1F}, 1, 0},
    {0x15, (const uint8_t[]){0x1F}, 1, 0},
    {0x16, (const uint8_t[]){0x1F}, 1, 0},
    {0x17, (const uint8_t[]){0x1F}, 1, 0},
    {0x18, (const uint8_t[]){0x52}, 1, 0},
    {0x19, (const uint8_t[]){0x51}, 1, 0},
    {0x1A, (const uint8_t[]){0x50}, 1, 0},
    {0x1B, (const uint8_t[]){0x4B}, 1, 0},
    {0x1C, (const uint8_t[]){0x4A}, 1, 0},
    {0x1D, (const uint8_t[]){0x49}, 1, 0},
    {0x1E, (const uint8_t[]){0x48}, 1, 0},
    {0x1F, (const uint8_t[]){0x47}, 1, 0},
    {0x20, (const uint8_t[]){0x46}, 1, 0},
    {0x21, (const uint8_t[]){0x45}, 1, 0},
    {0x22, (const uint8_t[]){0x44}, 1, 0},
    {0x23, (const uint8_t[]){0x40}, 1, 0},
    {0x24, (const uint8_t[]){0x41}, 1, 0},
    {0x25, (const uint8_t[]){0x1F}, 1, 0},
    {0x26, (const uint8_t[]){0x1F}, 1, 0},
    {0x27, (const uint8_t[]){0x1F}, 1, 0},
    {0x28, (const uint8_t[]){0x1F}, 1, 0},
    {0x29, (const uint8_t[]){0x1F}, 1, 0},
    {0x2A, (const uint8_t[]){0x1F}, 1, 0},
    {0x2B, (const uint8_t[]){0x1F}, 1, 0},
    {0x2C, (const uint8_t[]){0x1F}, 1, 0},
    {0x2D, (const uint8_t[]){0x1F}, 1, 0},
    {0x2E, (const uint8_t[]){0x52}, 1, 0},
    {0x2F, (const uint8_t[]){0x40}, 1, 0},
    {0x30, (const uint8_t[]){0x41}, 1, 0},
    {0x31, (const uint8_t[]){0x48}, 1, 0},
    {0x32, (const uint8_t[]){0x49}, 1, 0},
    {0x33, (const uint8_t[]){0x4A}, 1, 0},
    {0x34, (const uint8_t[]){0x4B}, 1, 0},
    {0x35, (const uint8_t[]){0x44}, 1, 0},
    {0x36, (const uint8_t[]){0x45}, 1, 0},
    {0x37, (const uint8_t[]){0x46}, 1, 0},
    {0x38, (const uint8_t[]){0x47}, 1, 0},
    {0x39, (const uint8_t[]){0x51}, 1, 0},
    {0x3A, (const uint8_t[]){0x50}, 1, 0},
    {0x3B, (const uint8_t[]){0x1F}, 1, 0},
    {0x3C, (const uint8_t[]){0x1F}, 1, 0},
    {0x3D, (const uint8_t[]){0x1F}, 1, 0},
    {0x3E, (const uint8_t[]){0x1F}, 1, 0},
    {0x3F, (const uint8_t[]){0x1F}, 1, 0},
    {0x40, (const uint8_t[]){0x1F}, 1, 0},
    {0x41, (const uint8_t[]){0x1F}, 1, 0},
    {0x42, (const uint8_t[]){0x1F}, 1, 0},
    {0x43, (const uint8_t[]){0x1F}, 1, 0},
    {0x44, (const uint8_t[]){0x52}, 1, 0},
    {0x45, (const uint8_t[]){0x40}, 1, 0},
    {0x46, (const uint8_t[]){0x41}, 1, 0},
    {0x47, (const uint8_t[]){0x48}, 1, 0},
    {0x48, (const uint8_t[]){0x49}, 1, 0},
    {0x49, (const uint8_t[]){0x4A}, 1, 0},
    {0x4A, (const uint8_t[]){0x4B}, 1, 0},
    {0x4B, (const uint8_t[]){0x44}, 1, 0},
    {0x4C, (const uint8_t[]){0x45}, 1, 0},
    {0x4D, (const uint8_t[]){0x46}, 1, 0},
    {0x4E, (const uint8_t[]){0x47}, 1, 0},
    {0x4F, (const uint8_t[]){0x51}, 1, 0},
    {0x50, (const uint8_t[]){0x50}, 1, 0},
    {0x51, (const uint8_t[]){0x1F}, 1, 0},
    {0x52, (const uint8_t[]){0x1F}, 1, 0},
    {0x53, (const uint8_t[]){0x1F}, 1, 0},
    {0x54, (const uint8_t[]){0x1F}, 1, 0},
    {0x55, (const uint8_t[]){0x1F}, 1, 0},
    {0x56, (const uint8_t[]){0x1F}, 1, 0},
    {0x57, (const uint8_t[]){0x1F}, 1, 0},
    {0x58, (const uint8_t[]){0x40}, 1, 0},
    {0x59, (const uint8_t[]){0x00}, 1, 0},
    {0x5A, (const uint8_t[]){0x00}, 1, 0},
    {0x5B, (const uint8_t[]){0x10}, 1, 0},
    {0x5C, (const uint8_t[]){0x05}, 1, 0},
    {0x5D, (const uint8_t[]){0x50}, 1, 0},
    {0x5E, (const uint8_t[]){0x01}, 1, 0},
    {0x5F, (const uint8_t[]){0x02}, 1, 0},
    {0x60, (const uint8_t[]){0x50}, 1, 0},
    {0x61, (const uint8_t[]){0x06}, 1, 0},
    {0x62, (const uint8_t[]){0x04}, 1, 0},
    {0x63, (const uint8_t[]){0x03}, 1, 0},
    {0x64, (const uint8_t[]){0x64}, 1, 0},
    {0x65, (const uint8_t[]){0x65}, 1, 0},
    {0x66, (const uint8_t[]){0x0B}, 1, 0},
    {0x67, (const uint8_t[]){0x73}, 1, 0},
    {0x68, (const uint8_t[]){0x07}, 1, 0},
    {0x69, (const uint8_t[]){0x06}, 1, 0},
    {0x6A, (const uint8_t[]){0x64}, 1, 0},
    {0x6B, (const uint8_t[]){0x08}, 1, 0},
    {0x6C, (const uint8_t[]){0x00}, 1, 0},
    {0x6D, (const uint8_t[]){0x32}, 1, 0},
    {0x6E, (const uint8_t[]){0x08}, 1, 0},

    /* Page 4 */
    {0xE0, (const uint8_t[]){0x04}, 1, 0},
    {0x2C, (const uint8_t[]){0x6B}, 1, 0},
    {0x35, (const uint8_t[]){0x08}, 1, 0},
    {0x37, (const uint8_t[]){0x00}, 1, 0},

    /* Page 0 — sleep-out, display-on */
    {0xE0, (const uint8_t[]){0x00}, 1,   0},
    {0x11, (const uint8_t[]){0x00}, 1, 120},  /* Sleep Out, 120 ms */
    {0x29, (const uint8_t[]){0x00}, 1,   5},  /* Display On,   5 ms */
    {0x35, (const uint8_t[]){0x00}, 1,   0},  /* Tearing Effect On */
};

/* -----------------------------------------------------------------------
 * Private panel data
 * --------------------------------------------------------------------- */

typedef struct {
    esp_lcd_panel_io_handle_t io;
    int                       reset_gpio_num;
    /* saved DPI vtable functions (called after our wrappers) */
    esp_err_t (*dpi_del)(esp_lcd_panel_t *panel);
    esp_err_t (*dpi_init)(esp_lcd_panel_t *panel);
} jd9365_panel_t;

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */

static esp_err_t panel_jd9365_del(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9365_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9365_init(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9365_mirror(esp_lcd_panel_t *panel,
                                     bool mirror_x, bool mirror_y);
static esp_err_t panel_jd9365_invert_color(esp_lcd_panel_t *panel,
                                           bool invert_color_data);
static esp_err_t panel_jd9365_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

/* -----------------------------------------------------------------------
 * Public constructor
 * --------------------------------------------------------------------- */

esp_err_t esp_lcd_new_panel_jd9365(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel,
                        ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    const jd9365_vendor_config_t *vendor =
        (const jd9365_vendor_config_t *)panel_dev_config->vendor_config;
    ESP_RETURN_ON_FALSE(vendor, ESP_ERR_INVALID_ARG, TAG, "vendor_config is NULL");

    esp_err_t ret = ESP_OK;
    jd9365_panel_t *jd = calloc(1, sizeof(jd9365_panel_t));
    ESP_RETURN_ON_FALSE(jd, ESP_ERR_NO_MEM, TAG, "no mem for jd9365_panel_t");

    jd->io             = io;
    jd->reset_gpio_num = panel_dev_config->reset_gpio_num;

    /* Configure reset GPIO if provided */
    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode         = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "gpio_config failed");
    }

    /* Create the underlying DPI panel — this is what drives draw_bitmap */
    ESP_GOTO_ON_ERROR(
        esp_lcd_new_panel_dpi(vendor->mipi_config.dsi_bus,
                              vendor->mipi_config.dpi_config,
                              ret_panel),
        err, TAG, "esp_lcd_new_panel_dpi failed");

    /* Save original DPI vtable entries we want to chain */
    jd->dpi_del  = (*ret_panel)->del;
    jd->dpi_init = (*ret_panel)->init;

    /* Override vtable with JD9365-specific handlers */
    (*ret_panel)->del          = panel_jd9365_del;
    (*ret_panel)->reset        = panel_jd9365_reset;
    (*ret_panel)->init         = panel_jd9365_init;
    (*ret_panel)->mirror       = panel_jd9365_mirror;
    (*ret_panel)->invert_color = panel_jd9365_invert_color;
    (*ret_panel)->disp_on_off  = panel_jd9365_disp_on_off;
    (*ret_panel)->user_data    = jd;

    ESP_LOGI(TAG, "JD9365 panel created, reset_gpio=%d", jd->reset_gpio_num);
    return ESP_OK;

err:
    free(jd);
    /* ESP_GOTO_ON_ERROR a déjà posé le code réel dans `ret` ; renvoyer
     * ESP_FAIL en dur l'écrasait et masquait la cause (ex. ESP_ERR_NO_MEM du
     * DPI, ESP_ERR_INVALID_ARG du gpio_config) à l'appelant. */
    return ret;
}

/* -----------------------------------------------------------------------
 * Panel vtable implementations
 * --------------------------------------------------------------------- */

static esp_err_t panel_jd9365_del(esp_lcd_panel_t *panel)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;
    if (jd->reset_gpio_num >= 0) {
        gpio_reset_pin(jd->reset_gpio_num);
    }
    esp_err_t ret = ESP_OK;
    if (jd->dpi_del) {
        ret = jd->dpi_del(panel);
    }
    free(jd);
    return ret;
}

static esp_err_t panel_jd9365_reset(esp_lcd_panel_t *panel)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;
    if (jd->reset_gpio_num >= 0) {
        gpio_set_level(jd->reset_gpio_num, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(jd->reset_gpio_num, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_jd9365_init(esp_lcd_panel_t *panel)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;

    /* Send JD9365 DCS init sequence */
    const size_t n = sizeof(vendor_specific_init) / sizeof(vendor_specific_init[0]);
    for (size_t i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(jd->io,
                                      vendor_specific_init[i].cmd,
                                      vendor_specific_init[i].data,
                                      vendor_specific_init[i].data_bytes),
            TAG, "send init cmd 0x%02X failed", vendor_specific_init[i].cmd);
        if (vendor_specific_init[i].delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(vendor_specific_init[i].delay_ms));
        }
    }

    /* Chain to DPI panel init (enables DPI timing engine) */
    if (jd->dpi_init) {
        ESP_RETURN_ON_ERROR(jd->dpi_init(panel), TAG, "dpi_init failed");
    }

    ESP_LOGI(TAG, "JD9365 init sequence done");
    return ESP_OK;
}

static esp_err_t panel_jd9365_mirror(esp_lcd_panel_t *panel,
                                     bool mirror_x, bool mirror_y)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;
    /* MADCTL: bit 6 = MY (mirror Y), bit 7 = MX (mirror X) */
    uint8_t madctl = 0;
    if (mirror_x) madctl |= (1 << 7);
    if (mirror_y) madctl |= (1 << 6);
    /* Select page 0 first */
    uint8_t page0 = 0x00;
    esp_lcd_panel_io_tx_param(jd->io, 0xE0, &page0, 1);
    esp_lcd_panel_io_tx_param(jd->io, 0x36, &madctl, 1);
    return ESP_OK;
}

static esp_err_t panel_jd9365_invert_color(esp_lcd_panel_t *panel,
                                            bool invert_color_data)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;
    uint8_t page0 = 0x00;
    esp_lcd_panel_io_tx_param(jd->io, 0xE0, &page0, 1);
    uint8_t cmd = invert_color_data ? 0x21 : 0x20;
    esp_lcd_panel_io_tx_param(jd->io, cmd, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_jd9365_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    jd9365_panel_t *jd = (jd9365_panel_t *)panel->user_data;
    uint8_t page0 = 0x00;
    esp_lcd_panel_io_tx_param(jd->io, 0xE0, &page0, 1);
    uint8_t cmd = on_off ? 0x29 : 0x28;
    esp_lcd_panel_io_tx_param(jd->io, cmd, NULL, 0);
    return ESP_OK;
}
