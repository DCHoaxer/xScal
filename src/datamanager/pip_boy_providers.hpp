#pragma once

#include "config/target_profile.hpp"
#include "datamanager/json_result_cache.hpp"
#include "datamanager/pip_boy_collections_snapshot.hpp"
#include "datamanager/pip_boy_inventory_provider.hpp"
#include "datamanager/pip_boy_inventory_snapshot.hpp"
#include "datamanager/pip_boy_perks_snapshot.hpp"
#include "datamanager/pip_boy_player_info_snapshot.hpp"
#include "datamanager/pip_boy_quests_snapshot.hpp"
#include "datamanager/pip_boy_special_snapshot.hpp"
#include "datamanager/pip_boy_status_snapshot.hpp"
#include "datamanager/time_data_snapshot.hpp"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace sf::datamanager {

// A Game.* provider that resolves its runtime once at install and builds
// the JSON from it on request. Slice names the runtime type and supplies
// static Resolve and Build functions.
template <typename Slice>
class PipBoySliceProvider final {
public:
    explicit PipBoySliceProvider(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept
        : virtualQuery_(virtualQuery), cache_(cacheTick) {}

    PipBoySliceProvider(const PipBoySliceProvider&) = delete;
    PipBoySliceProvider& operator=(const PipBoySliceProvider&) = delete;

    [[nodiscard]] bool Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept {
        typename Slice::Runtime resolvedRuntime{};
        if (!Slice::Resolve(
                virtualQuery_, moduleBase, profile, resolvedRuntime)) {
            return false;
        }

        std::unique_lock lock{lifecycleMutex_};
        if (installed_) {
            return false;
        }

        runtime_ = resolvedRuntime;
        installed_ = true;
        return true;
    }

    [[nodiscard]] bool Restore() noexcept {
        std::unique_lock lock{lifecycleMutex_};
        if (!installed_) {
            return false;
        }

        installed_ = false;
        cache_.Clear();
        runtime_ = {};
        return true;
    }

    [[nodiscard]] bool IsActive() const noexcept {
        std::shared_lock lock{lifecycleMutex_};
        return installed_;
    }

    [[nodiscard]] GameProviderRequestStatus Request(
        std::string& json) noexcept {
        json.clear();

        std::shared_lock lifecycleLock{lifecycleMutex_};
        if (!installed_) {
            return GameProviderRequestStatus::Inactive;
        }
        if (cache_.TryCopyFresh(json)) {
            return GameProviderRequestStatus::Delivered;
        }

        if (!Slice::Build(runtime_, json)) {
            return GameProviderRequestStatus::SnapshotUnavailable;
        }
        cache_.Store(json);
        return GameProviderRequestStatus::Delivered;
    }

private:
    platform::VirtualQueryFn virtualQuery_{};
    mutable std::shared_mutex lifecycleMutex_;
    typename Slice::Runtime runtime_{};
    bool installed_{};
    JsonResultCache cache_;
};

struct PipBoySpecialSlice final {
    using Runtime = ResolvedPipBoySpecialRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolvePipBoySpecialRuntime(
            virtualQuery,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoySpecial,
            runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildPipBoySpecialJson(runtime, json);
    }
};

struct PipBoyPlayerInfoSlice final {
    using Runtime = ResolvedPipBoyPlayerInfoRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolvePipBoyPlayerInfoRuntime(
            virtualQuery,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoyPlayerInfo,
            runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildPipBoyPlayerInfoJson(runtime, json);
    }
};

struct PipBoyPerksSlice final {
    using Runtime = ResolvedPipBoyPerksRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolvePipBoyPerksRuntime(
            virtualQuery, moduleBase, profile.dataManager.pipBoyPerks, runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildPipBoyPerksJson(runtime, json);
    }
};

struct PipBoyQuestsSlice final {
    using Runtime = ResolvedPipBoyQuestsRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolvePipBoyQuestsRuntime(
            virtualQuery, moduleBase, profile.dataManager.pipBoyQuests, runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildPipBoyQuestsJson(runtime, json);
    }
};

struct PipBoyCollectionsSlice final {
    using Runtime = ResolvedPipBoyCollectionsRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolvePipBoyCollectionsRuntime(
            virtualQuery,
            moduleBase,
            profile.dataManager.pipBoyCollections,
            runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildPipBoyCollectionsJson(runtime, json);
    }
};

// The STAT page also lists the equipped weapons' damage, which comes from
// the inventory reader. Status installs without it and then writes
// "weaponDamage":null.
struct PipBoyStatusSlice final {
    struct Runtime final {
        ResolvedPipBoyStatusRuntime status;
        ResolvedPipBoyInventoryRuntime inventory;
        bool inventoryResolved;
    };

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept;
    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept;
};

// Game hour and minute for xsGetTimeData and Game.Clock.
struct PipBoyTimeSlice final {
    using Runtime = ResolvedNativeTimeDataRuntime;

    [[nodiscard]] static bool Resolve(
        platform::VirtualQueryFn virtualQuery,
        std::uintptr_t moduleBase,
        const TargetProfile& profile,
        Runtime& runtime) noexcept {
        return ResolveNativeTimeDataRuntime(
            virtualQuery, moduleBase, profile.dataManager.timeData, runtime);
    }

    [[nodiscard]] static bool Build(
        const Runtime& runtime,
        std::string& json) noexcept {
        return BuildNativeTimeDataJson(runtime, json);
    }
};

using PipBoySpecialProvider = PipBoySliceProvider<PipBoySpecialSlice>;
using PipBoyStatusProvider = PipBoySliceProvider<PipBoyStatusSlice>;
using PipBoyPlayerInfoProvider = PipBoySliceProvider<PipBoyPlayerInfoSlice>;
using PipBoyPerksProvider = PipBoySliceProvider<PipBoyPerksSlice>;
using PipBoyQuestsProvider = PipBoySliceProvider<PipBoyQuestsSlice>;
using PipBoyCollectionsProvider = PipBoySliceProvider<PipBoyCollectionsSlice>;
using PipBoyTimeProvider = PipBoySliceProvider<PipBoyTimeSlice>;

// The providers behind the Game.* callbacks and xsGetTimeData.
struct PipBoyProviders final {
    explicit PipBoyProviders(
        platform::VirtualQueryFn virtualQuery = &::VirtualQuery,
        JsonResultCache::TickFn cacheTick = nullptr) noexcept;

    PipBoyProviders(const PipBoyProviders&) = delete;
    PipBoyProviders& operator=(const PipBoyProviders&) = delete;

    // Installs the providers whose enable constant the profile sets. A
    // provider whose addresses do not resolve stays inactive without
    // affecting the others. The ActorValueInfo, ActorValueOwner and
    // BGSInventoryList vtables come from the game's RTTI, not the profile.
    void Install(
        const TargetProfile& profile,
        std::uintptr_t moduleBase) noexcept;

    PipBoySpecialProvider special;
    PipBoyStatusProvider status;
    PipBoyPlayerInfoProvider playerInfo;
    PipBoyPerksProvider perks;
    PipBoyQuestsProvider quests;
    PipBoyInventoryProvider inventory;
    PipBoyCollectionsProvider collections;
    PipBoyTimeProvider time;

private:
    platform::VirtualQueryFn virtualQuery_{};
};

}
