#include "datamanager/pip_boy_perks_snapshot.hpp"

#include "common/json.hpp"
#include "datamanager/form_text.hpp"
#include "datamanager/game_string.hpp"
#include "datamanager/local_player.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace sf::datamanager {
namespace {

// The perk card tree is an MSVC std::map: the head node pointer is at +0x8
// of the tree object, and nodes keep left, parent and right links at
// +0x00/+0x08/+0x10 with the "is head" byte at +0x19. The head's left link is
// the first node.
constexpr std::size_t kTreeHeadOffset = 0x8;
constexpr std::size_t kNodeLeftOffset = 0x0;
constexpr std::size_t kNodeParentOffset = 0x8;
constexpr std::size_t kNodeRightOffset = 0x10;
constexpr std::size_t kNodeIsHeadOffset = 0x19;

constexpr std::size_t kMaximumPerkCards = 512;
constexpr std::uint32_t kMaximumRankCount = 32;
constexpr std::size_t kMaximumNameLength = 256;
constexpr std::size_t kMaximumDescriptionLength = 4096;
constexpr std::uint8_t kNoSpecial = 7;
constexpr std::array<std::string_view, 7> kSpecialNames{
    "S", "P", "E", "C", "I", "A", "L"};
constexpr std::size_t kMaximumPerksJsonBytes = 1024 * 1024;

using GetPlayerGameModeIndex = std::int32_t(__fastcall*)(void* player);
using GetPerkCardRankPerk = void*(__fastcall*)(
    const void* card,
    std::int32_t gameMode,
    std::uint8_t rankIndex);
using GetPerkDescriptionText = void(__fastcall*)(void* perk, GameString* text);
// Returns the float cost holder's value truncated to int when the rank record
// has one, otherwise the record's byte cost. Reads only the card.
using GetPerkCardRankPointCost = std::int32_t(__fastcall*)(
    const void* card,
    std::uint8_t rankIndex);

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

[[nodiscard]] bool ReadPointerField(
    std::uintptr_t object,
    std::size_t offset,
    std::uintptr_t& value) noexcept {
    return ReadField(object, offset, value) && value != 0 &&
        value % alignof(std::uintptr_t) == 0;
}

[[nodiscard]] bool IsHeadNode(std::uintptr_t node, bool& isHead) noexcept {
    std::uint8_t flag{};
    if (!ReadField(node, kNodeIsHeadOffset, flag)) {
        return false;
    }
    isHead = flag != 0;
    return true;
}

// In-order successor, as the game's own perk card walks compute it.
[[nodiscard]] bool NextNode(std::uintptr_t& node) noexcept {
    std::uintptr_t right{};
    bool rightIsHead{};
    if (!ReadPointerField(node, kNodeRightOffset, right) ||
        !IsHeadNode(right, rightIsHead)) {
        return false;
    }
    if (!rightIsHead) {
        node = right;
        for (std::size_t depth = 0; depth < kMaximumPerkCards; ++depth) {
            std::uintptr_t left{};
            bool leftIsHead{};
            if (!ReadPointerField(node, kNodeLeftOffset, left) ||
                !IsHeadNode(left, leftIsHead)) {
                return false;
            }
            if (leftIsHead) {
                return true;
            }
            node = left;
        }
        return false;
    }

    std::uintptr_t parent{};
    if (!ReadPointerField(node, kNodeParentOffset, parent)) {
        return false;
    }
    for (std::size_t depth = 0; depth < kMaximumPerkCards; ++depth) {
        bool parentIsHead{};
        std::uintptr_t parentRight{};
        if (!IsHeadNode(parent, parentIsHead)) {
            return false;
        }
        if (parentIsHead ||
            !ReadPointerField(parent, kNodeRightOffset, parentRight) ||
            node != parentRight) {
            node = parent;
            return true;
        }
        node = parent;
        if (!ReadPointerField(node, kNodeParentOffset, parent)) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool CallGetPlayerGameModeIndex(
    std::uintptr_t function,
    std::uintptr_t player,
    std::int32_t& gameMode) noexcept {
    __try {
        gameMode = reinterpret_cast<GetPlayerGameModeIndex>(function)(
            reinterpret_cast<void*>(player));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        gameMode = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetPerkCardRankPerk(
    std::uintptr_t function,
    std::uintptr_t card,
    std::int32_t gameMode,
    std::uint8_t rankIndex,
    std::uintptr_t& perk) noexcept {
    __try {
        perk = reinterpret_cast<std::uintptr_t>(
            reinterpret_cast<GetPerkCardRankPerk>(function)(
                reinterpret_cast<const void*>(card), gameMode, rankIndex));
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        perk = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetPerkDescriptionText(
    std::uintptr_t function,
    std::uintptr_t perk,
    GameString& text) noexcept {
    __try {
        reinterpret_cast<GetPerkDescriptionText>(function)(
            reinterpret_cast<void*>(perk), &text);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

[[nodiscard]] bool CallGetPerkCardRankPointCost(
    std::uintptr_t function,
    std::uintptr_t card,
    std::uint8_t rankIndex,
    std::int32_t& pointCost) noexcept {
    __try {
        pointCost = reinterpret_cast<GetPerkCardRankPointCost>(function)(
            reinterpret_cast<const void*>(card), rankIndex);
    } __except (platform::MemoryAccessExceptionFilter(GetExceptionCode())) {
        pointCost = 0;
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadPerkDescription(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::uintptr_t perk,
    std::string& description) noexcept {
    GameString text;
    const GameStringOwner owner{
        reinterpret_cast<GameDeallocate>(runtime.deallocate), text};
    return CallGetPerkDescriptionText(
               runtime.getPerkDescriptionText, perk, text) &&
        CopyGameString(text, kMaximumDescriptionLength, description);
}

enum class PerkCardReadResult {
    Read,
    // The card has no BGSPerk for this game mode and rank.
    NoRankPerk,
    Failed,
};

[[nodiscard]] PerkCardReadResult ReadPerkCard(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::uintptr_t node,
    std::int32_t gameMode,
    PipBoyPerkCard& card) noexcept {
    const auto& layout = runtime.layout;
    std::uintptr_t perkCard{};
    std::uint8_t formType{};
    std::uint8_t rankIndex{};
    std::uint8_t equipped{};
    std::uint8_t animated{};
    std::uint32_t rankCount{};
    std::uint8_t special{};
    if (!ReadPointerField(node, layout.nodeCardOffset, perkCard) ||
        !ReadField(perkCard, layout.formTypeOffset, formType) ||
        formType != layout.perkCardFormType ||
        !ReadField(node, layout.nodeUniqueIdOffset, card.uniqueId) ||
        !ReadField(node, layout.nodeRankOffset, rankIndex) ||
        !ReadField(node, layout.nodeEquippedOffset, equipped) ||
        !ReadField(node, layout.nodeAnimatedOffset, animated) ||
        !ReadField(perkCard, layout.formIdOffset, card.perkId) ||
        !ReadField(perkCard, layout.cardRankCountOffset, rankCount) ||
        rankCount == 0 || rankCount > kMaximumRankCount ||
        rankIndex >= rankCount ||
        !ReadField(
            perkCard, layout.cardLevelRequirementOffset,
            card.levelRequirement) ||
        !ReadField(perkCard, layout.cardSpecialOffset, special) ||
        !ReadField(
            perkCard, layout.cardRaceRestrictionOffset,
            card.raceRestriction) ||
        !ReadField(perkCard, layout.cardGameModeOffset, card.gameMode) ||
        !CallGetPerkCardRankPointCost(
            runtime.getPerkCardRankPointCost, perkCard, rankIndex,
            card.pointCost)) {
        return PerkCardReadResult::Failed;
    }

    std::uintptr_t perk{};
    if (!CallGetPerkCardRankPerk(
            runtime.getPerkCardRankPerk, perkCard, gameMode, rankIndex,
            perk)) {
        return PerkCardReadResult::Failed;
    }
    if (perk == 0) {
        return PerkCardReadResult::NoRankPerk;
    }

    std::uintptr_t fullName{};
    std::uintptr_t cardDescription{};
    if (!platform::TryAddAddress(perk, layout.perkFullNameOffset, fullName) ||
        !ReadFullName(
            fullName,
            layout.getFullNameVtableOffset,
            kMaximumNameLength,
            card.name) ||
        !ReadPerkDescription(runtime, perk, card.description) ||
        !platform::TryAddAddress(
            perkCard, layout.cardDescriptionOffset, cardDescription) ||
        !ReadDescription(
            cardDescription,
            layout.getDescriptionVtableOffset,
            reinterpret_cast<GameDeallocate>(runtime.deallocate),
            kMaximumDescriptionLength,
            card.flavorText)) {
        return PerkCardReadResult::Failed;
    }

    card.rank = static_cast<std::uint32_t>(rankIndex) + 1;
    card.maximumRank = rankCount;
    card.special = special < kSpecialNames.size() ? special : kNoSpecial;
    card.equipped = equipped != 0;
    card.animated = animated != 0;
    return PerkCardReadResult::Read;
}

[[nodiscard]] bool AppendPerkCard(
    const PipBoyPerkCard& card,
    std::string& json) {
    json += R"({"name":)";
    if (!json::AppendString(card.name, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"perkId":)";
    if (!json::AppendInteger(card.perkId, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"uniqueId":)";
    if (!json::AppendInteger(card.uniqueId, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"rank":)";
    if (!json::AppendInteger(card.rank, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"maxRank":)";
    if (!json::AppendInteger(card.maximumRank, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"pointCost":)";
    if (!json::AppendInteger(card.pointCost, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"levelRequirement":)";
    if (!json::AppendInteger(
            card.levelRequirement, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"raceRestriction":)";
    if (!json::AppendInteger(
            card.raceRestriction, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"gameMode":)";
    if (!json::AppendInteger(card.gameMode, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"special":)";
    if (card.special == kNoSpecial) {
        json += "null";
    } else if (!json::AppendString(
                   kSpecialNames[card.special], json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += card.equipped ? R"(,"equipped":true)" : R"(,"equipped":false)";
    json += card.animated ? R"(,"animated":true)" : R"(,"animated":false)";
    json += R"(,"description":)";
    if (!json::AppendString(card.description, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json += R"(,"flavorText":)";
    if (!json::AppendString(card.flavorText, json, kMaximumPerksJsonBytes)) {
        return false;
    }
    json.push_back('}');
    return true;
}

}

constexpr std::array kPipBoyPerksRuntimeBindings{
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, localPlayerGlobal),
        offsetof(ResolvedPipBoyPerksRuntime, localPlayerGlobal),
        platform::AddressCheck::Readable),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, localPlayerPrimaryVtable),
        offsetof(ResolvedPipBoyPerksRuntime, localPlayerPrimaryVtable),
        platform::AddressCheck::None),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, getPlayerGameModeIndex),
        offsetof(ResolvedPipBoyPerksRuntime, getPlayerGameModeIndex),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, getPerkCardRankPerk),
        offsetof(ResolvedPipBoyPerksRuntime, getPerkCardRankPerk),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, getPerkDescriptionText),
        offsetof(ResolvedPipBoyPerksRuntime, getPerkDescriptionText),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, getPerkCardRankPointCost),
        offsetof(ResolvedPipBoyPerksRuntime, getPerkCardRankPointCost),
        platform::AddressCheck::Executable),
    platform::Bind(
        offsetof(config::NativePipBoyPerksProfile, deallocate),
        offsetof(ResolvedPipBoyPerksRuntime, deallocate),
        platform::AddressCheck::Executable),
};

bool ResolvePipBoyPerksRuntime(
    platform::VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    const config::NativePipBoyPerksProfile& perks,
    ResolvedPipBoyPerksRuntime& runtime) noexcept {
    runtime = {};
    if (virtualQuery == nullptr || moduleBase == 0 ||
        perks.playerPerkCardTreeOffset == 0 ||
        perks.nodeCardOffset == 0 ||
        perks.nodeRankOffset == 0 ||
        perks.nodeEquippedOffset == 0 ||
        perks.nodeUniqueIdOffset == 0 ||
        perks.nodeAnimatedOffset == 0 ||
        perks.cardDescriptionOffset == 0 ||
        perks.cardRankCountOffset == 0 ||
        perks.cardLevelRequirementOffset == 0 ||
        perks.cardSpecialOffset == 0 ||
        perks.cardRaceRestrictionOffset == 0 ||
        perks.cardGameModeOffset == 0 ||
        perks.perkFullNameOffset == 0 ||
        perks.getFullNameVtableOffset == 0 ||
        perks.getDescriptionVtableOffset == 0 ||
        perks.formIdOffset == 0 ||
        perks.formTypeOffset == 0 ||
        perks.perkCardFormType == 0 ||
        perks.perkCardFormType > 0xFF ||
        !platform::ValidateVtable(
            virtualQuery, moduleBase, perks.localPlayerPrimaryVtable)) {
        return false;
    }

    ResolvedPipBoyPerksRuntime candidate{
        .layout = perks,
    };
    if (!platform::ResolveAddressBindings(
            virtualQuery,
            moduleBase,
            &perks,
            &candidate,
            kPipBoyPerksRuntimeBindings)) {
        return false;
    }

    runtime = candidate;
    return true;
}

bool ReadPipBoyPerkCards(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::vector<PipBoyPerkCard>& cards) noexcept {
    cards.clear();
    std::uintptr_t player{};
    std::uintptr_t tree{};
    std::uintptr_t head{};
    std::uintptr_t node{};
    std::int32_t gameMode{};
    if (!ReadLocalPlayer(
            runtime.localPlayerGlobal,
            runtime.localPlayerPrimaryVtable,
            player) ||
        !ReadPointerField(player, runtime.layout.playerPerkCardTreeOffset, tree) ||
        !ReadPointerField(tree, kTreeHeadOffset, head) ||
        !ReadPointerField(head, kNodeLeftOffset, node) ||
        !CallGetPlayerGameModeIndex(
            runtime.getPlayerGameModeIndex, player, gameMode)) {
        return false;
    }

    std::vector<PipBoyPerkCard> result;
    for (std::size_t visited = 0;; ++visited) {
        bool isHead{};
        if (visited > kMaximumPerkCards || !IsHeadNode(node, isHead)) {
            return false;
        }
        if (isHead) {
            break;
        }

        PipBoyPerkCard card;
        switch (ReadPerkCard(runtime, node, gameMode, card)) {
            case PerkCardReadResult::Read:
                try {
                    result.push_back(std::move(card));
                } catch (...) {
                    return false;
                }
                break;
            case PerkCardReadResult::NoRankPerk:
                break;
            case PerkCardReadResult::Failed:
                return false;
        }
        if (!NextNode(node)) {
            return false;
        }
    }

    cards = std::move(result);
    return true;
}

bool BuildPipBoyPerksJson(
    const ResolvedPipBoyPerksRuntime& runtime,
    std::string& json) noexcept {
    json.clear();
    std::vector<PipBoyPerkCard> cards;
    if (!ReadPipBoyPerkCards(runtime, cards)) {
        return false;
    }

    try {
        json.reserve(64 + cards.size() * 512);
        json += R"({"perks":[)";
        for (std::size_t index = 0; index < cards.size(); ++index) {
            if (index != 0) {
                json.push_back(',');
            }
            if (!AppendPerkCard(cards[index], json)) {
                json.clear();
                return false;
            }
        }
        json += "]}";
    } catch (...) {
        json.clear();
        return false;
    }
    if (json.size() > kMaximumPerksJsonBytes) {
        json.clear();
        return false;
    }
    return true;
}

}
