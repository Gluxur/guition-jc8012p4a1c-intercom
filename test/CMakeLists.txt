cmake_minimum_required(VERSION 3.20)
# C en plus de CXX : cJSON est du C, compilé comme tel (cf. plus bas).
project(visiophone_host_tests C CXX)

# Projet de tests HOST — volontairement séparé du projet ESP-IDF de la racine.
# Il compile les sources applicatives PURES (sans dépendance matérielle) avec le
# compilateur du PC, pour une boucle de test en < 1 s au lieu des ~40 s d'un
# cycle build/flash/monitor sur la carte. Tout ce qui touche le silicium (PPA,
# JPEG HW, I2S, DSI) n'a rien à faire ici : cf. test_apps/ ESP-IDF pour ça.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../main")
# Le BSP est matériel, sauf ppa_srm_quirk : de l'arithmétique de géométrie
# extraite là pour être vérifiable ici plutôt que par cycles flash/monitor.
set(BSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../components/bsp")

# --- GoogleTest -------------------------------------------------------------
# Récupéré au configure (nécessite git + réseau la première fois seulement ;
# ensuite _deps/ est réutilisé).
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
# Nécessaire sous MSVC : gtest doit utiliser la même CRT dynamique que nos cibles.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# --- cJSON ------------------------------------------------------------------
# Les parsers météo/agenda en dépendent. Récupéré comme gtest plutôt que pointé
# dans l'installation ESP-IDF : un chemin en dur vers C:\esp\vX.Y.Z casserait à
# la première mise à jour d'IDF, et c'est la MÊME bibliothèque amont.
#
# SOURCE_SUBDIR vers un dossier inexistant = "télécharge mais n'ajoute pas le
# sous-projet" : le CMakeLists de cJSON déclare cmake_minimum_required(2.8.5),
# que CMake 4 refuse net. On compile donc son unique .c nous-mêmes — cible
# séparée, pour qu'elle échappe au force-include C++ appliqué à `tests`.
FetchContent_Declare(
  cjson
  GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
  GIT_TAG        v1.7.18
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake-build-disabled
)
FetchContent_MakeAvailable(cjson)

add_library(cjson_host STATIC "${cjson_SOURCE_DIR}/cJSON.c")
target_include_directories(cjson_host PUBLIC "${cjson_SOURCE_DIR}")

# --- mbedTLS / PSA Crypto ---------------------------------------------------
# net/noise_nnpsk0.cpp (ESPHome API encryption) calls PSA Crypto. ESP-IDF 6.0
# ships mbedTLS 4.0.0; the same release is fetched here, so the host test runs
# the firmware's exact crypto path. Only the libraries are built (~1 min, once).
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.0.0/mbedtls-4.0.0.tar.bz2
  URL_HASH SHA256=2f3a47f7b3a541ddef450e4867eeecb7ce2ef7776093f3a11d6d43ead6bf2827
)
FetchContent_MakeAvailable(mbedtls)

