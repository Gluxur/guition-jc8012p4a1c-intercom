#pragma once

#include <cstddef>

// Validation of an entity id typed into a text entity on the HA device page.
// Pure, host-tested (test/test_ha_entity_id).
namespace ha_entity_id {

// Buffer size for a stored id, NUL included: ids are < kMax characters.
// 128 and not 64: integration-generated ids get long (a Meteo-France entity
// measured 94 characters).
inline constexpr size_t kMax = 128;

// Trims ASCII spaces. Empty result -> out = "" and true (the entity is unset).
// Otherwise true iff the value is "<domain>.<object>" with a non-empty
// [a-z0-9_] object and fewer than kMax characters. HA ids are lowercase, so
// "Cover.X" is a typo, not an id. On false `out` is left untouched. `cap`
// must be >= kMax.
[[nodiscard]] bool normalize(const char *in, const char *domain, char *out,
                             size_t cap);

} // namespace ha_entity_id
