cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_p4_webrtc_cam)

# ThorVG (embarqué dans le composant managé lvgl__lvgl) compare un char signé
# à 0 dans b64Decode -> -Wtype-limits. Warning légitime, mais chez l'upstream :
# managed_components/ est régénéré depuis dependencies.lock et ne doit jamais
# être édité. Suppression ciblée sur ce seul composant, pour garder un build
# où nos propres avertissements restent visibles.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-type-limits)