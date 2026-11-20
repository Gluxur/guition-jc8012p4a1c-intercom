#pragma once

#include "sys/mqtt_ctrl.h"

// Catalogue des entités Home Assistant exposées en MQTT.
//
// Pourquoi ce module est dans app/ et non dans sys/ : il doit lire le BSP
// (luminosité), prefs, call_history, camera_session, sys_stats, file_server et
// déclencher ota_ctrl. app/ est la couche autorisée à connaître tout le monde ;
// le placer dans sys/ aurait exigé d'injecter une quinzaine de pointeurs de
// fonctions pour éviter qu'une couche basse dépende des couches hautes.
namespace ha_entities {

// Seule dépendance qui ne peut PAS être résolue ici : l'ouverture de l'écran
// caméra vit dans main.cpp (elle mêle ui_camera_v2, ringtone, call_history et
// audio_pipeline, et son comportement dépend de l'état de la session micro).
struct Hooks {
  void (*ring)(); // même chemin que la sonnette MQTT
};

// Construit la table et démarre la tâche de publication. À appeler AVANT
// mqtt_ctrl::init : c'est ce module qui fournit les callbacks du transport, et
// Home Assistant republie ses messages retenus dès l'abonnement — un catalogue
// pas encore construit les perdrait.
void init(const Hooks &hooks);

// Publie tout de suite l'état d'une entité (object_id du catalogue), sans
// attendre la republication de 30 s. Sans effet hors connexion : la
// republication suivante rattrapera. Appelable depuis n'importe quelle tâche.
void publish_now(const char *object_id);

// Callbacks à passer à mqtt_ctrl::init(). Référence stable (objet statique).
[[nodiscard]] const mqtt_ctrl::Callbacks &callbacks();

} // namespace ha_entities
