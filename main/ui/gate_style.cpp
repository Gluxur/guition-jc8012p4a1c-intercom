#include "gate_style.h"

#include "theme.h"

namespace ui {

uint32_t gate_tone_color(gate_logic::Tone t) {
  switch (t) {
  case gate_logic::Tone::kOpen:
    return theme::active.gate_open;
  case gate_logic::Tone::kClose:
    return theme::active.gate_close;
  case gate_logic::Tone::kStop:
    return theme::active.gate_stop;
  case gate_logic::Tone::kOff:
    break;
  }
  return theme::active.gate_off;
}

} // namespace ui
