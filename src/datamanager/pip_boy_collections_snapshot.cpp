#include "datamanager/pip_boy_collections_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/form_text.hpp"
#include "datamanager/game_string.hpp"
#include "datamanager/local_player.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

namespace sf::datamanager {
namespace {

constexpr std::uint32_t kMaximumCollectibles = 4096;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumDescriptionLength = 4096;
constexpr std::size_t kMaximumCollectionsJsonBytes = 2 * 1024 * 1024;

using GetFormComponent = void*(__fastcall*)(void* form, std::uint32_t signature);
using GetCollectionOwnedCount = std::uint32_t(__fastcall*)(
    void* player,
    std::uint32_t formId);
using IsCollectionGateOpen = bool(__fastcall*)(void* gate);
using GetFixedStringText = const char*(__fastcall*)(const void* fixedString);

template <typename T>
[[nodiscard]] bool ReadField(
    std::uintptr_t object,
    std::size_t offset,
    T& value) noexcept {
    std::uintptr_t address{};
    return platform::TryAddAddress(object, offset, address) &&
        platform::TryCopyBytes(
            reinterpret_cast<const void*>(address), &value, sizeof(value));
}

[[nodiscard]] bool CallGetFormComponent(
    std::uintptr_t function,
    std::uintptr_t form,
    std::uint32_t signature,
    std::uintptr_t& component) noexcept {
    __try {
        component = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<GetFormComponent>(function)(
                reinterpret_cast<void*>(form), signature));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        component = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetCollectionOwnedCount(
    std::uintptr_t function,
    std::uintptr_t player,
    std::uint32_t formId,
    std::uint32_t& owned) noexcept {
    __try {
        owned = reinterpret_cast<GetCollectionOwnedCount>(function)(
            reinterpret_cast<void*>(player), formId);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        owned = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallIsCollectionGateOpen(
    std::uintptr_t function,
    std::uintptr_t gate,
    bool& open) noexcept {
    __try {
        open = reinterpret_cast<IsCollectionGateOpen>(function)(
            reinterpret_cast<void*>(gate));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        open = false;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetFixedStringText(
    std::uintptr_t function,
    std::uintptr_t fixedString,
    const char*& text) noexcept {
    __try {
        text = reinterpret_cast<GetFixedStringText>(function)(
            reinterpret_cast<const void*>(fixedString));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        text = nullptr;
        return false;
    }
    return text != nullptr;
}

enum class CollectibleReadResult {
    Read,
    // Not a collectible form, or a gated category while the gate is closed.
    Skipped,
    Failed,
};

[[nodiscard]] CollectibleReadResult ReadCollectible(
    const ResolvedPipBoyCollectionsRuntime& runtime,
    std::uintptr_t player,
    std::uintptr_t form,
    bool gateOpen,
    PipBoyCollectible& collectible) noexcept {
    const auto& layout = runtime.layout;
    std::uint8_t formType{};
    std::uint16_t category{};
    if (!ReadField(form, layout.formTypeOffset, formType)) {
        return CollectibleReadResult::Failed;
    }
    if (formType != layout.collectionFormType) {
        return CollectibleReadResult::Skipped;
    }
    if (!ReadField(form, layout.collectionCategoryOffset, category)) {
        return CollectibleReadResult::Failed;
    }
    if (category == layout.collectionGatedCategory && !gateOpen) {
        return CollectibleReadResult::Skipped;
    }

    std::uintptr_t description{};
    std::uintptr_t nameString{};
    const char* name{};
    if (!ReadField(form, layout.formIdOffset, collectible.formId) ||
        !ReadField(form, layout.collectionCapacityOffset, collectible.capacity) ||
        !CallGetFormComponent(
            runtime.getFormComponent,
            form,
            static_cast<std::uint32_t>(layout.collectionDescriptionSignature),
            description) ||
        (description != 0 &&
         !ReadDescription(
             description,
             layout.getDescriptionVtableOffset,
             reinterpret_cast<GameDeallocate>(runtime.deallocate),
             kMaximumDescriptionLength,
             collectible.description)) ||
        !platform::TryAddAddress(form, layout.collectionNameOffset, nameString) ||
        !CallGetFixedStringText(runtime.getFixedStringText, nameString, name) ||
        !platform::CopyBoundedCString(
            name, kMaximumNameLength, true, collectible.name) ||
        !CallGetCollectionOwnedCount(
            runtime.getCollectionOwnedCount,
            player,
            collectible.formId,
            collectible.owned)) {
        return CollectibleReadResult::Failed;
    }
    std::replace(
        collectible.description.begin(), collectible.description.end(), '\r', ' ');
    return CollectibleReadResult::Read;
}

}

constexpr std::array kPipBoyCollectionsRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, localPlayerGlobal),
        offsetof(ResolvedPipBoyCollectionsRuntime, localPlayerGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, localPlayerPrimaryVtable),
        offsetof(ResolvedPipBoyCollectionsRuntime, localPlayerPrimaryVtable),
        platform::AddressCheck::None),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, collectionManagerGlobal),
        offsetof(ResolvedPipBoyCollectionsRuntime, collectionManagerGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, getFormComponent),
        offsetof(ResolvedPipBoyCollectionsRuntime, getFormComponent),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, getCollectionOwnedCount),
        offsetof(ResolvedPipBoyCollectionsRuntime, getCollectionOwnedCount),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, isCollectionGateOpen),
        offsetof(ResolvedPipBoyCollectionsRuntime, isCollectionGateOpen),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, collectionGateObject),
        offsetof(ResolvedPipBoyCollectionsRuntime, collectionGateObject),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, getFixedStringText),
        offsetof(ResolvedPipBoyCollectionsRuntime, getFixedStringText),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyCollectionsProfile, deallocate),
        offsetof(ResolvedPipBoyCollectionsRuntime, deallocate),
        platform::AddressCheck::Executable),
};

bool ResolvePipBoyCollectionsRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyCollectionsProfile& collections,
    ResolvedPipBoyCollectionsRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        collections.collectionFormsOffset == 0 ||
        collections.collectionFormCountOffset == 0 ||
        collections.collectionFormType == 0 ||
        collections.collectionFormType > 0xFF ||
        collections.collectionNameOffset == 0 ||
        collections.collectionCapacityOffset == 0 ||
        collections.collectionCategoryOffset == 0 ||
        collections.collectionGatedCategory > 0xFFFF ||
        collections.collectionDescriptionSignature == 0 ||
        collections.collectionDescriptionSignature > 0xFFFFFFFF ||
        collections.formIdOffset == 0 ||
        collections.formTypeOffset == 0 ||
        collections.getDescriptionVtableOffset == 0 ||
        !platform::ValidateVtable(
            virtualQuery, moduleBase, collections.localPlayerPrimaryVtable)) {
        return false;
    }

    ResolvedPipBoyCollectionsRuntime candidate{
        .layout = collections,
    };
    if (!platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &collections,
            &candidate,
            kPipBoyCollectionsRuntimeBindings)) {
        return false;
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoyCollectibles(
    const ResolvedPipBoyCollectionsRuntime& runtime,
    std::vector<PipBoyCollectible>& collectibles) noexcept {
    collectibles.clear();
    const auto& layout = runtime.layout;
    std::uintptr_t player{};
    std::uintptr_t manager{};
    if (!ReadLocalPlayer(
            runtime.localPlayerGlobal,
            runtime.localPlayerPrimaryVtable,
            player) ||
        !platform::TryCopyBytes(
            reinterpret_cast<const void*>(runtime.collectionManagerGlobal),
            &manager,
            sizeof(manager))) {
        return false;
    }
    // The manager exists once the game data is loaded.
    if (manager == 0) {
        return true;
    }

    std::uintptr_t forms{};
    std::uint32_t count{};
    bool gateOpen{};
    if (!ReadField(manager, layout.collectionFormsOffset, forms) ||
        !ReadField(manager, layout.collectionFormCountOffset, count) ||
        count > kMaximumCollectibles || (count != 0 && forms == 0) ||
        !CallIsCollectionGateOpen(
            runtime.isCollectionGateOpen, runtime.collectionGateObject, gateOpen)) {
        return false;
    }

    std::vector<PipBoyCollectible> result;
    try {
        result.reserve(count);
    } catch (...) {
        return false;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uintptr_t form{};
        if (!ReadField(forms, index * sizeof(std::uintptr_t), form)) {
            return false;
        }
        if (form == 0) {
            continue;
        }
        PipBoyCollectible collectible;
        switch (ReadCollectible(runtime, player, form, gateOpen, collectible)) {
            case CollectibleReadResult::Read:
                result.push_back(std::move(collectible));
                break;
            case CollectibleReadResult::Skipped:
                break;
            case CollectibleReadResult::Failed:
                return false;
        }
    }

    collectibles = std::move(result);
    return true;
}

bool BuildPipBoyCollectionsJson(
    const ResolvedPipBoyCollectionsRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    std::vector<PipBoyCollectible> collectibles;
    if (!ReadPipBoyCollectibles(runtime, collectibles)) {
        return false;
    }

    try {
        json.reserve(32 + collectibles.size() * 192);
        json += R"({"collections":[)";
        for (std::size_t index = 0; index < collectibles.size(); ++index) {
            const auto& collectible = collectibles[index];
            if (index != 0) {
                json.push_back(',');
            }
            json += R"({"name":)";
            bool written =
                json::AppendString(collectible.name, json, kMaximumCollectionsJsonBytes);
            json += R"(,"formId":)";
            written = written &&
                json::AppendInteger(
                    collectible.formId, json, kMaximumCollectionsJsonBytes);
            json += R"(,"owned":)";
            written = written &&
                json::AppendInteger(
                    collectible.owned, json, kMaximumCollectionsJsonBytes);
            json += R"(,"capacity":)";
            written = written &&
                json::AppendInteger(
                    collectible.capacity, json, kMaximumCollectionsJsonBytes);
            json += R"(,"description":)";
            written = written &&
                json::AppendString(
                    collectible.description, json, kMaximumCollectionsJsonBytes);
            if (!written) {
                json.clear();
                return false;
            }
            json.push_back('}');
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumCollectionsJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
