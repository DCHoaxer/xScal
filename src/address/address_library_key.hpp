#pragma once

#include <cstddef>
#include <cstdint>

namespace sf::address {

// The build supplies the key as 32 comma-separated byte literals in the
// XSCAL_ADDRESS_LIBRARY_KEY definition. A build without it cannot read any
// database.
#ifdef XSCAL_ADDRESS_LIBRARY_KEY
inline constexpr bool kHasLibraryKey = true;
inline constexpr std::uint8_t kLibraryKey[32]{XSCAL_ADDRESS_LIBRARY_KEY};
#else
inline constexpr bool kHasLibraryKey = false;
inline constexpr std::uint8_t kLibraryKey[32]{};
#endif

}  // namespace sf::address
