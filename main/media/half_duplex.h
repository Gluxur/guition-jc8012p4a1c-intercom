#pragma once

#include <cstddef>
#include <cstdint>

// Semi-duplex du backchannel micro : pendant que le haut-parleur joue la voix
// du visiteur, le micro est atténué, le visiteur ne s'entend donc plus dans la
// platine. Alternative à l'AEC esp-sr (media/aec) : aucun calcul lourd, aucune
// SRAM interne, pas de référence I2S stéréo. Prix : pas de parole simultanée,
// le visiteur a priorité.
//
// Deux tâches : aud_render signale ce qu'il envoie au HP (note_far), mic_send
// lit l'état (far_speaking) et atténue sa trame (Ducker). Le seul état partagé
// est un horodatage atomique 32 bits.
namespace half_duplex {

// Valeurs de départ, à régler à l'écoute sur la tablette (non mesurées).
// Crête d'une trame HP au-dessus de laquelle on considère que le visiteur
// parle : ~-30 dBFS, au-dessus du souffle G.711 d'une platine au repos.
inline constexpr int16_t kFarThreshold = 1000;
// Maintien après la dernière trame forte : couvre la file DMA I2S (jusqu'à
// ~180 ms de son déjà poussé mais pas encore joué) plus la réverbération.
inline constexpr uint32_t kHoldMs = 300;
// Gain du micro atténué, Q15 : 3277 / 32768 = 0,1 soit -20 dB. Pas un silence
// complet : un micro qui « disparaît » s'entend comme une coupure de ligne.
inline constexpr int32_t kDuckGainQ15 = 3277;
inline constexpr int32_t kUnityQ15 = 32768;
// Rampes par échantillon (8 kHz) : attaque en 40 éch. = 5 ms, pour couper
// l'écho dès son arrivée ; relâche en 800 éch. = 100 ms, sans « pompage ».
inline constexpr int32_t kAttackStep = (kUnityQ15 - kDuckGainQ15) / 40;
inline constexpr int32_t kReleaseStep = (kUnityQ15 - kDuckGainQ15) / 800;

// Crête absolue de la trame, 0..32767 (-32768 compte pour 32767).
[[nodiscard]] int16_t peak(const int16_t *pcm, size_t n);

// Vrai tant que `now_ms` est avant `until_ms`. Résiste au rebouclage du
// compteur 32 bits (49 jours).
[[nodiscard]] bool before(uint32_t now_ms, uint32_t until_ms);

// Appelé par aud_render pour chaque bloc envoyé au HP, 8 kHz : si la trame
// est forte, prolonge la fenêtre « visiteur parle » jusqu'à la fin du bloc
// plus kHoldMs.
void note_far(const int16_t *pcm, size_t n, uint32_t now_ms);

// Lu par mic_send à chaque trame.
[[nodiscard]] bool far_speaking(uint32_t now_ms);

// Remet la fenêtre à zéro (début d'appel, tests).
void clear_far();

// Gain du micro, rampe incluse pour éviter les clics. Une instance, une tâche.
class Ducker {
public:
  // Atténue `pcm` en place. `duck` = cible -20 dB, sinon gain unité.
  void apply(int16_t *pcm, size_t n, bool duck);
  void reset() { gain_ = kUnityQ15; }
  [[nodiscard]] int32_t gain_q15() const { return gain_; }

private:
  int32_t gain_ = kUnityQ15;
};

} // namespace half_duplex
