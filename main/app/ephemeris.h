#pragma once

// Éphéméride : saint du jour du calendrier civil français, table compilée dans
// le firmware. Aucun accès réseau ni NVS — c'est une donnée qui ne change
// jamais, la faire dépendre de Home Assistant coûterait une entité et un fetch
// pour zéro information supplémentaire. Logique pure : testée sur host (test/).
namespace ephemeris {

// `mon` et `mday` sont pris DIRECTEMENT de struct tm : mon 0-11, mday 1-31.
// Renvoie une chaîne UTF-8 statique ("St Armel", "Assomption"...), "" si la
// date est hors bornes. Le 29 février a sa propre entrée ; l'index est calculé
// sur une disposition d'année bissextile, donc mars-décembre tombent sur le
// même saint que l'année soit bissextile ou non (pas d'année en paramètre).
const char *saint_of_day(int mon, int mday);

} // namespace ephemeris
