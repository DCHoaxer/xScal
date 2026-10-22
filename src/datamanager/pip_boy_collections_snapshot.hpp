#pragma once

#include "config/target_profile.hpp"
#include "platform/windows_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

struct ResolvedPipBoyCollectionsRuntime final {
    config::NativePipBoyCollectionsProfile layout{};
    std::uintptr_t localPlayerGlobal{};
    std::uintptr_t localPlayerPrimaryVtable{};
    std::uintptr_t collectionManagerGlobal{};
    std::uintptr_t getFormComponent{};
    std::uintptr_t getCollectionOwnedCount{};
    std::uintptr_t isCollectionGateOpen{};
    std::uintptr_t collectionGateObject{};
    std::uintptr_t getFixedStringText{};
    std::uintptr_t deallocate{};
};

struct PipBoyCollectible final {
    std::uint32_t formId{};
    std::string name;
    // Description with carriage returns replaced by spaces, as the
    // collections tab shows it.
    std::string description;
    std::uint32_t owned{};
    // 0 means no limit.
    std::uint32_t capacity{};
};

[[nodiscard]] bool ResolvePipBoyCollectionsRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyCollectionsProfile& collections,
    ResolvedPipBoyCollectionsRuntime& runtime) noexcept;

// Reads the collectibles in the collection manager's order, with the same
// form type and gated-category filter the collections tab applies.
[[nodiscard]] bool ReadPipBoyCollectibles(
    const ResolvedPipBoyCollectionsRuntime& runtime,
    std::vector<PipBoyCollectible>& collectibles) noexcept;

[[nodiscard]] bool BuildPipBoyCollectionsJson(
    const ResolvedPipBoyCollectionsRuntime& runtime,
    std::string& json) noexcept;

}
