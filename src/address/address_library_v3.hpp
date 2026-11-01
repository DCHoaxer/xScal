#pragma once

#include "address/address_library.hpp"
#include "address/address_library_format.hpp"
#include "config/target_profile.hpp"
#include "plugins/plugin_signature.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Reads addlib.json and its cached parts.
namespace sf::address::v3 {

inline constexpr std::size_t kMaximumFileSize = 1024 * 1024;
inline constexpr std::uint32_t kDefaultCheckHours = 24;
inline constexpr std::string_view kAddlibPath = "/addlib/v3/addlib.json";
inline constexpr std::string_view kDatabasePathPrefix = "/addlib/v3/";
inline constexpr std::string_view kRevocationsContext = "xscal-revocations-v1";

enum class Severity { Info, Warning, Error };

struct Message final {
    std::string id;
    Severity severity{Severity::Info};
    bool hasMinXScal{};
    bool hasMaxXScal{};
    format::XScalVersion minXScal{};
    format::XScalVersion maxXScal{};
    // FILETIME; 0 when the message does not expire.
    std::uint64_t expires{};
    std::string text;
    std::string url;
};

enum class BuildStatus { Ok, Held, Unavailable, Retired };

struct DatabaseEntry final {
    format::XScalVersion release{};
    format::XScalVersion minimum{};
    std::uint32_t revision{};
    std::uint32_t size{};
    plugins::signing::Hash sha256{};
    std::string url;
};

struct BlockedRevision final {
    format::XScalVersion release{};
    std::uint32_t revision{};
    std::string messageId;
};

struct Build final {
    TargetKind target{TargetKind::Steam};
    config::RuntimeVersion runtime{};
    ExecutableIdentity executable{};
    BuildStatus status{BuildStatus::Unavailable};
    std::uint32_t checkAgainHours{kDefaultCheckHours};
    bool hasDatabase{};
    DatabaseEntry database;
    // One entry per xScal release, from "databases"; empty when the status
    // has no such list (then only "database" applies).
    std::vector<DatabaseEntry> databases;
    std::vector<BlockedRevision> blockedRevisions;
    std::vector<std::string> messageIds;
    // "Always" messages for this build, most specific first: the first that
    // applies is shown on every launch.
    std::vector<std::string> alwaysMessageIds;
};

struct Status final {
    std::uint64_t sequence{};
    std::uint64_t revocationsSequence{};
    std::vector<Build> builds;
    std::vector<Message> messages;
    // For the flatline dialog when no usable database is installed and the
    // build lists no message that applies, also for builds the status does
    // not list. Most specific first; the first that applies is shown.
    std::vector<std::string> missingMessageIds;
    // "Always" messages for builds the status does not list, most specific
    // first.
    std::vector<std::string> alwaysMessageIds;
};

struct DeveloperRevocation final {
    std::uint32_t id{};
    std::string reason;
    std::string messageId;
};

struct HashRevocation final {
    plugins::signing::Hash sha256{};
    std::uint32_t developerId{};
    std::string reason;
    std::string messageId;
};

struct Revocations final {
    std::uint64_t sequence{};
    std::vector<DeveloperRevocation> developers;
    std::vector<HashRevocation> pluginHashes;
    std::vector<Message> messages;
};

// One signed part as it is written in addlib.json and in the cache files.
struct RawPart final {
    std::string payload;    // base64
    std::string signature;  // base64
    std::uint32_t rootKeyIndex{};  // revocations only
};

// The two parts of addlib.json. A part that is absent or malformed has
// has... false; its detail names the problem (nullptr when the part is null).
struct RawFile final {
    bool hasStatus{};
    RawPart status;
    const char* statusDetail{};
    bool hasRevocations{};
    RawPart revocations;
    const char* revocationsDetail{};
};

enum class PartCheck {
    Accepted,
    // Valid, but the sequence is not higher than newerThan. The parsed
    // payload is still returned.
    NotNewer,
    BadBase64,
    BadSignature,
    Malformed,
};

// Standard base64 with padding. False on any other character or layout.
[[nodiscard]] bool DecodeBase64(
    std::string_view text,
    std::vector<std::uint8_t>& bytes) noexcept;
[[nodiscard]] std::string EncodeBase64(
    const std::uint8_t* bytes,
    std::size_t size);

// Splits addlib.json. False when the file is not a version 3 object.
[[nodiscard]] bool SplitFile(
    std::string_view text,
    RawFile& file,
    const char*& detail) noexcept;

// Checks one part and parses its payload. detail names the signature check
// result or the malformed field. status and revocations are filled for
// Accepted and NotNewer.
[[nodiscard]] PartCheck CheckStatusPart(
    const RawPart& part,
    std::uint64_t newerThan,
    Status& status,
    const char*& detail) noexcept;
[[nodiscard]] PartCheck CheckRevocationsPart(
    const RawPart& part,
    std::uint64_t newerThan,
    Revocations& revocations,
    const char*& detail) noexcept;

[[nodiscard]] const Build* FindBuild(
    const Status& status,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable) noexcept;
[[nodiscard]] const BlockedRevision* FindBlockedRevision(
    const Build& build,
    format::XScalVersion release,
    std::uint32_t revision) noexcept;
// The database entry for the running xScal: among "databases" the entry with
// the highest (release, revision) whose minimum is not newer than running.
// Falls back to "database" when the list is empty. Null when none applies.
[[nodiscard]] const DatabaseEntry* FindDatabaseFor(
    const Build& build,
    format::XScalVersion running) noexcept;
[[nodiscard]] const Message* FindMessage(
    const std::vector<Message>& messages,
    std::string_view id) noexcept;

// True when xscal is inside the message's version range (both ends
// included) and nowFileTime is before expires.
[[nodiscard]] bool IsMessageCurrent(
    const Message& message,
    format::XScalVersion xscal,
    std::uint64_t nowFileTime) noexcept;
// IsMessageCurrent for the running xScal, and not shown yet in this launch.
// Marks the message as shown when it returns true. Used for the box of a
// plugin rejected by an online revocation.
[[nodiscard]] bool ClaimMessage(
    const Message& message,
    std::uint64_t nowFileTime) noexcept;
// The dialog text in the style of xScal's own dialogs: lead (a paragraph
// xScal writes, such as which plugin was not loaded; may be empty), the
// message text, then the link on its own line. warning and error use the
// exclamation icon like the other Address Library dialogs.
[[nodiscard]] bool BuildMessageText(
    const Message& message,
    std::wstring_view lead,
    std::wstring& text,
    UINT& icon) noexcept;

// Cache files in Data\XSCAL\AddressLibrary. Both loaders check the part
// again and return false for a missing or failing file.
[[nodiscard]] bool LoadCachedStatus(Status& status) noexcept;
[[nodiscard]] bool LoadCachedRevocations(Revocations& revocations) noexcept;
[[nodiscard]] bool StoreStatusPart(const RawPart& part) noexcept;
[[nodiscard]] bool StoreRevocationsPart(const RawPart& part) noexcept;
// checkAgainHours of the cached entry for this build, kDefaultCheckHours
// when there is no accepted status or no entry.
[[nodiscard]] std::uint32_t CachedCheckHours(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable) noexcept;

// Adds the revocations to the signature checks. Does nothing after the first
// call.
void ApplyCachedRevocations() noexcept;
void ApplyRevocations(const Revocations& revocations) noexcept;

struct OnlineRejection final {
    const char* reason{};
    const Message* message{};
};
// Looks up the online entry behind a DeveloperRevoked or BuildRevoked result
// whose onlineRevocation is set.
[[nodiscard]] bool FindOnlineRejection(
    const plugins::signing::SignatureResult& result,
    OnlineRejection& rejection) noexcept;

}  // namespace sf::address::v3
