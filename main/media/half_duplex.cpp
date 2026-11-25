#include "media/half_duplex.h"

#include <atomic>

namespace half_duplex {
namespace {

// Fin de la fenêtre « visiteur parle », ms depuis le boot. 32 bits et non
// int64 : un atomique 64 bits n'est pas sans verrou sur ce RISC-V 32 bits.
std::atomic<uint32_t> s_far_until{0};
// Faux tant qu'aucune trame forte n'a été vue : sans lui, until = 0 au boot
// passerait pour une fenêtre ouverte une fois le compteur rebouclé.
std::atomic<bool> s_far_seen{false};

constexpr uint32_t kSamplesPerMs = 8;

} // namespace

int16_t peak(const int16_t *pcm, size_t n) {
  int32_t p = 0;
  for (size_t i = 0; i < n; i++) {
    const int32_t a = pcm[i] < 0 ? -static_cast<int32_t>(pcm[i]) : pcm[i];
    if (a > p) {
      p = a;
    }
  }
  return static_cast<int16_t>(p > 32767 ? 32767 : p);
}

bool before(uint32_t now_ms, uint32_t until_ms) {
  return static_cast<int32_t>(until_ms - now_ms) > 0;
}

void note_far(const int16_t *pcm, size_t n, uint32_t now_ms) {
  if (peak(pcm, n) < kFarThreshold) {
    return;
  }
  const uint32_t until =
      now_ms + static_cast<uint32_t>(n / kSamplesPerMs) + kHoldMs;
  s_far_until.store(until, std::memory_order_relaxed);
  s_far_seen.store(true, std::memory_order_release);
}

bool far_speaking(uint32_t now_ms) {
  if (!s_far_seen.load(std::memory_order_acquire)) {
    return false;
  }
  return before(now_ms, s_far_until.load(std::memory_order_relaxed));
}

void clear_far() {
  s_far_seen.store(false, std::memory_order_release);
  s_far_until.store(0, std::memory_order_relaxed);
}

void Ducker::apply(int16_t *pcm, size_t n, bool duck) {
  for (size_t i = 0; i < n; i++) {
    if (duck) {
      gain_ -= kAttackStep;
      if (gain_ < kDuckGainQ15) {
        gain_ = kDuckGainQ15;
      }
    } else {
      gain_ += kReleaseStep;
      if (gain_ > kUnityQ15) {
        gain_ = kUnityQ15;
      }
    }
    // |s| <= 32768 et gain <= 32768 : produit <= 2^30, tient en int32. Le
    // résultat ne dépasse jamais |s| : pas de saturation à gérer.
    pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * gain_) >> 15);
  }
}

} // namespace half_duplex
