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

// Text entity `pattern` sent to HA (ListEntitiesTextResponse field 10): HA
// refuses a non-matching text.set_value with an error, so a typo is visible
// instead of silently dropped. Must accept exactly what normalize() accepts
// (test/test_ha_entity_id: PatternAgreesWithNormalize). Anchored at both ends:
// HA uses re.match, which anchors only the start. Length is bounded by the
// entity's max_length, not here.
inline constexpr const char *kCoverPattern = "^ *(cover\\.[a-z0-9_]+)? *$";
inline constexpr const char *kWeatherPattern = "^ *(weather\\.[a-z0-9_]+)? *$";
inline constexpr const char *kCalendarPattern = "^ *(calendar\\.[a-z0-9_]+)? *$";

} // namespace ha_entity_id
