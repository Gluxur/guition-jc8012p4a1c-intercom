#pragma once

#include <cstdint>

// Marqueurs de diagnostic qui survivent à un reset (mémoire LP, RTC_NOINIT).
// Un blocage matériel (reset=WDT) ne laisse ni panic ni trace, et le journal
// SD perd ses ~2 dernières secondes, encore dans le tampon : relus au boot
// suivant, ces marqueurs disent quelle étape a été atteinte en dernier.
//
// Un horodatage par étape (dernière fois vue), pas un historique : un store
// 32 bits aligné, sans verrou, sûr depuis n'importe quelle tâche et sur le
// chemin chaud, et les étapes vues à chaque frame n'effacent pas un événement
// rare comme l'appui sur le portail. Étape *Begin plus récente que son *End =
// la puce s'est arrêtée dedans.
namespace crumb {

enum class Id : uint8_t {
  kGatePress,
  kGateQueued,
  kApiActionSend,
  kApiActionSent,
  kGateDone,
  kBandComposeBegin,
  kBandComposeEnd,
  kBandBlitBegin,
  kBandBlitEnd,
  kJpegDecodeBegin,
  kJpegDecodeEnd,
  kPresentBegin,
  kPresentEnd,
  kPresenceCamStop,
  kPresenceCaptureBegin,
  kPresenceCaptureEnd,
  kPresenceWriteBegin,
  kPresenceWriteEnd,
  kSdLogWriteBegin,
  kSdLogWriteEnd,
  kBandSnapDone,
  kBandMsyncBegin,
  kCamStop,
  kCamHistoryBegin,
  kCamHistoryEnd,
  kVideoStopBegin,
  kVideoStopEnd,
  kCamPeerClose,
  kCamTeardownEnd,
  kMicStop,
  kMicSendJoined,
  kMicPeerClose,
  kMicTeardownEnd,
  kAecBegin,
  kAecEnd,
  kAecStop,
  kMicReadBegin,
  kMicReadEnd,
  kMicSendBegin,
  kMicSendEnd,
  kCount
};

// Horodate l'étape (ms depuis le boot) et retient `arg`. Pas depuis une ISR.
void mark(Id id, uint32_t arg = 0);

// Au boot, après sd_log::start() : journalise les marqueurs laissés par le
// démarrage précédent si le reset n'était pas volontaire, puis les remet à
// zéro. À appeler une fois.
void report_previous();

} // namespace crumb
