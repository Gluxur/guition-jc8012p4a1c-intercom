#pragma once

// Watchdog de CONNECTIVITÉ (à ne pas confondre avec esp_task_wdt, qui surveille
// la famine CPU). Une tâche lente compare l'ancienneté du dernier « lien vu
// vivant » à un plafond, pour le WiFi et pour MQTT séparément, et redémarre la
// carte au-delà. Les deux plafonds diffèrent volontairement (5 min / 30 min) :
// un Wi-Fi muet est une panne LOCALE qu'un redémarrage corrige, un broker muet
// est une panne DISTANTE qu'il ne corrige pas.
//
// Pourquoi : le visiophone est mural et sans clavier. Un lien perdu que ni le
// retry esp_wifi_connect ni la reconnexion interne d'esp-mqtt ne rattrapent
// rend la carte inutile ET injoignable par OTA — seul un redémarrage la sort
// de là.
namespace net_wdt {

// busy_probe : renvoie true tant qu'un redémarrage serait destructeur (flux
// caméra ou micro en cours, OTA en écriture). Injecté plutôt qu'appelé en dur
// pour garder sys/ indépendant de app/ (même patron qu'audio_pipeline::init).
// Peut être nullptr : le watchdog ne gèle alors jamais.
//
// on_healthy : appelé à chaque tick où MQTT est connecté (peut être nullptr).
// Sert à la validation OTA, qui ne doit PAS dépendre d'une unique fenêtre de
// 30 s au boot : un broker qui revient à t=35 s laissait sinon l'image en
// PENDING_VERIFY, et le prochain reboot quelconque — coupure, panique, des
// jours plus tard — la déclarait invalide et rebootait sur l'ancienne.
//
// À appeler en FIN de app_main, après mqtt_ctrl::init : démarré plus tôt, le
// temps de boot consommerait lui-même le délai.
void init(bool (*busy_probe)(), void (*on_healthy)());

} // namespace net_wdt
