#pragma once

// Force-inclus dans TOUTES les unités de compilation du projet de tests host
// (cf. test/CMakeLists.txt). Il ne comble qu'un écart de bibliothèque C, pas
// une dépendance matérielle : `strlcpy` est une fonction BSD, présente dans le
// newlib d'ESP-IDF mais absente de MinGW et des glibc < 2.38. Les sources
// applicatives l'utilisent partout pour les copies bornées — les réécrire en
// strncpy pour faire plaisir au host reviendrait à tester autre chose que ce
// qui tourne sur la carte.
//
// Le force-include plutôt qu'un `#include` ajouté aux sources : le firmware,
// lui, n'a rien à savoir de ce projet de tests.

#include <cstddef>
#include <cstring>

#ifndef HOST_HAS_STRLCPY
// Sémantique BSD : copie tronquée mais TOUJOURS terminée, retourne la longueur
// de `src` (donc >= size si troncature).
inline size_t strlcpy(char *dst, const char *src, size_t size) {
  const size_t len = std::strlen(src);
  if (size != 0) {
    const size_t n = (len < size - 1) ? len : size - 1;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
  }
  return len;
}
#endif
