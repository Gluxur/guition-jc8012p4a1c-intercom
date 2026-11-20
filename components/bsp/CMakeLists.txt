idf_component_register(
    SRCS "bsp.cpp" "bsp_camera.cpp" "ppa_srm_quirk.cpp" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_lcd_touch driver esp_driver_ledc esp_timer freertos lvgl esp_driver_i2c esp_driver_ppa esp_driver_i2s esp_codec_dev
    PRIV_REQUIRES esp_lcd_jd9365 esp_lcd_touch_gsl3680 esp_hw_support esp_driver_gpio nvs_flash fatfs sdmmc esp_driver_sdmmc esp_driver_cam esp_driver_isp
)

# lv_mem_psram.c n'est référencé que par la bibliothèque LVGL, liée APRÈS ce
# composant : sans -u, le linker ne tirerait jamais l'objet et LVGL resterait
# sans allocateur (symboles indéfinis) ou retomberait sur un autre.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
