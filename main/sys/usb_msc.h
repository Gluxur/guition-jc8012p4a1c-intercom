#pragma once

#include <cstdint>

// Mode clé USB : la carte microSD est exposée au PC en stockage de masse sur
// le port OTG High-Speed.
//
// Pourquoi un mode explicite et pas une exposition permanente : le PC obtient
// l'accès BLOC au support. Deux propriétaires simultanés du système de
// fichiers — l'ESP qui journalise et le PC qui écrit — corrompraient la FAT.
// L'entrée dans le mode suspend le journal SD et démonte proprement ; la
// sortie remonte et recharge la sonnerie.
//
// Aucun appel LVGL ici : ce module ne connaît pas l'interface. C'est ui_sdlog
// qui l'appelle depuis sa tâche disque, et ui_usb qui l'affiche.
namespace usb_msc {

enum class Status : uint8_t {
  kOff,    // fonctionnement normal, carte possédée par l'application
  kActive, // carte exposée au PC
  kError,  // sortie dont le remontage a échoué : plus en mode, sans la carte
};

// Entre en mode clé USB. À n'appeler QUE depuis la tâche disque de ui_sdlog :
// la séquence démonte un système de fichiers et installe un pilote USB, elle
// dure des centaines de millisecondes et ne doit jamais bloquer LVGL.
// En cas d'échec, l'appareil est ramené exactement à son état initial et le
// statut reste kOff — la cause est dans last_error().
[[nodiscard]] bool enter();

// Sort du mode, remonte la carte et recharge /sdcard/ring.wav. Même contrainte
// d'appel qu'enter(). Un échec de remontage laisse kError : c'est le seul cas
// qu'on ne sait pas défaire, et l'onglet Carte SD doit le montrer.
[[nodiscard]] bool leave();

[[nodiscard]] Status status();

// Cause du dernier échec, prête à afficher. "" si aucun.
[[nodiscard]] const char *last_error();

// Vrai quand un hôte USB est énuméré. C'EST le diagnostic du connecteur : la
// carte a trois USB-C dont un seul non sérigraphié, et rien ne prouve encore
// que ce soit l'OTG. Si ce drapeau reste faux câble branché, ce n'est pas le
// bon port.
[[nodiscard]] bool host_connected();

} // namespace usb_msc
