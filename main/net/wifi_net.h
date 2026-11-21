#pragma once

namespace wifi_net {

/* Initialise netif/event loop/Wi-Fi (station via esp_hosted), force
 * WIFI_PS_NONE (le power-save cause des pertes RTP massives) et attend une IP.
 * À appeler une seule fois au boot.
 *
 * Attente BORNÉE à 60 s : au-delà, renvoie false et rend la main. Le driver
 * continue ses tentatives en arrière-plan (handler STA_DISCONNECTED), donc
 * l'appelant DOIT poursuivre son initialisation — l'API, OTA et net_wdt sont
 * les seules voies de récupération à distance, et à portMAX_DELAY un SSID
 * erroné saisi sur la page Configuration les empêchait toutes de démarrer. */
bool connect_blocking(const char *ssid, const char *pass);

/* RSSI courant du lien station (dBm), ou 0 si indisponible. Léger : lecture
 * d'un cache driver (esp_wifi_sta_get_ap_info), sûr à appeler périodiquement
 * hors chemin chaud. Diagnostic débit MJPEG (TCP s'effondre sur perte 2.4GHz). */
int wifi_rssi(void);

/* True entre IP_EVENT_STA_GOT_IP et WIFI_EVENT_STA_DISCONNECTED. Sonde
 * d'état pure : ne déclenche aucun trafic, lisible depuis n'importe quelle
 * tâche. Utilisée par l'indicateur de l'écran météo et par sys/net_wdt. */
[[nodiscard]] bool link_up(void);

} // namespace wifi_net
