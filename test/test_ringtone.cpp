#include "media/ringtone.h"

#include <gtest/gtest.h>

#include "esp_timer.h" // stub host : horloge pilotée par le test

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

// Constantes internes de ringtone.cpp, répliquées ici car le mixeur ne les
// expose pas (namespace anonyme). Si elles bougent là-bas, ces tests doivent
// échouer bruyamment plutôt que de se réaligner en silence — d'où les valeurs
// en dur et non un header partagé.
constexpr int kSampleRate = 8000;
constexpr int16_t kAmplitude = 16000;      // sonnerie (+6 dB, cf. ringtone.cpp)
constexpr int16_t kMicToneAmplitude = 23000; // tonalité de test micro
constexpr int64_t kMaxRingUs = 30'000'000;

// kMelody : {659 Hz, 250 ms}, {440 Hz, 850 ms}, {silence, 500 ms}.
constexpr int kDingSamples = 250 * kSampleRate / 1000;  // 2000
constexpr int kDongSamples = 850 * kSampleRate / 1000;  // 6800
constexpr int kPauseSamples = 500 * kSampleRate / 1000; // 4000
constexpr int kMelodySamples = kDingSamples + kDongSamples + kPauseSamples;

int16_t peak_abs(const std::vector<int16_t> &v, size_t from, size_t to) {
  int32_t m = 0;
  for (size_t i = from; i < to; i++) {
    const int32_t a = v[i] < 0 ? -static_cast<int32_t>(v[i]) : v[i];
    m = std::max(m, a);
  }
  return static_cast<int16_t>(m);
}

// L'état du mixeur (curseurs mélodie, flags d'armement, horodatage de départ)
// est global au module. La fixture le remet à plat avant chaque test pour
// qu'aucun ne dépende de l'ordre d'exécution.
class Ringtone : public ::testing::Test {
protected:
  void SetUp() override {
    // Base non nulle : une horloge démarrant à 0 masquerait un oubli de
    // mémoriser s_start_us (la différence resterait sous kMaxRingUs par hasard).
    esp_timer_stub::set_now(1'000'000);
    ringtone::stop();
    ringtone::mic_tone_stop();
    // Source et volume sont globaux au module comme les curseurs : sans remise
    // à plat, un test laissant une source installée ferait échouer les cas
    // « mélodie synthétisée » qui le suivent.
    ringtone::clear_source();
    ringtone::set_volume(100);
  }
  void TearDown() override {
    ringtone::stop();
    ringtone::mic_tone_stop();
    ringtone::clear_source();
    ringtone::set_volume(100);
  }
};

// --- Armement --------------------------------------------------------------

TEST_F(Ringtone, DisarmedMixIsANoOp) {
  // Le mixeur tourne dans audio_render_task sur des trames déjà remplies par
  // l'audio distant : s'il écrivait alors qu'il est désarmé, il écraserait la
  // voix de l'interlocuteur.
  std::vector<int16_t> buf(64, 1234);
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, StopSilencesImmediately) {
  ringtone::start();
  ASSERT_TRUE(ringtone::is_ringing());
  ringtone::stop();
  EXPECT_FALSE(ringtone::is_ringing());

  std::vector<int16_t> buf(64, 1234);
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, NullBufferIsIgnored) {
  ringtone::start();
  ringtone::mix_into(nullptr, 64); // ne doit pas déréférencer
  ringtone::mic_tone_start();
  ringtone::mic_tone_mix_into(nullptr, 64);
  SUCCEED();
}

// --- Filet de sécurité 30 s ------------------------------------------------
// Jamais couvert par l'ancien self_test de boot : sans horloge pilotable il
// aurait fallu attendre 30 s au démarrage.

TEST_F(Ringtone, KeepsRingingJustBeforeTheSafetyCap) {
  ringtone::start();
  esp_timer_stub::advance(kMaxRingUs - 1);
  EXPECT_TRUE(ringtone::is_ringing());
}

TEST_F(Ringtone, DisarmsItselfPastTheSafetyCap) {
  // Sans ce filet, un `start()` dont le `stop()` se perd (Home Assistant muet, session
  // avortée) ferait sonner l'appareil indéfiniment.
  ringtone::start();
  esp_timer_stub::advance(kMaxRingUs + 1);
  EXPECT_FALSE(ringtone::is_ringing());

  std::vector<int16_t> buf(64, 1234);
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, RestartResetsTheSafetyCap) {
  ringtone::start();
  esp_timer_stub::advance(kMaxRingUs + 1);
  ASSERT_FALSE(ringtone::is_ringing());
  ringtone::start();
  EXPECT_TRUE(ringtone::is_ringing());
}

