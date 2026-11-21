#pragma once

#include <cstddef>

namespace dbg_log {

// Installe un hook esp_log_set_vprintf (une fois) qui copie chaque ligne de
// log dans un ring buffer SPIRAM tout en chaînant vers la sortie d'origine
// (UART préservé). À appeler tôt dans app_main, après bsp::init().
void init();

// Copie jusqu'à `cap` octets non encore lus dans `dst`, retourne le nombre
// copié. Si le ring contient plus de `cap` octets, garde les plus récents.
// Les octets retournés sont consommés (curseur avancé). Sûr depuis la tâche
// LVGL ; producteur (hook) et consommateur (drain) protégés par spinlock.
size_t drain(char *dst, size_t cap);

// Sink secondaire optionnel : appelé depuis le hook vprintf pour chaque ligne
// formatée (déjà bornée en longueur). DOIT être léger + non bloquant (s'exécute
// dans le contexte de l'appelant du log). Utilisé par esphome_api pour tee-er
// les lignes vers les clients de l'API, et par sd_log pour les pousser dans son ring — sans
// consommer le ring Debug (drain() est destructif, il appartient à l'UI).
using LineSink = void (*)(const char *line, size_t len);

// Deux slots. Tableau fixe plutôt que liste chaînée : aucun sink ne se
// détache en pratique, et le chemin de log ne mérite pas un déréférencement
// de plus. Renvoie false si les deux slots sont déjà pris.
bool add_line_sink(LineSink sink);

} // namespace dbg_log
