cmake_minimum_required(VERSION 3.16)

# Projet ESP-IDF INDÉPENDANT du firmware de la racine : il produit son propre
# binaire, flashé à la place de l'application. Le firmware de production ne
# grossit donc pas d'un octet — rien de ce dossier n'est linké dedans.
#
# Périmètre : ce qui ne peut PAS être testé sur PC — moteur JPEG matériel, PPA,
# double framebuffer DSI, codec ES8311. Toute logique pure va dans test/
# (GoogleTest sur host, boucle < 1 s) et pas ici.

# Réutilise components/ du projet parent (bsp + drivers panneau/tactile
# vendorisés) sans les dupliquer.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(visiophone_target_tests)
