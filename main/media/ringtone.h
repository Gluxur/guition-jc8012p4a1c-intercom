#pragma once

#include <cstddef>
#include <cstdint>

// Sonnerie ding-dong (onde carrée 8 kHz mono) mixée par-dessus l'audio distant.
// Mixeur PASSIF : aucune tâche propre, aucun accès codec. mix_into() est appelée
// par audio_render_task (écrivain codec unique) — voir main.cpp. Toutes les
// fonctions sont sûres à appeler depuis n'importe quelle tâche (start/stop/
// is_ringing via std::atomic ; mix_into uniquement depuis audio_render_task).
namespace ringtone {

// Arme la sonnerie : réinitialise le curseur mélodie + l'horodatage de départ.
// Non bloquant. Idempotent (un start pendant qu'elle sonne redémarre la mélodie).
void start();

// Désarme la sonnerie (coupure nette).
void stop();

// True tant que la sonnerie est armée ET que < 30 s se sont écoulées.
bool is_ringing();

// Mixe la sonnerie dans buf[0..n) : ajout saturé d'échantillons carrés, mono
// int16 @ 8 kHz. No-op si non armée. Fait avancer le curseur mélodie de n
// échantillons. Auto-désarme si > 30 s depuis start(). À n'appeler QUE depuis
// audio_render_task (pas de verrou interne sur le curseur).
void mix_into(int16_t *buf, size_t n);

// --- Source PCM externe (WAV préchargé depuis la carte SD) -----------------
// Quand une source est installée, mix_into() la joue en boucle À LA PLACE de la
// mélodie synthétisée ; sinon le ding-dong interne reste le comportement par
// défaut. Le reste du contrat ne change pas : même armement, même plafond 30 s,
// même coupure nette par stop().

// `pcm` doit rester valide tant que la source est installée. En pratique c'est
// un bloc PSRAM alloué au boot par ringtone_sd et jamais libéré.
// À n'appeler QUE depuis app_main, AVANT la création de la tâche aud_render :
// le pointeur est écrit sans verrou parce que sa seule lectrice n'existe pas
// encore à cet instant (cf. l'ordre imposé dans app_main).
void set_source(const int16_t *pcm, size_t samples);

// Désinstalle la source : la mélodie synthétisée reprend. Aucun appelant
// applicatif — existe pour que les tests host réinitialisent l'état.
void clear_source();

[[nodiscard]] bool has_source();

// Volume de la sonnerie, 0-100 %, appliqué à la source WAV comme à la mélodie
// synthétisée. 100 % ne laisse PAS passer un WAV pleine échelle : le plafond
// est kAmplitude, le même cadrage anti-clipping que le ding-dong.
void set_volume(uint8_t percent);

// --- Tonalité de test vers la caméra (page Micro/Debug) -------------------
// Même mélodie, mais second curseur INDÉPENDANT de celui de mix_into() : le
// consommateur est mic_send_task, pas audio_render_task. Les deux jeux d'état
// étant disjoints, les deux tâches avancent en parallèle sans verrou.
// Différence de comportement : aucun plafond de durée (la sonnerie s'auto-
// désarme à 30 s), la tonalité tourne jusqu'à mic_tone_stop().

// Arme/désarme la tonalité. Non bloquant, sûr depuis n'importe quelle tâche.
void mic_tone_start();
void mic_tone_stop();
bool mic_tone_active();

// Mixe la tonalité dans buf[0..n) (ajout saturé) et avance son curseur de n
// échantillons. No-op si désarmée. À n'appeler QUE depuis mic_send_task.
void mic_tone_mix_into(int16_t *buf, size_t n);

} // namespace ringtone
