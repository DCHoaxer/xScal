#include "datamanager/pip_boy_providers.hpp"

#include "platform/diagnostics.hpp"
#include "platform/rtti_vtables.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sf::datamanager {

bool PipBoyStatusSlice::Resolve(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const TargetProfile& profile,
    Runtime& runtime) noexcept {
    if (!ResolvePipBoyStatusRuntime(
            virtualQuery,
            moduleBase,
            profile.dataManager.actorValues,
            profile.dataManager.pipBoyStatus,
            runtime.status)) {
        return false;
    }
    runtime.inventoryResolved = ResolvePipBoyInventoryRuntime(
        virtualQuery,
        moduleBase,
        profile.dataManager.pipBoyInventory,
        runtime.inventory);
    return true;
}

bool PipBoyStatusSlice::Build(
    const Runtime& runtime,
    std::string& json) noexcept {
    // A busy or unreadable inventory leaves weaponDamage null.
    std::vector<PipBoyDamageRow> weaponDamage;
    const bool hasWeaponDamage = runtime.inventoryResolved &&
        ReadEquippedWeaponDamage(runtime.inventory, weaponDamage) ==
            PipBoyInventoryCaptureResult::Captured;
    return BuildPipBoyStatusJson(
        runtime.status, json, hasWeaponDamage ? &weaponDamage : nullptr);
}

PipBoyProviders::PipBoyProviders(
    platform::VirtualQueryFn virtualQuery,
    JsonResultCache::TickFn cacheTick) noexcept
    : special(virtualQuery, cacheTick),
      status(virtualQuery, cacheTick),
      playerInfo(virtualQuery, cacheTick),
      perks(virtualQuery, cacheTick),
      quests(virtualQuery, cacheTick),
      inventory(virtualQuery, cacheTick),
      collections(virtualQuery, cacheTick),
      time(virtualQuery, cacheTick),
      virtualQuery_(virtualQuery) {}

void PipBoyProviders::Install(
    const TargetProfile& profile,
    std::uintptr_t moduleBase) noexcept {
    // The readers compare live objects against these vtables. Taking them
    // from RTTI keeps them out of the Address Library, and RTTI lives in data
    // sections that are readable while .text is still encrypted.
    const auto ownerOffset = profile.dataManager.actorValues.actorValueOwnerOffset;
    std::array queries{
        platform::RttiVtableQuery{".?AVActorValueInfo@@", 0, 0},
        platform::RttiVtableQuery{
            ".?AVLocalPlayerCharacter@@",
            static_cast<std::uint32_t>(ownerOffset),
            0},
        platform::RttiVtableQuery{".?AVBGSInventoryList@@", 0, 0},
    };
    (void)platform::FindRttiVtables(
        virtualQuery_, moduleBase, queries.data(), queries.size());
    for (const auto& query : queries) {
        if (query.vtableRva == 0) {
            DiagnosticLogFormat(
                "xScal RTTI: no unique vtable for %.*s",
                static_cast<int>(query.typeName.size()),
                query.typeName.data());
        }
    }

    TargetProfile resolved = profile;
    resolved.dataManager.actorValues.actorValueInfoVtable = queries[0].vtableRva;
    // A zero owner offset is an incomplete profile; the lookup then found the
    // primary vtable, which is not the owner's.
    resolved.dataManager.actorValues.localPlayerActorValueOwnerVtable =
        ownerOffset != 0 ? queries[1].vtableRva : 0;
    resolved.dataManager.pipBoyInventory.inventoryListVtable =
        queries[2].vtableRva;

    if (resolved.pipBoySpecialEnabled) {
        (void)special.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyStatusEnabled) {
        (void)status.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyPlayerInfoEnabled) {
        (void)playerInfo.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyPerksEnabled) {
        (void)perks.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyQuestsEnabled) {
        (void)quests.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyInventoryEnabled) {
        (void)inventory.Install(resolved, moduleBase);
    }
    if (resolved.pipBoyCollectionsEnabled) {
        (void)collections.Install(resolved, moduleBase);
    }
    // Time data has no enable constant. Its profile is empty, so the install
    // fails, when the database lacks any of its four entries.
    (void)time.Install(resolved, moduleBase);
}

}
