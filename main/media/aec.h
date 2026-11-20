#pragma once

#include <cstddef>
#include <cstdint>

// Annulation d'écho acoustique du backchannel micro (esp-sr, modes FD).
//
// Toute la chaîne audio du visiophone est à 8 kHz (G.711), l'AEC d'esp-sr
// n'accepte que 16 kHz : ce module convertit autour de lui (mesuré : +0,4 %
// d'un cœur, négligeable) et regroupe les trames de 20 ms du micro en blocs
// de 32 ms, la taille imposée par les modes FD. Latence ajoutée : un bloc.
//
// Un seul appelant, la tâche mic_send : aucun verrou.
namespace aec {

// Crée l'instance. `high_perf` = FD_HIGH_PERF (moins de SRAM interne, plus de
// PSRAM), sinon FD_LOW_COST (moins de CPU). false si la création échoue —
// typiquement la SRAM interne, rare pendant un appel : l'appelant continue
// alors sans AEC, le micro marche toujours.
[[nodiscard]] bool start(bool high_perf);

// Libère l'instance et les tampons. Sans effet si rien n'est ouvert.
void stop();

[[nodiscard]] bool active();

// `mic` et `ref` (ce que joue le HP, aligné échantillon pour échantillon) en
// entrée, `out` = micro débarrassé de l'écho, même longueur `n`, 8 kHz. Les
// premiers appels rendent du silence le temps de remplir le premier bloc.
// `n` <= kMaxFrame.
inline constexpr size_t kMaxFrame = 320;
void process(const int16_t *mic, const int16_t *ref, int16_t *out, size_t n);

} // namespace aec