# --- Cible de test ----------------------------------------------------------
add_executable(tests
  test_call_log_parse.cpp
  test_ephemeris.cpp
  test_file_path.cpp
  test_g711.cpp
  test_ppa_srm_quirk.cpp
  test_ringtone.cpp
  test_wav_parse.cpp
  test_gcal_parse.cpp
  test_motion_detect.cpp
  test_premul_blend.cpp
  test_ota_image_check.cpp
  test_weather_parse.cpp
  test_esphome_frame.cpp
  test_esphome_pb.cpp
  test_noise_nnpsk0.cpp
  test_gate_logic.cpp
  test_ha_entity_id.cpp
  test_github_release.cpp
  test_i18n.cpp
  # Sources applicatives réelles, compilées pour le host. En ajouter une ici
  # n'est légitime que si elle ne tire aucun header de driver ESP-IDF (stubs/
  # ne couvre que le log et le compteur µs, cf. les commentaires en tête).
  # Les modules réseau sont donc scindés : net/<x>.cpp garde HTTP/tâches/cache,
  # net/<x>_parse.cpp le décodage pur — seul ce dernier est compilé ici.
  "${APP_DIR}/app/call_log_parse.cpp"
  "${APP_DIR}/app/ephemeris.cpp"
  "${APP_DIR}/app/gate_logic.cpp"
  "${APP_DIR}/app/ha_entity_id.cpp"
  "${APP_DIR}/app/motion_detect.cpp"
  "${APP_DIR}/media/g711.cpp"
  "${APP_DIR}/media/ringtone.cpp"
  "${APP_DIR}/media/wav_parse.cpp"
  "${APP_DIR}/ui/premul_blend.cpp"
  "${APP_DIR}/net/esphome_frame.cpp"
  "${APP_DIR}/net/esphome_pb.cpp"
  "${APP_DIR}/net/file_path.cpp"
  "${APP_DIR}/net/noise_nnpsk0.cpp"
  "${APP_DIR}/net/gcal_parse.cpp"
  "${APP_DIR}/net/github_release.cpp"
  "${APP_DIR}/net/weather_parse.cpp"
  "${APP_DIR}/sys/i18n.cpp"
  "${APP_DIR}/sys/ota_image_check.cpp"
  "${BSP_DIR}/ppa_srm_quirk.cpp"
)

# Vecteurs binaires lus au runtime par les tests (en-tête d'image OTA réelle) :
# le chemin est passé en macro plutôt que relatif au cwd, que ctest ne garantit
# pas.
target_compile_definitions(tests PRIVATE
  VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/vectors")

target_include_directories(tests PRIVATE
  "${APP_DIR}"                          # inclusions qualifiées : "media/g711.h"
  "${BSP_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"   # remplace esp_log.h & co. sur host
)

target_link_libraries(tests PRIVATE GTest::gtest_main cjson_host tfpsacrypto)

# strlcpy (BSD, fournie par le newlib d'ESP-IDF) manque au host : cf. le
# commentaire en tête de stubs/host_compat.h pour le pourquoi du force-include.
# glibc >= 2.38 declare deja strlcpy : le stub entrerait alors en conflit.
include(CheckSymbolExists)
check_symbol_exists(strlcpy "string.h" HOST_HAS_STRLCPY)
if(HOST_HAS_STRLCPY)
  target_compile_definitions(tests PRIVATE HOST_HAS_STRLCPY)
endif()
set(HOST_COMPAT "${CMAKE_CURRENT_SOURCE_DIR}/stubs/host_compat.h")
if(MSVC)
  target_compile_options(tests PRIVATE "/FI${HOST_COMPAT}")
else()
  target_compile_options(tests PRIVATE -include "${HOST_COMPAT}")
endif()

if(MSVC)
  target_compile_options(tests PRIVATE /W4 /permissive-)
else()
  target_compile_options(tests PRIVATE -Wall -Wextra)
  if(MINGW)
    # Sans ça l'exécutable réclame libstdc++-6 / libgcc_s_seh-1 / libwinpthread-1
    # au lancement et meurt en 0xC0000135 dès qu'il tourne hors d'un shell où
    # mingw64\bin est sur le PATH — ce qui inclut la découverte des tests par
    # CMake et le Test Explorer de l'IDE.
    target_link_options(tests PRIVATE -static)
  endif()
  # Les sanitizers attrapent plus de bugs que les assertions elles-mêmes.
  # Non activés par défaut : ASan sous MinGW est incomplet.
  option(TESTS_SANITIZE "Build tests with ASan/UBSan" OFF)
  if(TESTS_SANITIZE)
    target_compile_options(tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tests PRIVATE -fsanitize=address,undefined)
  endif()
endif()

# DISCOVERY_MODE PRE_TEST : n'exécute pas le binaire pendant le build pour
# énumérer les cas, mais au lancement de ctest. Sur cette machine l'antivirus
# scanne l'exe à son premier lancement et dépassait le timeout de découverte de
# 5 s par défaut, faisant échouer le build alors que la compilation était bonne.
gtest_discover_tests(tests
  DISCOVERY_MODE PRE_TEST
  DISCOVERY_TIMEOUT 60
)