// --- Génération du signal ---------------------------------------------------

TEST_F(Ringtone, GeneratesAnOscillationOnBothPolarities) {
  // Un signal qui ne franchirait pas zéro serait une composante continue :
  // inaudible dans le haut-parleur, mais consommant toute la marge de mix.
  std::vector<int16_t> buf(kDingSamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  bool saw_pos = false, saw_neg = false;
  for (int16_t s : buf) {
    if (s > kAmplitude / 2)
      saw_pos = true;
    if (s < -kAmplitude / 2)
      saw_neg = true;
  }
  EXPECT_TRUE(saw_pos);
  EXPECT_TRUE(saw_neg);
}

TEST_F(Ringtone, NeverExceedsItsNominalAmplitude) {
  // La sonnerie est sommée à l'audio distant : dépasser kAmplitude mangerait la
  // marge anti-clipping prévue dans ringtone.cpp.
  std::vector<int16_t> buf(kMelodySamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_LE(peak_abs(buf, 0, buf.size()), kAmplitude);
}

TEST_F(Ringtone, PauseNoteEmitsExactSilence) {
  // 3e note = {0 Hz, 500 ms}. Une fréquence nulle doit produire des zéros
  // exacts, pas un résidu d'enveloppe : c'est le silence entre deux ding-dong.
  std::vector<int16_t> buf(kMelodySamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  for (int i = kDingSamples + kDongSamples; i < kMelodySamples; i++)
    ASSERT_EQ(0, buf[i]) << "i=" << i;
  // ...et les deux notes précédentes, elles, sonnent.
  EXPECT_GT(peak_abs(buf, 0, kDingSamples), kAmplitude / 2);
  EXPECT_GT(peak_abs(buf, kDingSamples, kDingSamples + kDongSamples),
            kAmplitude / 2);
}

TEST_F(Ringtone, MelodyLoopsAfterTheFinalPause) {
  // Le curseur reboucle : au-delà de kMelodySamples on doit réentendre le ding.
  std::vector<int16_t> buf(kMelodySamples + kDingSamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_GT(peak_abs(buf, kMelodySamples, buf.size()), kAmplitude / 2);
}

TEST_F(Ringtone, EnvelopeDecaysWithinANote) {
  // Enveloppe linéaire à plancher 0 pour la sonnerie : la queue de la note
  // longue doit être nettement plus faible que son attaque, sinon la cloche
  // sonne comme un buzzer.
  std::vector<int16_t> buf(kMelodySamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  const int dong_start = kDingSamples;
  const int dong_end = kDingSamples + kDongSamples;
  const int quarter = kDongSamples / 4;
  EXPECT_GT(peak_abs(buf, dong_start, dong_start + quarter),
            peak_abs(buf, dong_end - quarter, dong_end));
}

TEST_F(Ringtone, MixIsAdditiveOnTopOfExistingAudio) {
  // mix_into somme, il n'écrase pas : l'audio distant doit rester audible
  // sous la sonnerie.
  constexpr int16_t kBase = 1000;
  std::vector<int16_t> mixed(kDingSamples, kBase);
  std::vector<int16_t> alone(kDingSamples, 0);

  ringtone::start();
  ringtone::mix_into(mixed.data(), mixed.size());
  ringtone::start(); // curseur remis à zéro : même portion de mélodie
  ringtone::mix_into(alone.data(), alone.size());

  for (size_t i = 0; i < mixed.size(); i++)
    ASSERT_EQ(static_cast<int32_t>(alone[i]) + kBase, mixed[i]) << "i=" << i;
}

// --- Saturation -------------------------------------------------------------

TEST_F(Ringtone, SaturatesInsteadOfWrappingOnLoudPositiveAudio) {
  // Le bug que sat_add existe pour éviter : un simple `+` replierait 46000 en
  // valeur négative, soit un clic plein volume dans le haut-parleur.
  constexpr int16_t kLoud = 30000; // + kAmplitude => 46000, hors int16
  std::vector<int16_t> buf(kDingSamples, kLoud);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  bool saw_clip = false;
  for (int16_t s : buf) {
    ASSERT_GT(s, 0) << "repliement de signe";
    if (s == 32767)
      saw_clip = true;
  }
  EXPECT_TRUE(saw_clip) << "la crête aurait dû atteindre la butée";
}

TEST_F(Ringtone, SaturatesInsteadOfWrappingOnLoudNegativeAudio) {
  constexpr int16_t kLoud = -30000;
  std::vector<int16_t> buf(kDingSamples, kLoud);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  bool saw_clip = false;
  for (int16_t s : buf) {
    ASSERT_LT(s, 0) << "repliement de signe";
    if (s == -32768)
      saw_clip = true;
  }
  EXPECT_TRUE(saw_clip) << "la crête aurait dû atteindre la butée";
}

// --- Tonalité de test micro -------------------------------------------------

TEST_F(Ringtone, MicToneIsSilentUntilArmed) {
  std::vector<int16_t> buf(64, 1234);
  ringtone::mic_tone_mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
  EXPECT_FALSE(ringtone::mic_tone_active());
}

TEST_F(Ringtone, MicToneHasNoSafetyCap) {
  // Différence assumée avec la sonnerie : la tonalité de test tourne jusqu'au
  // mic_tone_stop() explicite de la page Debug.
  ringtone::mic_tone_start();
  esp_timer_stub::advance(kMaxRingUs * 10);
  EXPECT_TRUE(ringtone::mic_tone_active());

  std::vector<int16_t> buf(kDingSamples, 0);
  ringtone::mic_tone_mix_into(buf.data(), buf.size());
  EXPECT_GT(peak_abs(buf, 0, buf.size()), kMicToneAmplitude / 2);
}

TEST_F(Ringtone, MicToneKeepsAFloorWhereTheRingtoneFadesOut) {
  // Plancher d'enveloppe 0,35 : sans lui le VU-mètre de la page Debug ne
  // montrerait qu'un pic fugace sur la note longue.
  std::vector<int16_t> mic(kMelodySamples, 0);
  ringtone::mic_tone_start();
  ringtone::mic_tone_mix_into(mic.data(), mic.size());

  const int dong_end = kDingSamples + kDongSamples;
  const int quarter = kDongSamples / 4;
  const int16_t tail = peak_abs(mic, dong_end - quarter, dong_end);
  EXPECT_GT(tail, static_cast<int16_t>(kMicToneAmplitude * 0.30f));
  // La pause reste un silence exact, plancher ou pas (freq 0).
  for (int i = dong_end; i < kMelodySamples; i++)
    ASSERT_EQ(0, mic[i]) << "i=" << i;
}

TEST_F(Ringtone, MicToneCursorIsIndependentOfTheRingtoneCursor) {
  // Les deux mixeurs tournent dans des tâches différentes (audio_render_task /
  // mic_send_task) sans verrou : c'est la disjonction des curseurs qui rend ça
  // sûr. Si l'un faisait avancer l'autre, ils se voleraient leur position.
  ringtone::start();
  ringtone::mic_tone_start();

  // La sonnerie traverse le ding entier et entre dans le dong...
  std::vector<int16_t> ring(kDingSamples + 1000, 0);
  ringtone::mix_into(ring.data(), ring.size());

  // ...la tonalité, elle, doit toujours être au tout début de SON ding.
  std::vector<int16_t> mic(200, 0);
  std::vector<int16_t> mic_ref(200, 0);
  ringtone::mic_tone_mix_into(mic.data(), mic.size());
  ringtone::mic_tone_start(); // curseur tonalité remis à zéro
  ringtone::mic_tone_mix_into(mic_ref.data(), mic_ref.size());

  EXPECT_EQ(mic, mic_ref);
}

// --- Source PCM externe (WAV de la carte SD) --------------------------------

// Gain à 100 % = kAmplitude/32767 en Q15 : 100*16000*32768 / (100*32767) =
// 16000,49 -> 16000 après troncature entière. Les valeurs de source ci-dessous
// sont choisies pour que (x * 16000) >> 15 tombe juste :
//   4096 * 16000 =  65 536 000, >> 15 -> 2000 exactement
//   8192 * 16000 = 131 072 000, >> 15 -> 4000 exactement
// Les attendus sont donc calculés à la main, pas relevés sur l'implémentation.
constexpr int16_t kSrcA = 4096;
constexpr int16_t kSrcB = -4096;
constexpr int16_t kSrcC = 8192;
constexpr int16_t kMixA = 2000;
constexpr int16_t kMixB = -2000;
constexpr int16_t kMixC = 4000;

TEST_F(Ringtone, InstalledSourceReplacesTheSynthesizedMelody) {
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  ASSERT_TRUE(ringtone::has_source());

  std::vector<int16_t> buf(7, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  // Bouclage : la source de 3 échantillons couvre 7 positions.
  const std::vector<int16_t> expected = {kMixA, kMixB, kMixC, kMixA,
                                         kMixB, kMixC, kMixA};
  EXPECT_EQ(expected, buf);
}

TEST_F(Ringtone, SourceCursorRestartsWithTheRingtone) {
  // start() remet le curseur à zéro comme il le fait pour la mélodie : deux
  // coups de sonnette successifs doivent sonner pareil.
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);

  std::vector<int16_t> first(2, 0);
  ringtone::start();
  ringtone::mix_into(first.data(), first.size());

  std::vector<int16_t> second(2, 0);
  ringtone::start();
  ringtone::mix_into(second.data(), second.size());

  EXPECT_EQ(first, second);
}

TEST_F(Ringtone, HotReloadToAShorterSourceStaysInBoundsAndRewinds) {
  // Rechargement à chaud (page Configuration, sortie du mode clé USB) PENDANT
  // une sonnerie : le curseur de lecture avance dans l'ancienne source, plus
  // longue. Il doit repartir du début de la nouvelle, jamais indexer au-delà
  // de sa fin.
  std::vector<int16_t> longer(1000, kSrcA);
  ringtone::set_source(longer.data(), longer.size());
  std::vector<int16_t> warmup(900, 0);
  ringtone::start();
  ringtone::mix_into(warmup.data(), warmup.size()); // curseur à 900

  const int16_t shorter[] = {kSrcC, kSrcB};
  ringtone::set_source(shorter, 2);
  std::vector<int16_t> buf(5, 0);
  ringtone::mix_into(buf.data(), buf.size());

  const std::vector<int16_t> expected = {kMixC, kMixB, kMixC, kMixB, kMixC};
  EXPECT_EQ(expected, buf);
}

TEST_F(Ringtone, HotReloadedSourcePlaysFromItsStart) {
  // Le curseur appartient au lecteur (aud_render) : set_source ne le touche
  // plus, il signale seulement la nouvelle source. Celle-ci doit tout de même
  // être jouée depuis son premier échantillon, même si l'ancien curseur tombe
  // dans ses bornes.
  const int16_t first[] = {kSrcA, kSrcA, kSrcA};
  ringtone::set_source(first, 3);
  std::vector<int16_t> warmup(1, 0);
  ringtone::start();
  ringtone::mix_into(warmup.data(), warmup.size()); // curseur à 1

  const int16_t second[] = {kSrcC, kSrcB, kSrcA};
  ringtone::set_source(second, 3);
  std::vector<int16_t> buf(3, 0);
  ringtone::mix_into(buf.data(), buf.size());

  const std::vector<int16_t> expected = {kMixC, kMixB, kMixA};
  EXPECT_EQ(expected, buf);
}

TEST_F(Ringtone, ClearSourceRestoresTheSynthesizedMelody) {
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  ringtone::clear_source();
  EXPECT_FALSE(ringtone::has_source());

  std::vector<int16_t> buf(kDingSamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_GT(peak_abs(buf, 0, buf.size()), kAmplitude / 2);
}

TEST_F(Ringtone, EmptyOrNullSourceFallsBackToTheMelody) {
  // Un WAV dont le bloc data serait vide ne doit pas rendre la sonnerie muette.
  const int16_t src[] = {kSrcA};
  ringtone::set_source(src, 0);
  EXPECT_FALSE(ringtone::has_source());
  ringtone::set_source(nullptr, 3);
  EXPECT_FALSE(ringtone::has_source());

  std::vector<int16_t> buf(kDingSamples, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_GT(peak_abs(buf, 0, buf.size()), kAmplitude / 2);
}

TEST_F(Ringtone, SourceObeysTheThirtySecondSafetyCap) {
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  ringtone::start();
  esp_timer_stub::advance(kMaxRingUs + 1);

  std::vector<int16_t> buf(8, 1234);
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, SourceIsAddedOnTopOfExistingAudio) {
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  constexpr int16_t kBase = 500;
  std::vector<int16_t> buf(3, kBase);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  const std::vector<int16_t> expected = {
      static_cast<int16_t>(kMixA + kBase), static_cast<int16_t>(kMixB + kBase),
      static_cast<int16_t>(kMixC + kBase)};
  EXPECT_EQ(expected, buf);
}

// --- Volume -----------------------------------------------------------------

TEST_F(Ringtone, ZeroVolumeSilencesTheSource) {
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  ringtone::set_volume(0);

  std::vector<int16_t> buf(3, 1234);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, ZeroVolumeSilencesTheSynthesizedMelodyToo) {
  // Le réglage vaut pour les deux sources, sinon retirer la carte SD ferait
  // remonter le niveau tout seul.
  ringtone::set_volume(0);
  std::vector<int16_t> buf(kMelodySamples, 1234);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(1234, s);
}

TEST_F(Ringtone, HalfVolumeHalvesTheSource) {
  // 50 % -> gain Q15 = 50 * 16000 * 32768 / (100 * 32767) = 8000 (troncature)
  //   4096 * 8000 = 32 768 000, >> 15 -> 1000 exactement
  const int16_t src[] = {kSrcA};
  ringtone::set_source(src, 1);
  ringtone::set_volume(50);

  std::vector<int16_t> buf(1, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_EQ(1000, buf[0]);
}

TEST_F(Ringtone, HalfVolumeHalvesTheSynthesizedMelody) {
  std::vector<int16_t> buf(kMelodySamples, 0);
  ringtone::set_volume(50);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());

  const int16_t peak = peak_abs(buf, 0, buf.size());
  EXPECT_LE(peak, kAmplitude / 2);
  EXPECT_GT(peak, kAmplitude / 4);
}

TEST_F(Ringtone, FullScaleSourceStaysWithinTheMixHeadroomAtFullVolume) {
  // Un WAV normalisé à 0 dBFS ne doit PAS sortir à pleine échelle : 100 % est
  // plafonné au même cadrage que le ding-dong (kAmplitude), sans quoi la
  // sonnerie saturerait à chaque mix avec la voix du visiteur.
  const int16_t src[] = {32767, -32768};
  ringtone::set_source(src, 2);
  ringtone::set_volume(100);

  std::vector<int16_t> buf(2, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_LE(peak_abs(buf, 0, buf.size()), kAmplitude);
}

TEST_F(Ringtone, SourceSaturatesInsteadOfWrapping) {
  // Même garde-fou que pour la mélodie : sans sat_add, 32767 + 15999 se
  // replierait en négatif, soit un clic plein volume dans le haut-parleur.
  const int16_t src[] = {32767};
  ringtone::set_source(src, 1);
  std::vector<int16_t> buf(4, 32767);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  for (int16_t s : buf)
    EXPECT_EQ(32767, s);

  const int16_t neg[] = {-32768};
  ringtone::set_source(neg, 1);
  std::vector<int16_t> nbuf(4, -32768);
  ringtone::start();
  ringtone::mix_into(nbuf.data(), nbuf.size());
  for (int16_t s : nbuf)
    EXPECT_EQ(-32768, s);
}

TEST_F(Ringtone, VolumeAboveOneHundredIsClamped) {
  const int16_t src[] = {kSrcA};
  ringtone::set_source(src, 1);
  ringtone::set_volume(250);

  std::vector<int16_t> buf(1, 0);
  ringtone::start();
  ringtone::mix_into(buf.data(), buf.size());
  EXPECT_EQ(kMixA, buf[0]);
}

TEST_F(Ringtone, MicToneIgnoresTheSourceAndTheVolume) {
  // La tonalité de test reste un sinus d'amplitude connue : c'est ce qui rend
  // le VU-mètre de la page Debug interprétable.
  const int16_t src[] = {kSrcA, kSrcB, kSrcC};
  ringtone::set_source(src, 3);
  ringtone::set_volume(10);

  std::vector<int16_t> buf(kDingSamples, 0);
  ringtone::mic_tone_start();
  ringtone::mic_tone_mix_into(buf.data(), buf.size());
  EXPECT_GT(peak_abs(buf, 0, buf.size()), kMicToneAmplitude / 2);
}

} // namespace
