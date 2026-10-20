#pragma once

#include "config/target_profile.hpp"

#include <cstdint>

namespace sf::archive {

// Totals since the process started.
struct ArchiveStreamTotals final {
    // Compressed entries decompressed with libdeflate, their unpacked size, the
    // time spent reading and decompressing them, and the Read calls answered
    // from those entries.
    std::uint64_t entries{};
    std::uint64_t bytes{};
    std::uint64_t microseconds{};
    std::uint64_t bufferedReads{};
    // Entries handed back to the game's zlib because the archive read or the
    // decompression failed.
    std::uint64_t fallbacks{};
};

// Replaces the Read, Seek and Close slots of the archive stream vtable when all
// three hold the game's functions.
//
// The first Read or forward Seek on a new zlib stream then reads the packed
// entry, decompresses it whole with libdeflate and serves that and every later
// call on the stream from the buffer. Close frees the buffer. Other streams and
// any failed read or decompression stay with the game's functions.
[[nodiscard]] bool InstallArchiveStreamHook(
    const config::ArchiveStreamProfile& profile,
    std::uintptr_t moduleBase) noexcept;

[[nodiscard]] ArchiveStreamTotals ReadArchiveStreamTotals() noexcept;

// The buffered path without the install state, for tests. Each returns the
// profile's unsupportedResult when the stream would go to the game's function.
void SetArchiveStreamLayoutForTest(const config::ArchiveStreamProfile& profile) noexcept;
[[nodiscard]] std::uint32_t ReadBufferedForTest(
    void* stream, void* buffer, std::uint64_t length, std::uint64_t* bytesRead) noexcept;
[[nodiscard]] std::uint32_t SeekBufferedForTest(
    void* stream, std::int64_t offset, std::uint64_t* newPosition) noexcept;
void CloseBufferedForTest(void* stream) noexcept;

}
