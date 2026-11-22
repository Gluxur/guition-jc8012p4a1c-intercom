#pragma once

#include <cstdint>

#include "app/gate_logic.h"

namespace ui {

// Couleur de fond du bouton portail pour la palette active.
[[nodiscard]] uint32_t gate_tone_color(gate_logic::Tone t);

} // namespace ui
