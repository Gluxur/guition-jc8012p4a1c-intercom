idf_component_register(
    SRCS "esp_lcd_touch_gsl3680.c" "gsl_point_id.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_lcd_touch esp_driver_gpio
)

# Code fournisseur GSL3680 : reliquats d'une fonctionnalité de gestes jamais
# câblée (tpc_gesture_id, pre_distance, distance_flag, zoomIn/OutDebounce,
# _Get_Cal_msg et ses locales). On les conserve comme référence vendeur plutôt
# que de les supprimer — mais on éteint les avertissements ici, sinon ils
# noient les warnings de notre propre code dans la sortie de build.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
)
