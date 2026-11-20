# Third-party notices

The code written for this project is under the [MIT license](LICENSE). The
following bundled files come from elsewhere and keep their own terms.

| Path | Origin | License |
|---|---|---|
| `components/esp_lcd_touch_gsl3680/gsl_point_id.c`, `include/gsl_point_id.h` | Silead gslX680 touch reference code (2010-2016 Silead Inc.), via the Guition board demo | **GPL-2.0-or-later** |
| `components/esp_lcd_touch_gsl3680/` (other files, incl. the controller configuration table in `esp_lcd_touch_gsl3680.h`) | Guition JC8012P4A1C board demo, adapted to the `esp_lcd_touch` API | Vendor demo code, no explicit license |
| `components/esp_lcd_jd9365/` | Init sequence from the Guition JC8012P4A1C demo, via [sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6](https://github.com/sukesh-ak/JC8012P4A1-GUITION-ESP32-P4_ESP32-C6); structure follows Espressif's `esp_lcd_ili9881c` | See the upstream repository / Apache-2.0 (Espressif structure) |
| `components/bsp/ov02c10_init.h` | OV02C10 register table from the Guition SDK, via [espressif/esp-video-components#90](https://github.com/espressif/esp-video-components/pull/90) | Apache-2.0 (© 2024 Espressif Systems) |
| `main/images/weather_icons_lottie/*.json` | [meteocons](https://github.com/basmilius/meteocons) by Bas Milius (`@meteocons/lottie`) | MIT |
| `main/fonts/montserrat_fr_*.c` | Montserrat (Julieta Ulanovsky), converted with `lv_font_conv` from LVGL's built-in font sources | SIL Open Font License 1.1 |
| `main/fonts/icons_fa_56.c` | Font Awesome 5 Free glyphs, converted with `lv_font_conv` | Icons CC BY 4.0, font SIL OFL 1.1 |

## About the GPL file

`gsl_point_id.c` implements the GSL3680's finger-tracking algorithm and is
linked into the firmware. The source files in this repository are distributed
under their respective licenses, but **a firmware binary built from this
repository is a combined work covered by the GPL-2.0-or-later**: if you
distribute such a binary, you must also make the corresponding source
available under the GPL. Replacing this file with an independent
implementation would remove that constraint.

## Dependencies fetched at build time

Not stored in this repository, downloaded by the ESP-IDF component manager
(`main/idf_component.yml`, pinned in `dependencies.lock`): LVGL, `esp_peer`
(Espressif WebRTC), `esp_hosted` / `esp_wifi_remote`, `esp-sr`, `esp_lcd_touch`,
TinyUSB, cJSON, esp-mqtt. Each one comes with its own license.

The host unit tests fetch GoogleTest (BSD-3-Clause) and cJSON (MIT).
