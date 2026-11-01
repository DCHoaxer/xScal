#pragma once

#include <cstddef>
#include <string_view>

namespace sf {

struct MovieRootContext;
struct ScaleformValue;

// Game.* calls Game.GetAS3Object accepts. Each has one cached object per movie.
inline constexpr std::size_t kGameObjectSlotCount = 8;

// Writes an owned reference to the ActionScript object built from json into
// out, created in context's movie. slot is the Game.* call json came from.
// When the movie's object for slot was built from the same json, out
// references that object and json is not parsed. Returns false when json is
// not a JSON object or the object cannot be created. Callers must treat the
// object as read-only, because later calls can return it again.
[[nodiscard]] bool ReadGameObject(
    const MovieRootContext& context,
    std::size_t slot,
    std::string_view json,
    ScaleformValue& out) noexcept;

// Releases the objects cached for movieRoot. The MovieRoot destructor hook
// calls this before the movie is destroyed.
void ReleaseGameObjects(void* movieRoot) noexcept;

// Objects are cached only while the MovieRoot destructor hook is installed,
// because nothing else releases them before their movie is destroyed.
void SetGameObjectCacheAvailable(bool available) noexcept;

}  // namespace sf
