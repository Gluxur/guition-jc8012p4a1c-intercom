# WHOLE_ARCHIVE est OBLIGATOIRE : les TEST_CASE s'enregistrent via des
# constructeurs statiques qu'aucun symbole ne référence. Sans ce drapeau
# l'éditeur de liens jette les objets et le menu Unity affiche zéro test.
idf_component_register(
    SRCS "test_app_main.cpp"
         "test_helpers.cpp"
         "test_menu.cpp"
         "test_jpeg_hw.cpp"
         "test_bsp_video.cpp"
         "test_bsp_audio.cpp"
         "test_sd_card_only.cpp"
    INCLUDE_DIRS "."
    REQUIRES unity bsp esp_driver_jpeg esp_mm nvs_flash esp_timer
    EMBED_FILES "assets/quad_320x240.jpg"
                "assets/quad_200x120.jpg"
    WHOLE_ARCHIVE
)
