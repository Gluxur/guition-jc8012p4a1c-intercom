#pragma once

#include <cstdint>

// Journal persistant sur carte microSD.
//
// Pourquoi : le visiophone est mural, sans clavier, et l'UART n'est pas
// branché en exploitation. Quand la carte redémarre seule (watchdog de
// connectivité, panique, brownout), la seule trace exploitable est ce qui a
// été écrit sur la carte AVANT la coupure, plus le bandeau du démarrage
// suivant, qui nomme la cause du reset.
//
// Chemin d'une ligne : ESP_LOGx -> dbg_log (hook vprintf) -> sink d'ici ->
// ring PSRAM -> tâche « sd_log » -> fichier. Le sink ne touche JAMAIS au
// système de fichiers : il s'exécute dans le contexte de l'appelant du log,
// potentiellement une tâche temps réel, où un fwrite bloquerait.
namespace sd_log {

enum class Health : uint8_t {
  kDisabled,  // pas de carte, ring non alloué, ou coupé par l'utilisateur
  kBuffering, // capture en RAM, fichier pas encore ouvert (attente de l'heure)
  kWriting,   // régime nominal
  kError,     // écritures en échec, carte démontée
};

// Phase 1 — juste après dbg_log::init(), avant tout autre module. Alloue le
// ring et enregistre le sink : les lignes du tout début de boot (NVS,
// settings, BSP) sont capturées avant même que la carte soit montable. Sans
// ce découpage, les logs les plus utiles au diagnostic d'un boot raté
// n'atteindraient jamais le fichier.
void init_ring();

// Phase 2 — après bsp::Bsp::init(). Monte la carte, démarre la tâche
// d'écriture (qui purge, ouvre le fichier du jour et écrit le bandeau).
void start();

// Vide le ring et fsync. À appeler avant tout esp_restart() volontaire.
// Bornée à ~500 ms : un redémarrage ne doit jamais dépendre de la carte.
void flush_now();

// Suspend l'écriture ET ATTEND que la tâche d'écriture ait fermé son fichier.
// Indispensable avant un formatage : pause() ne posait qu'un drapeau, la tâche
// pouvait être en plein fwrite/fsync avec le fichier ouvert quand
// esp_vfs_fat_sdcard_format() démontait la partition sous elle.
// false = la tâche n'a pas acquitté dans le délai imparti ; l'appelant ne doit
// alors PAS toucher au système de fichiers.
bool pause_and_wait(uint32_t timeout_ms = 2000);

// Reprend l'écriture (le fichier du jour est rouvert au tour suivant). Pendant
// la suspension les lignes continuent d'entrer dans le ring.
void resume();

// Interrupteur utilisateur (page Configuration), effet immédiat, persisté par
// prefs::set_sd_log_enabled. Distinct de pause_and_wait/resume, qui sont une
// suspension COURTE et technique (formatage, mode clé USB) : les deux
// coexistent sans se marcher dessus — sortir du mode USB ne rallume pas un
// journal que l'utilisateur a éteint.
//
// À l'arrêt, le sink cesse d'alimenter le ring (sinon il déborderait en
// silence et gonflerait dropped_lines pour rien) et la tâche ferme son
// fichier. La carte reste montée : ring.wav et le mode clé USB en dépendent.
void set_enabled(bool on);

[[nodiscard]] bool enabled();

[[nodiscard]] Health health();

// Lignes perdues par débordement du ring depuis le démarrage.
[[nodiscard]] uint32_t dropped_lines();

// Nom (sans chemin) du fichier en cours d'écriture, "" si aucun.
[[nodiscard]] const char *current_file();

// Écrit / relit / compare / supprime un fichier temporaire, et journalise son
// verdict. Même motif que g711::self_test et weather::self_test.
void self_test();

} // namespace sd_log
