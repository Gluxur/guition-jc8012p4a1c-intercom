#pragma once

namespace log_stream {

// publish : sink de batch de logs ; main le câble sur mqtt publish cfg::topic::kLog.
// Installe le tee dbg_log + crée la tâche de publication. À appeler après
// dbg_log::init() et mqtt_ctrl::init().
void init(bool (*publish)(const char *data, int len));

// Active/désactive le stream (OFF par défaut). Câblé sur le switch HA.
void set_enabled(bool en);

} // namespace log_stream
