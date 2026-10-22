#pragma once

#include "datamanager/game_string.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace sf::datamanager {

// Copies the name of a TESFullName component through its virtual name getter.
// The getter returns a borrowed pointer into the string pool.
[[nodiscard]] bool ReadFullName(
    std::uintptr_t fullNameComponent,
    std::size_t getFullNameVtableOffset,
    std::size_t maximumLength,
    std::string& name) noexcept;

// Copies the text of a TESDescription component through its virtual text
// getter, which fills a game-allocated string that is freed with deallocate.
[[nodiscard]] bool ReadDescription(
    std::uintptr_t descriptionComponent,
    std::size_t getDescriptionVtableOffset,
    GameDeallocate deallocate,
    std::size_t maximumLength,
    std::string& description) noexcept;

}
