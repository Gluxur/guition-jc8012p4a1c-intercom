#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JD9365 vendor configuration — passed via esp_lcd_panel_dev_config_t.vendor_config
 */
typedef struct {
    struct {
        esp_lcd_dsi_bus_handle_t         dsi_bus;    /*!< MIPI-DSI bus handle */
        const esp_lcd_dpi_panel_config_t *dpi_config; /*!< DPI panel config (timing, format…) */
    } mipi_config;
} jd9365_vendor_config_t;

/**
 * @brief Create an LCD panel handle for the JD9365 MIPI-DSI controller.
 *
 * The returned handle wraps an esp_lcd DPI panel: draw_bitmap works via
 * the DPI engine; reset/init/mirror send DCS commands over the DBI IO.
 *
 * @param io           DBI panel-IO handle (from esp_lcd_new_panel_io_dbi)
 * @param panel_dev_config  Device config — reset_gpio_num, bits_per_pixel,
 *                          rgb_ele_order, vendor_config (jd9365_vendor_config_t)
 * @param ret_panel    Output panel handle
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_new_panel_jd9365(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
