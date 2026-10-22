#pragma once

#include <cstdint>

namespace sf::datamanager {

// Reads the local-player global and accepts the pointer only when the object's
// primary vtable is the expected LocalPlayerCharacter vtable. Both arguments
// are absolute addresses resolved at install time.
[[nodiscard]] bool ReadLocalPlayer(
    std::uintptr_t localPlayerGlobal,
    std::uintptr_t localPlayerPrimaryVtable,
    std::uintptr_t& player) noexcept;

}
