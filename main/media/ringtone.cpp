#include "ringtone.h"

#include <atomic>
#include <math.h>

#include "esp_timer.h"

namespace ringtone {
namespace {

constexpr uint32_t kSampleRate = 8000;
// +6 dB par rapport aux 8000 d'origine : la sonnerie n'était pas assez forte
// alors que le volume haut-parleur était déjà à 100 %. Sûr parce que la fenêtre
// est déjà la bonne par construction — mix_into() ne fait quelque chose que
// pendant is_ringing(), qui devient faux exactement à ringtone::stop(), appelé
// en tout premier par mic_session::start() quand l'utilisateur décroche. La
// somme avec l'audio distant ne dure donc que tant que personne n'a répondu, et
// il reste 16k de marge sous int16 max (sat_add borne le reste).
constexpr int16_t kAmplitude = 16000;
// Tonalité de test : sommée à la capture micro, pas à un flux distant déjà
// fort. On vise ~-3 dBFS pour un signal franchement audible côté caméra et un
// VU-mètre lisible ; il reste 9k de marge avant saturation si on parle en même
// temps.
constexpr int16_t kMicToneAmplitude = 23000;
// Plancher d'enveloppe : sans lui, la note longue (850 ms) retombe à zéro et le
// VU ne montre qu'un pic fugace. La queue reste à 35 % au lieu de s'éteindre.
constexpr float kMicToneDecayFloor = 0.35f;
constexpr int64_t kMaxRingUs = 30'000'000; // filet de sécurité 30 s

struct Note {
  uint16_t freq_hz; // 0 => silence (pause)
  uint16_t dur_ms;
};

// Nouvelles fréquences (Mi5 -> Do5) et timings adaptés pour une cloche
constexpr Note kMelody[] = {
    {659, 250}, // ding (Mi5) - frappe claire et courte
    {440, 850}, // dong (Do5) - résonance longue
    {0, 500},   // pause avant répétition
};
constexpr size_t kMelodyLen = sizeof(kMelody) / sizeof(kMelody[0]);

std::atomic<bool> s_armed{false};
// Atomique : écrit par start() (sonnette, UI) pendant que aud_render le lit
// dans is_ringing(). Un int64 se lit en deux mots sur RV32 : une lecture
// déchirée au moment d'une re-sonnerie pouvait faire croire aux 30 s écoulées
// et désarmer la sonnerie aussitôt relancée.
std::atomic<int64_t> s_start_us{0};

// Curseur de la tonalité de test (consommateur : mic_send_task). Volontairement
// séparé de s_note_idx/s_phase pour que sonnerie et tonalité puissent tourner
// simultanément sans se voler leur position dans la mélodie.
std::atomic<bool> s_mic_armed{false};
size_t s_mic_note_idx = 0;
uint32_t s_mic_phase = 0;

// Curseur mélodie. Touché uniquement dans mix_into (mono-consommateur
// audio_render_task) et réinitialisé dans start() avant l'armement -> pas de
// course. samples_per_note dérivé à la volée depuis dur_ms.
// Un `note_pos` accompagnait autrefois `phase` : les deux étaient remis à zéro
// et incrémentés exactement aux mêmes instants — un seul compteur suffit, et
// c'est celui que chime_sample lit déjà pour l'enveloppe.
size_t s_note_idx = 0; // index dans kMelody
uint32_t s_phase = 0;  // échantillons écoulés dans la note courante

// Source PCM externe (WAV préchargé en PSRAM par ringtone_sd). Elle n'est plus
// écrite seulement au boot : la page Configuration et la sortie du mode clé
// USB la rechargent pendant que aud_render tourne. Le couple {pcm, n} est donc
// publié d'un bloc par pointeur atomique (deux emplacements alternés : celui
// qu'on remplit n'est jamais celui que le lecteur peut tenir). Avant, le
// lecteur pouvait voir le nouveau pcm avec l'ancien n — lecture hors bornes
// si le nouveau fichier était plus court.
struct Src {
  const int16_t *pcm;
  size_t n;
};
Src s_src_slots[2] = {};
int s_src_next = 0;                        // emplacement à remplir (écrivain)
std::atomic<const Src *> s_src{nullptr};   // nullptr = mélodie synthétisée
size_t s_src_pos = 0; // curseur du lecteur ; borné à chaque usage par src->n
// Nouvelle source publiée : le lecteur remet son curseur à zéro (le curseur
// lui appartient, l'écrivain ne le touche plus).
std::atomic<bool> s_src_rewind{false};

// 100 % plafonne le WAV à kAmplitude et non à la pleine échelle : un fichier
// normalisé à 0 dBFS saturerait sinon à chaque somme avec l'audio distant.
// Q15 pour que le chemin chaud ne fasse qu'une multiplication et un décalage,
// jamais de flottant — mix_into tourne dans aud_render, priorité 6.
constexpr uint32_t kQ15One = 32768u;
constexpr uint32_t kFullScale = 32767u;

uint16_t gain_q15_for(uint8_t pct) {
  // uint64_t obligatoire : 100 * 8000 * 32768 = 2,6e10, hors int32.
  const uint64_t g =
      static_cast<uint64_t>(pct) * kAmplitude * kQ15One / (100ull * kFullScale);
  return static_cast<uint16_t>(g);
}

// Atomiques : écrits par l'UI / l'API, lus par aud_render.
std::atomic<uint8_t> s_volume_pct{100};
std::atomic<uint16_t> s_gain_q15{static_cast<uint16_t>(
    static_cast<uint64_t>(100) * kAmplitude * kQ15One / (100ull * kFullScale))};

inline int16_t sat_add(int16_t a, int16_t b) {
  int32_t s = (int32_t)a + (int32_t)b;
  if (s > 32767)
    s = 32767;
  if (s < -32768)
    s = -32768;
  return (int16_t)s;
}

// Échantillon sinusoïdal avec atténuation linéaire. freq 0 => silence.
inline int16_t chime_sample(uint16_t freq_hz, uint32_t phase,
                            uint32_t total_samples, int16_t amplitude,
                            float decay_floor) {
  if (freq_hz == 0 || total_samples == 0)
    return 0;

  // 1. Onde sinusoïdale (2 * PI = 6.2831853f)
  float time_s = (float)phase / kSampleRate;
  float sine_wave = sinf(6.2831853f * freq_hz * time_s);

  // 2. Enveloppe d'atténuation (Fade out linéaire)
  // Commence à 1.0 au début de la note et descend à 0.0 à la fin
  float decay = 1.0f - ((float)phase / total_samples);
  if (decay < decay_floor)
    decay = decay_floor; // plancher (0 pour la sonnerie = extinction complète)

  // 3. Signal final
  return (int16_t)(amplitude * sine_wave * decay);
}

// Avance un curseur d'un échantillon (change de note quand dur_ms atteinte,
// boucle en fin de mélodie). Réinitialise la phase à chaque note. Paramétré par
// référence : sonnerie et tonalité de test ont chacune sa paire d'état.
inline void advance(size_t &note_idx, uint32_t &phase) {
  phase++;
  const uint32_t samples_this_note =
      (uint32_t)kMelody[note_idx].dur_ms * kSampleRate / 1000u;
  if (phase >= samples_this_note) {
    phase = 0;
    note_idx = (note_idx + 1) % kMelodyLen;
  }
}

} // namespace

void start() {
  s_note_idx = 0;
  s_phase = 0;
  s_src_pos = 0;
  s_start_us.store(esp_timer_get_time(), std::memory_order_relaxed);
  s_armed.store(true, std::memory_order_release);
}

void stop() { s_armed.store(false, std::memory_order_release); }

bool is_ringing() {
  if (!s_armed.load(std::memory_order_acquire))
    return false;
  if (esp_timer_get_time() - s_start_us.load(std::memory_order_relaxed) >
      kMaxRingUs) {
    s_armed.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void mix_into(int16_t *buf, size_t n) {
  if (!is_ringing() || buf == nullptr)
    return;

  // Source WAV : simple lecture bouclée. Le modulo remplace exactement
  // l'advance() de la mélodie, avec les mêmes garanties de mono-consommateur.
  // Lu UNE fois par appel : pcm et n viennent forcément de la même source.
  const Src *src = s_src.load(std::memory_order_acquire);
  if (src != nullptr) {
    const uint32_t gain = s_gain_q15.load(std::memory_order_relaxed);
    // Curseur hérité d'une source plus longue (rechargement à chaud) : repart
    // du début plutôt que lire hors du tampon.
    if (s_src_rewind.exchange(false, std::memory_order_relaxed) ||
        s_src_pos >= src->n)
      s_src_pos = 0;
    for (size_t i = 0; i < n; i++) {
      const int32_t s =
          (static_cast<int32_t>(src->pcm[s_src_pos]) * (int32_t)gain) >> 15;
      buf[i] = sat_add(buf[i], static_cast<int16_t>(s));
      if (++s_src_pos >= src->n)
        s_src_pos = 0;
    }
    return;
  }

  const int16_t amp = static_cast<int16_t>(
      static_cast<int32_t>(kAmplitude) *
      s_volume_pct.load(std::memory_order_relaxed) / 100);
  for (size_t i = 0; i < n; i++) {
    // Calcul du nombre total d'échantillons pour la note en cours
    const uint32_t samples_this_note =
        (uint32_t)kMelody[s_note_idx].dur_ms * kSampleRate / 1000u;

    // Génération du son de cloche
    const int16_t s = chime_sample(kMelody[s_note_idx].freq_hz, s_phase,
                                   samples_this_note, amp, 0.0f);

    buf[i] = sat_add(buf[i], s);
    advance(s_note_idx, s_phase);
  }
}

void set_source(const int16_t *pcm, size_t samples) {
  // Une source vide n'en est pas une : un WAV au bloc `data` nul doit laisser
  // la mélodie synthétisée reprendre, pas rendre la sonnerie muette.
  const bool usable = pcm != nullptr && samples > 0;
  if (!usable) {
    s_src.store(nullptr, std::memory_order_release);
    return;
  }
  Src &slot = s_src_slots[s_src_next];
  s_src_next ^= 1;
  slot.pcm = pcm;
  slot.n = samples;
  s_src_rewind.store(true, std::memory_order_relaxed);
  s_src.store(&slot, std::memory_order_release);
}

void clear_source() { s_src.store(nullptr, std::memory_order_release); }

bool has_source() { return s_src.load(std::memory_order_acquire) != nullptr; }

void set_volume(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  s_volume_pct.store(percent, std::memory_order_relaxed);
  s_gain_q15.store(gain_q15_for(percent), std::memory_order_relaxed);
}

void mic_tone_start() {
  s_mic_note_idx = 0;
  s_mic_phase = 0;
  s_mic_armed.store(true, std::memory_order_release);
}

void mic_tone_stop() { s_mic_armed.store(false, std::memory_order_release); }

bool mic_tone_active() { return s_mic_armed.load(std::memory_order_acquire); }

void mic_tone_mix_into(int16_t *buf, size_t n) {
  if (!mic_tone_active() || buf == nullptr)
    return;
  for (size_t i = 0; i < n; i++) {
    const uint32_t samples_this_note =
        (uint32_t)kMelody[s_mic_note_idx].dur_ms * kSampleRate / 1000u;
    const int16_t s = chime_sample(kMelody[s_mic_note_idx].freq_hz, s_mic_phase,
                                   samples_this_note, kMicToneAmplitude,
                                   kMicToneDecayFloor);
    buf[i] = sat_add(buf[i], s);
    advance(s_mic_note_idx, s_mic_phase);
  }
}

} // namespace ringtone