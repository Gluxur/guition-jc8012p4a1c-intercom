#include "motion_detect.h"

#include <cstdlib>

namespace motion {

void reduce_raw10(const uint8_t *frame, int width, int height, int row_step,
                  Grid &out) {
  // Statiques et non sur la pile : 7,7 Ko, plus que ne supporte raisonnablement
  // une tâche de fond. Appelant unique (la tâche presence), cf. le header.
  static uint32_t acc[kGridH][kGridW];
  static uint32_t cnt[kGridH][kGridW];
  for (int cy = 0; cy < kGridH; cy++) {
    for (int cx = 0; cx < kGridW; cx++) {
      acc[cy][cx] = 0;
      cnt[cy][cx] = 0;
    }
  }
  const int stride = width * 10 / 8;
  const int groups = width / 4;
  if (row_step < 1) {
    row_step = 1;
  }

  for (int y = 0; y < height; y += row_step) {
    const int cy = y * kGridH / height;
    const uint8_t *row = frame + static_cast<size_t>(y) * stride;
    for (int g = 0; g < groups; g += 2) {
      const int cx = (g * 4) * kGridW / width;
      const uint8_t *p = row + g * 5;
      acc[cy][cx] += p[0] + p[1] + p[2] + p[3];
      cnt[cy][cx] += 4;
    }
  }

  uint32_t sum = 0;
  for (int cy = 0; cy < kGridH; cy++) {
    for (int cx = 0; cx < kGridW; cx++) {
      const uint32_t v = cnt[cy][cx] ? acc[cy][cx] / cnt[cy][cx] : 0;
      out.cell[cy][cx] = static_cast<uint8_t>(v);
      sum += v;
    }
  }
  out.mean = static_cast<uint8_t>(sum / (kGridW * kGridH));
}

Diff compare(const Grid &prev, const Grid &cur, int threshold) {
  const int dm = static_cast<int>(cur.mean) - static_cast<int>(prev.mean);
  Diff d{0, 0};
  for (int cy = 0; cy < kGridH; cy++) {
    for (int cx = 0; cx < kGridW; cx++) {
      const int delta = std::abs(static_cast<int>(cur.cell[cy][cx]) -
                                 static_cast<int>(prev.cell[cy][cx]) - dm);
      if (delta > d.max_delta) {
        d.max_delta = delta;
      }
      if (delta > threshold) {
        d.changed++;
      }
    }
  }
  return d;
}

Exposure next_exposure(const Exposure &cur, uint8_t mean, uint8_t target,
                       uint8_t black_level, uint32_t max_lines,
                       uint32_t min_lines, uint32_t max_gain_x16) {
  // Signal utile = niveau mesuré moins le piédestal du noir (0x4003 = 64 en
  // RAW10, soit 16 en 8 bits) : sans ce retrait, une image noire semblerait
  // déjà à 16/target de la consigne et l'AE sous-corrigerait.
  const int sig = static_cast<int>(mean) - black_level;
  const int want = static_cast<int>(target) - black_level;
  if (want <= 0) {
    return cur;
  }
  // Rapport voulu/mesuré en 1/256, borné x0,5 .. x2.
  int ratio256 = sig <= 0 ? 512 : (want * 256) / sig;
  if (ratio256 > 512) {
    ratio256 = 512;
  }
  if (ratio256 < 128) {
    ratio256 = 128;
  }
  if (ratio256 > 218 && ratio256 < 294) { // bande morte +-15 %
    return cur;
  }

  // Produit pose x gain (en lignes x1/16), réparti pose d'abord : à gain égal
  // une pose plus longue a moins de bruit, et à 5 img/s on a ~195 ms de
  // marge avant le flou de bougé qui gênerait.
  const uint64_t prod = static_cast<uint64_t>(cur.lines) * cur.gain_x16;
  uint64_t want_prod = (prod * static_cast<uint64_t>(ratio256)) / 256;
  const uint64_t min_prod = static_cast<uint64_t>(min_lines) * 16;
  const uint64_t max_prod = static_cast<uint64_t>(max_lines) * max_gain_x16;
  if (want_prod < min_prod) {
    want_prod = min_prod;
  }
  if (want_prod > max_prod) {
    want_prod = max_prod;
  }

  Exposure out;
  if (want_prod <= static_cast<uint64_t>(max_lines) * 16) {
    out.gain_x16 = 16;
    out.lines = static_cast<uint32_t>(want_prod / 16);
  } else {
    out.lines = max_lines;
    out.gain_x16 = static_cast<uint32_t>(want_prod / max_lines);
  }
  if (out.lines < min_lines) {
    out.lines = min_lines;
  }
  if (out.gain_x16 > max_gain_x16) {
    out.gain_x16 = max_gain_x16;
  }
  return out;
}

} // namespace motion
