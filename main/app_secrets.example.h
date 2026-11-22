#pragma once

// Optional compiled-in defaults for a single installation.
//
// Copy this file to `main/app_secrets.h` (ignored by git) and fill it in if you
// want a freshly flashed board to join your network without touching the
// screen. Every field is optional: anything left out stays empty and must be
// entered on the Settings page (or provisioned with tools/provision_nvs.ps1).
//
// Values stored in NVS always win over these defaults. Home Assistant needs no
// credential here: it discovers the board as an ESPHome device and provisions
// the API encryption key itself.

#define APP_SECRET_WIFI_SSID "my-wifi"
#define APP_SECRET_WIFI_PASS "my-wifi-password"

// Machine running go2rtc and Home Assistant.
#define APP_SECRET_HOST "192.168.1.10"

// Home Assistant entities shown on the weather and agenda pages (Settings >
// Entities in HA). Defaults: weather.forecast_home (the Met.no integration's
// default) and calendar.home.
#define APP_SECRET_HA_WEATHER_ENTITY "weather.forecast_home"
#define APP_SECRET_HA_CALENDAR_ENTITY "calendar.home"
