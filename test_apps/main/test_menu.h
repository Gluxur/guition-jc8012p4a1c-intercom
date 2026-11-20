#pragma once

namespace tst {

/* Menu interactif de la console, en remplacement d'unity_run_menu().
 * Même grammaire, plus l'entrée « 0 » qui lance toute la suite (unity_run_menu
 * ne connaît que '*', et '0' y tombait dans la branche « index de test » où il
 * valait -1, donc rien). Ne modifie AUCUNE source ESP-IDF : tout passe par
 * l'API publique du composant unity. Ne rend jamais la main. */
void run_menu();

} // namespace tst
