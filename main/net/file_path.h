#pragma once

#include <cstddef>

// Assainissement des chemins reçus dans une query string HTTP.
//
// Module PUR : aucun header de driver, aucune IO. C'est ce qui le rend
// compilable et testable sur PC (test/), là où la barrière entre une chaîne
// venue du réseau et un fopen() doit être couverte exhaustivement — et non
// vérifiée par cycles flash/monitor.
namespace file_path {

// Décode les séquences %XX de `raw`, écrit le résultat dans `out` et valide :
//   - le chemin commence par cfg::sd::kMountPoint, suivi de '\0' ou '/'
//     (un préfixe qui ressemble, "/sdcardX", est refusé) ;
//   - aucun segment n'est exactement ".." ;
//   - aucun caractère de contrôle ni '\\' ;
//   - le résultat tient dans out_cap, NUL compris.
// Le décodage ne fait qu'UN tour : un "%252e" produit le texte "%2e", jamais
// un point. C'est ce qui bloque le double encodage.
[[nodiscard]] bool sanitize(const char *raw, char *out, size_t out_cap);

} // namespace file_path
