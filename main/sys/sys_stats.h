#pragma once

namespace sys_stats {

struct CpuUsage {
  int core_a; // Cœur 0 (PRO CPU)
  int core_b; // Cœur 1 (APP CPU)
};

CpuUsage cpu_usage_percent();

/* Occupation PSRAM en %, calcul instantané (pas d'état partagé). */
int psram_used_percent();

/* Arme/désarme les diagnostics de performance (répartition CPU par tâche,
 * détention du mutex LVGL). Désarmés au démarrage.
 * Pourquoi un drapeau et pas ESP_LOGD : CONFIG_LOG_MAXIMUM_LEVEL vaut INFO, donc
 * ESP_LOGD est supprimé à la compilation — inutilisable pour un diagnostic
 * qu'on veut pouvoir rallumer. Et le relever compilerait tous les ESP_LOGD de
 * l'IDF, ce que ce projet évite justement. */
void set_perf_diag(bool on);
[[nodiscard]] bool perf_diag();

/* Journalise en UNE ligne les tâches les plus consommatrices, avec leur cœur,
 * plus la SRAM interne libre. Diagnostic : cpu_usage_percent() dit QU'UN cœur
 * sature, pas QUI le sature.
 * Appelant unique attendu (état statique non protégé), au plus 1x/s.
 * No-op tant que perf_diag() est faux — échantillonnage compris, car c'est
 * uxTaskGetSystemState() (ordonnanceur suspendu) qui coûte, pas le log. */
void log_task_cpu();

/* Même ligne que log_task_cpu(), mais seulement quand l'un des cœurs a atteint
 * min_core_pct sur la fenêtre écoulée depuis l'appel précédent. Table propre,
 * indépendante de perf_diag : faite pour tourner en permanence à basse cadence
 * (~2 ms d'ordonnanceur suspendu par appel) et attraper une saturation qui
 * n'apparaît qu'après des heures, hors session. Appelant unique attendu. */
void log_busy_tasks(int min_core_pct);

} // namespace sys_stats
