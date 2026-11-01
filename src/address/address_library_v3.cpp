#include "address/address_library_v3.hpp"

#include "common/json.hpp"
#include "config/config.h"
#include "platform/diagnostics.hpp"
#include "platform/win32_file.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace sf::address::v3 {
namespace {

using json::MemberStatus;
using plugins::signing::Hash;

constexpr std::size_t kMaximumBuilds = 256;
constexpr std::size_t kMaximumMessages = 256;
constexpr std::size_t kMaximumListEntries = 4096;
constexpr std::size_t kMaximumDatabases = 64;
constexpr std::size_t kMaximumText = 4000;
constexpr std::size_t kMaximumUrl = 500;
constexpr std::size_t kMaximumReason = 500;
constexpr std::int64_t kMaximumInteger = std::numeric_limits<std::int64_t>::max();

constexpr wchar_t kStatusFile[] = L"Data\\XSCAL\\AddressLibrary\\v3-status.json";
constexpr wchar_t kRevocationsFile[] = L"Data\\XSCAL\\AddressLibrary\\v3-revocations.json";

[[nodiscard]] json::ParseOptions ParseOptions() noexcept {
    json::ParseOptions options;
    options.maximumInputBytes = kMaximumFileSize;
    options.maximumDepth = 16;
    options.rejectDuplicateMembers = true;
    return options;
}

// Field readers. On failure they set detail to label and return false.
[[nodiscard]] bool ReadText(
    const json::Value& object,
    const char* name,
    const char* label,
    std::size_t maximumLength,
    std::string_view& value,
    const char*& detail) noexcept {
    if (object.ReadStringMember(name, value) != MemberStatus::Found ||
        value.size() > maximumLength) {
        detail = label;
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadNumber(
    const json::Value& object,
    const char* name,
    const char* label,
    std::int64_t minimum,
    std::int64_t maximum,
    std::int64_t& value,
    const char*& detail) noexcept {
    if (object.ReadIntegerMember(name, value) != MemberStatus::Found ||
        value < minimum || value > maximum) {
        detail = label;
        return false;
    }
    return true;
}

[[nodiscard]] bool ReadList(
    const json::Value& object,
    const char* name,
    const char* label,
    std::size_t maximumSize,
    json::Value& list,
    const char*& detail) noexcept {
    if (object.ReadArrayMember(name, list) != MemberStatus::Found ||
        list.ArraySize() > maximumSize) {
        detail = label;
        return false;
    }
    return true;
}

[[nodiscard]] bool IsDigit(char character) noexcept {
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool ParseDecimal(
    std::string_view text,
    std::uint32_t maximum,
    std::uint32_t& value) noexcept {
    if (text.empty() || text.size() > 5 || (text.size() > 1 && text[0] == '0')) {
        return false;
    }
    std::uint32_t result = 0;
    for (const char character : text) {
        if (!IsDigit(character)) {
            return false;
        }
        result = result * 10 + static_cast<std::uint32_t>(character - '0');
    }
    if (result > maximum) {
        return false;
    }
    value = result;
    return true;
}

[[nodiscard]] bool ParseVersion(
    std::string_view text,
    format::XScalVersion& version) noexcept {
    std::uint32_t parts[3]{};
    for (int index = 0; index < 3; ++index) {
        const std::size_t dot = text.find('.');
        if ((index < 2) == (dot == std::string_view::npos)) {
            return false;
        }
        if (!ParseDecimal(text.substr(0, dot), 255, parts[index])) {
            return false;
        }
        if (index < 2) {
            text.remove_prefix(dot + 1);
        }
    }
    version = {
        static_cast<std::uint8_t>(parts[0]),
        static_cast<std::uint8_t>(parts[1]),
        static_cast<std::uint8_t>(parts[2])};
    return true;
}

[[nodiscard]] bool ParseRuntime(
    std::string_view text,
    config::RuntimeVersion& runtime) noexcept {
    std::uint32_t parts[4]{};
    for (int index = 0; index < 4; ++index) {
        const std::size_t dot = text.find('.');
        if ((index < 3) == (dot == std::string_view::npos)) {
            return false;
        }
        if (!ParseDecimal(text.substr(0, dot), 65535, parts[index])) {
            return false;
        }
        if (index < 3) {
            text.remove_prefix(dot + 1);
        }
    }
    runtime = {
        static_cast<std::uint16_t>(parts[0]),
        static_cast<std::uint16_t>(parts[1]),
        static_cast<std::uint16_t>(parts[2]),
        static_cast<std::uint16_t>(parts[3])};
    return true;
}

[[nodiscard]] int HexValue(char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// "0x" and one to eight hex digits.
[[nodiscard]] bool ParseHex32(std::string_view text, std::uint32_t& value) noexcept {
    if (text.size() < 3 || text.size() > 10 || text[0] != '0' || text[1] != 'x') {
        return false;
    }
    std::uint32_t result = 0;
    for (const char character : text.substr(2)) {
        const int digit = HexValue(character);
        if (digit < 0) {
            return false;
        }
        result = (result << 4) | static_cast<std::uint32_t>(digit);
    }
    value = result;
    return true;
}

[[nodiscard]] bool ParseHash(std::string_view text, Hash& hash) noexcept {
    if (text.size() != hash.size() * 2) {
        return false;
    }
    for (std::size_t index = 0; index < hash.size(); ++index) {
        const int high = HexValue(text[index * 2]);
        const int low = HexValue(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        hash[index] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return true;
}

[[nodiscard]] bool ReadDigits(
    std::string_view text,
    std::size_t offset,
    std::size_t count,
    WORD& value) noexcept {
    unsigned int result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char character = text[offset + index];
        if (!IsDigit(character)) {
            return false;
        }
        result = result * 10 + static_cast<unsigned int>(character - '0');
    }
    value = static_cast<WORD>(result);
    return true;
}

// "YYYY-MM-DDTHH:MM:SSZ", or "YYYY-MM-DD" when dateOnly.
[[nodiscard]] bool ParseTime(
    std::string_view text,
    bool dateOnly,
    std::uint64_t& fileTime) noexcept {
    if (text.size() != (dateOnly ? 10u : 20u) || text[4] != '-' || text[7] != '-') {
        return false;
    }
    SYSTEMTIME time{};
    if (!ReadDigits(text, 0, 4, time.wYear) || !ReadDigits(text, 5, 2, time.wMonth) ||
        !ReadDigits(text, 8, 2, time.wDay)) {
        return false;
    }
    if (!dateOnly) {
        if (text[10] != 'T' || text[13] != ':' || text[16] != ':' || text[19] != 'Z' ||
            !ReadDigits(text, 11, 2, time.wHour) || !ReadDigits(text, 14, 2, time.wMinute) ||
            !ReadDigits(text, 17, 2, time.wSecond)) {
            return false;
        }
    }
    FILETIME converted{};
    if (::SystemTimeToFileTime(&time, &converted) == FALSE) {
        return false;
    }
    fileTime = (static_cast<std::uint64_t>(converted.dwHighDateTime) << 32) |
        converted.dwLowDateTime;
    return true;
}

[[nodiscard]] bool IsMessageId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 40) {
        return false;
    }
    for (const char character : id) {
        if (!(character >= 'a' && character <= 'z') && !IsDigit(character) &&
            character != '-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ReadTimeField(
    const json::Value& object,
    const char* name,
    const char* label,
    bool dateOnly,
    const char*& detail) noexcept {
    std::string_view text;
    std::uint64_t ignored{};
    if (!ReadText(object, name, label, 20, text, detail)) {
        return false;
    }
    if (!ParseTime(text, dateOnly, ignored)) {
        detail = label;
        return false;
    }
    return true;
}

// A member that is null or a message ID.
[[nodiscard]] bool ReadOptionalMessageId(
    const json::Value& object,
    const char* label,
    std::string& id,
    const char*& detail) noexcept {
    const json::Value member = object.FindMember("messageId");
    if (member && member.GetType() == json::Type::Null) {
        id.clear();
        return true;
    }
    std::string_view text;
    if (!member || !member.ReadString(text) || !IsMessageId(text)) {
        detail = label;
        return false;
    }
    id.assign(text);
    return true;
}

[[nodiscard]] bool ReadOptionalVersion(
    const json::Value& object,
    const char* name,
    const char* label,
    bool& present,
    format::XScalVersion& version,
    const char*& detail) noexcept {
    const json::Value member = object.FindMember(name);
    present = false;
    if (member && member.GetType() == json::Type::Null) {
        return true;
    }
    std::string_view text;
    if (!member || !member.ReadString(text) || !ParseVersion(text, version)) {
        detail = label;
        return false;
    }
    present = true;
    return true;
}

[[nodiscard]] bool ParseMessage(
    const json::Value& object,
    Message& message,
    const char*& detail) noexcept {
    if (object.GetType() != json::Type::Object) {
        detail = "messages[]";
        return false;
    }
    std::string_view text;
    if (!ReadText(object, "id", "messages[].id", 40, text, detail)) {
        return false;
    }
    if (!IsMessageId(text)) {
        detail = "messages[].id";
        return false;
    }
    message.id.assign(text);
    if (!ReadText(object, "severity", "messages[].severity", 16, text, detail)) {
        return false;
    }
    if (text == "info") message.severity = Severity::Info;
    else if (text == "warning") message.severity = Severity::Warning;
    else if (text == "error") message.severity = Severity::Error;
    else {
        detail = "messages[].severity";
        return false;
    }
    if (!ReadOptionalVersion(object, "minXScal", "messages[].minXScal",
            message.hasMinXScal, message.minXScal, detail) ||
        !ReadOptionalVersion(object, "maxXScal", "messages[].maxXScal",
            message.hasMaxXScal, message.maxXScal, detail)) {
        return false;
    }
    const json::Value expires = object.FindMember("expires");
    message.expires = 0;
    if (!expires || (expires.GetType() != json::Type::Null &&
            (!expires.ReadString(text) || !ParseTime(text, false, message.expires)))) {
        detail = "messages[].expires";
        return false;
    }
    if (!ReadText(object, "text", "messages[].text", kMaximumText, text, detail)) {
        return false;
    }
    message.text.assign(text);
    if (!ReadText(object, "url", "messages[].url", kMaximumUrl, text, detail)) {
        return false;
    }
    if (!text.empty() && text.substr(0, 8) != "https://") {
        detail = "messages[].url";
        return false;
    }
    message.url.assign(text);
    return true;
}

[[nodiscard]] bool ParseMessages(
    const json::Value& root,
    std::vector<Message>& messages,
    const char*& detail) noexcept {
    json::Value list;
    if (!ReadList(root, "messages", "messages", kMaximumMessages, list, detail)) {
        return false;
    }
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        Message message;
        if (!ParseMessage(list.ArrayItem(index), message, detail)) {
            return false;
        }
        messages.push_back(std::move(message));
    }
    return true;
}

[[nodiscard]] bool IsDatabaseUrl(std::string_view url) noexcept {
    if (url.size() <= kDatabasePathPrefix.size() || url.size() > 200 ||
        url.substr(0, kDatabasePathPrefix.size()) != kDatabasePathPrefix ||
        url.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char character : url) {
        if (!IsDigit(character) && !(character >= 'a' && character <= 'z') &&
            !(character >= 'A' && character <= 'Z') && character != '/' &&
            character != '.' && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ParseDatabase(
    const json::Value& object,
    DatabaseEntry& database,
    const char*& detail) noexcept {
    std::string_view text;
    std::int64_t number{};
    if (!ReadText(object, "release", "builds[].database.release", 16, text, detail)) return false;
    if (!ParseVersion(text, database.release)) {
        detail = "builds[].database.release";
        return false;
    }
    if (!ReadText(object, "minimum", "builds[].database.minimum", 16, text, detail)) return false;
    if (!ParseVersion(text, database.minimum)) {
        detail = "builds[].database.minimum";
        return false;
    }
    if (!ReadNumber(object, "revision", "builds[].database.revision", 0, 0xFFFFFFFF, number, detail)) return false;
    database.revision = static_cast<std::uint32_t>(number);
    if (!ReadNumber(object, "size", "builds[].database.size", 1,
            static_cast<std::int64_t>(kMaximumFileSize), number, detail)) return false;
    database.size = static_cast<std::uint32_t>(number);
    if (!ReadText(object, "sha256", "builds[].database.sha256", 64, text, detail)) return false;
    if (!ParseHash(text, database.sha256)) {
        detail = "builds[].database.sha256";
        return false;
    }
    if (!ReadText(object, "url", "builds[].database.url", 200, text, detail)) return false;
    if (!IsDatabaseUrl(text)) {
        detail = "builds[].database.url";
        return false;
    }
    database.url.assign(text);
    return ReadTimeField(object, "deployedAt", "builds[].database.deployedAt", false, detail);
}

// Reads an optional list of message IDs; older payloads do not have it.
[[nodiscard]] bool ReadOptionalMessageIds(
    const json::Value& object,
    const char* name,
    const char* label,
    std::vector<std::string>& ids,
    const char*& detail) noexcept {
    if (!object.FindMember(name)) {
        return true;
    }
    json::Value list;
    if (!ReadList(object, name, label, kMaximumListEntries, list, detail)) {
        return false;
    }
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        std::string_view id;
        if (!list.ArrayItem(index).ReadString(id) || !IsMessageId(id)) {
            detail = label;
            return false;
        }
        ids.emplace_back(id);
    }
    return true;
}

[[nodiscard]] bool ParseBuild(
    const json::Value& object,
    Build& build,
    const char*& detail) noexcept {
    if (object.GetType() != json::Type::Object) {
        detail = "builds[]";
        return false;
    }
    std::string_view text;
    std::int64_t number{};
    if (!ReadText(object, "target", "builds[].target", 16, text, detail)) return false;
    if (text == "steam") build.target = TargetKind::Steam;
    else if (text == "gamepass") build.target = TargetKind::GamePass;
    else {
        detail = "builds[].target";
        return false;
    }
    if (!ReadText(object, "runtime", "builds[].runtime", 24, text, detail)) return false;
    if (!ParseRuntime(text, build.runtime)) {
        detail = "builds[].runtime";
        return false;
    }
    json::Value executable;
    if (object.ReadObjectMember("executable", executable) != MemberStatus::Found) {
        detail = "builds[].executable";
        return false;
    }
    if (!ReadText(executable, "timeDateStamp", "builds[].executable.timeDateStamp", 10, text, detail)) return false;
    if (!ParseHex32(text, build.executable.timeDateStamp)) {
        detail = "builds[].executable.timeDateStamp";
        return false;
    }
    if (!ReadText(executable, "sizeOfImage", "builds[].executable.sizeOfImage", 10, text, detail)) return false;
    if (!ParseHex32(text, build.executable.imageSize)) {
        detail = "builds[].executable.sizeOfImage";
        return false;
    }
    if (!ReadText(object, "status", "builds[].status", 16, text, detail)) return false;
    if (text == "ok") build.status = BuildStatus::Ok;
    else if (text == "held") build.status = BuildStatus::Held;
    else if (text == "unavailable") build.status = BuildStatus::Unavailable;
    else if (text == "retired") build.status = BuildStatus::Retired;
    else {
        detail = "builds[].status";
        return false;
    }
    if (!ReadNumber(object, "checkAgainHours", "builds[].checkAgainHours", 1, 168, number, detail)) return false;
    build.checkAgainHours = static_cast<std::uint32_t>(number);

    const json::Value database = object.FindMember("database");
    if (!database) {
        detail = "builds[].database";
        return false;
    }
    build.hasDatabase = database.GetType() != json::Type::Null;
    if (build.hasDatabase && !ParseDatabase(database, build.database, detail)) {
        return false;
    }

    // 0.4.33 clients ignore this list and read "database" above.
    json::Value list;
    if (object.FindMember("databases")) {
        if (!ReadList(object, "databases", "builds[].databases", kMaximumDatabases, list, detail)) return false;
        for (std::size_t index = 0; index < list.ArraySize(); ++index) {
            const json::Value item = list.ArrayItem(index);
            DatabaseEntry entry;
            if (item.GetType() != json::Type::Object) {
                detail = "builds[].databases[]";
                return false;
            }
            if (!ParseDatabase(item, entry, detail)) return false;
            build.databases.push_back(std::move(entry));
        }
    }
    if (!ReadList(object, "blockedRevisions", "builds[].blockedRevisions", kMaximumListEntries, list, detail)) return false;
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        const json::Value item = list.ArrayItem(index);
        BlockedRevision blocked;
        if (item.GetType() != json::Type::Object ||
            !ReadText(item, "release", "builds[].blockedRevisions[].release", 16, text, detail)) {
            detail = detail != nullptr ? detail : "builds[].blockedRevisions[]";
            return false;
        }
        if (!ParseVersion(text, blocked.release)) {
            detail = "builds[].blockedRevisions[].release";
            return false;
        }
        if (!ReadNumber(item, "revision", "builds[].blockedRevisions[].revision", 0, 0xFFFFFFFF, number, detail)) return false;
        blocked.revision = static_cast<std::uint32_t>(number);
        if (!ReadOptionalMessageId(item, "builds[].blockedRevisions[].messageId", blocked.messageId, detail)) return false;
        build.blockedRevisions.push_back(std::move(blocked));
    }
    if (!ReadList(object, "messageIds", "builds[].messageIds", kMaximumListEntries, list, detail)) return false;
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        if (!list.ArrayItem(index).ReadString(text) || !IsMessageId(text)) {
            detail = "builds[].messageIds[]";
            return false;
        }
        build.messageIds.emplace_back(text);
    }
    return ReadOptionalMessageIds(object, "alwaysMessageIds", "builds[].alwaysMessageIds", build.alwaysMessageIds, detail);
}

[[nodiscard]] bool ParseStatusPayload(
    std::string_view text,
    Status& status,
    const char*& detail) noexcept {
    json::Document document;
    if (!json::ParseDocument(text, ParseOptions(), document)) {
        detail = "payload is not valid JSON";
        return false;
    }
    const json::Value root = document.Root();
    if (root.GetType() != json::Type::Object) {
        detail = "payload is not an object";
        return false;
    }
    std::int64_t number{};
    if (!ReadNumber(root, "format", "format", 3, 3, number, detail) ||
        !ReadNumber(root, "sequence", "sequence", 1, kMaximumInteger, number, detail)) {
        return false;
    }
    status.sequence = static_cast<std::uint64_t>(number);
    if (!ReadTimeField(root, "generated", "generated", false, detail) ||
        !ReadNumber(root, "revocationsSequence", "revocationsSequence", 0, kMaximumInteger, number, detail)) {
        return false;
    }
    status.revocationsSequence = static_cast<std::uint64_t>(number);
    json::Value list;
    if (!ReadList(root, "builds", "builds", kMaximumBuilds, list, detail)) {
        return false;
    }
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        Build build;
        if (!ParseBuild(list.ArrayItem(index), build, detail)) {
            return false;
        }
        status.builds.push_back(std::move(build));
    }
    if (!ParseMessages(root, status.messages, detail)) {
        return false;
    }
    if (!ReadOptionalMessageIds(root, "missingMessageIds", "missingMessageIds[]", status.missingMessageIds, detail) ||
        !ReadOptionalMessageIds(root, "alwaysMessageIds", "alwaysMessageIds[]", status.alwaysMessageIds, detail)) {
        return false;
    }
    if (!ReadList(root, "developers", "developers", kMaximumListEntries, list, detail)) {
        return false;
    }
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        const json::Value item = list.ArrayItem(index);
        std::string_view value;
        if (item.GetType() != json::Type::Object) {
            detail = "developers[]";
            return false;
        }
        if (!ReadNumber(item, "id", "developers[].id", 0, 0xFFFFFFFF, number, detail) ||
            !ReadText(item, "prefix", "developers[].prefix", 31, value, detail) ||
            !ReadText(item, "name", "developers[].name", kMaximumReason, value, detail) ||
            !ReadText(item, "homepage", "developers[].homepage", kMaximumUrl, value, detail) ||
            !ReadText(item, "status", "developers[].status", 16, value, detail)) {
            return false;
        }
        if (value != "active" && value != "revoked") {
            detail = "developers[].status";
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ParseRevocationsPayload(
    std::string_view text,
    Revocations& revocations,
    const char*& detail) noexcept {
    json::Document document;
    if (!json::ParseDocument(text, ParseOptions(), document)) {
        detail = "payload is not valid JSON";
        return false;
    }
    const json::Value root = document.Root();
    if (root.GetType() != json::Type::Object) {
        detail = "payload is not an object";
        return false;
    }
    std::int64_t number{};
    if (!ReadNumber(root, "format", "format", 1, 1, number, detail) ||
        !ReadNumber(root, "sequence", "sequence", 1, kMaximumInteger, number, detail)) {
        return false;
    }
    revocations.sequence = static_cast<std::uint64_t>(number);
    if (!ReadTimeField(root, "generated", "generated", false, detail)) {
        return false;
    }
    json::Value list;
    if (!ReadList(root, "developers", "developers", kMaximumListEntries, list, detail)) {
        return false;
    }
    std::string_view value;
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        const json::Value item = list.ArrayItem(index);
        DeveloperRevocation developer;
        if (item.GetType() != json::Type::Object) {
            detail = "developers[]";
            return false;
        }
        if (!ReadNumber(item, "id", "developers[].id", 0, 0xFFFFFFFF, number, detail)) return false;
        developer.id = static_cast<std::uint32_t>(number);
        if (!ReadTimeField(item, "since", "developers[].since", true, detail) ||
            !ReadText(item, "reason", "developers[].reason", kMaximumReason, value, detail)) {
            return false;
        }
        developer.reason.assign(value);
        if (!ReadOptionalMessageId(item, "developers[].messageId", developer.messageId, detail)) return false;
        revocations.developers.push_back(std::move(developer));
    }
    if (!ReadList(root, "pluginHashes", "pluginHashes", kMaximumListEntries, list, detail)) {
        return false;
    }
    for (std::size_t index = 0; index < list.ArraySize(); ++index) {
        const json::Value item = list.ArrayItem(index);
        HashRevocation entry;
        if (item.GetType() != json::Type::Object) {
            detail = "pluginHashes[]";
            return false;
        }
        if (!ReadText(item, "sha256", "pluginHashes[].sha256", 64, value, detail)) return false;
        if (!ParseHash(value, entry.sha256)) {
            detail = "pluginHashes[].sha256";
            return false;
        }
        if (!ReadNumber(item, "developerId", "pluginHashes[].developerId", 0, 0xFFFFFFFF, number, detail)) return false;
        entry.developerId = static_cast<std::uint32_t>(number);
        if (!ReadText(item, "reason", "pluginHashes[].reason", kMaximumReason, value, detail)) return false;
        entry.reason.assign(value);
        if (!ReadOptionalMessageId(item, "pluginHashes[].messageId", entry.messageId, detail)) return false;
        revocations.pluginHashes.push_back(std::move(entry));
    }
    return ParseMessages(root, revocations.messages, detail);
}

[[nodiscard]] bool ReadPart(
    const json::Value& object,
    bool withRootKey,
    RawPart& part,
    const char*& detail) noexcept {
    if (object.GetType() != json::Type::Object) {
        detail = "part is not an object";
        return false;
    }
    std::string_view text;
    if (object.ReadStringMember("payload", text) != MemberStatus::Found) {
        detail = "payload";
        return false;
    }
    part.payload.assign(text);
    if (object.ReadStringMember("signature", text) != MemberStatus::Found) {
        detail = "signature";
        return false;
    }
    part.signature.assign(text);
    if (withRootKey) {
        std::int64_t index{};
        if (object.ReadIntegerMember("rootKeyIndex", index) != MemberStatus::Found ||
            index < 0 || index > 255) {
            detail = "rootKeyIndex";
            return false;
        }
        part.rootKeyIndex = static_cast<std::uint32_t>(index);
    }
    return true;
}

[[nodiscard]] bool ParsePartFile(
    std::string_view text,
    bool withRootKey,
    RawPart& part,
    const char*& detail) noexcept {
    json::Document document;
    if (!json::ParseDocument(text, ParseOptions(), document)) {
        detail = "file is not valid JSON";
        return false;
    }
    return ReadPart(document.Root(), withRootKey, part, detail);
}

[[nodiscard]] std::string SerializePart(const RawPart& part, bool withRootKey) {
    std::string text = "{\"payload\":";
    json::AppendString(part.payload, text);
    text += ",\"signature\":";
    json::AppendString(part.signature, text);
    if (withRootKey) {
        text += ",\"rootKeyIndex\":" + std::to_string(part.rootKeyIndex);
    }
    text += "}\n";
    return text;
}

[[nodiscard]] bool ResolveCachePath(
    const wchar_t* relative,
    platform::ExecutableRelativePath& path) noexcept {
    return platform::ResolveExecutableRelativePath(relative, path);
}

[[nodiscard]] bool ReadCacheFile(const wchar_t* relative, std::string& text) noexcept {
    platform::ExecutableRelativePath path;
    return ResolveCachePath(relative, path) &&
        platform::QueryFilePresence(path) == platform::FilePresence::Present &&
        platform::ReadBoundedFile(path, kMaximumFileSize, text);
}

[[nodiscard]] bool WriteCacheFile(const wchar_t* relative, std::string_view text) noexcept {
    platform::ExecutableRelativePath path;
    return ResolveCachePath(relative, path) && platform::WriteFileAtomically(path, text);
}

SRWLOCK launchLock = SRWLOCK_INIT;
std::vector<std::string> launchShown;
INIT_ONCE revocationsOnce = INIT_ONCE_STATIC_INIT;
Revocations onlineRevocations;

BOOL CALLBACK LoadRevocationsOnce(PINIT_ONCE, PVOID, PVOID*) noexcept {
    Revocations cached;
    if (LoadCachedRevocations(cached)) {
        ApplyRevocations(cached);
    }
    return TRUE;
}

[[nodiscard]] bool ToWide(std::string_view text, std::wstring& wide) {
    wide.clear();
    if (text.empty()) {
        return true;
    }
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(length));
    return ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        wide.data(), length) == length;
}

[[nodiscard]] int CompareVersion(format::XScalVersion left, format::XScalVersion right) noexcept {
    const std::uint32_t a = format::EncodeXScalVersion(left);
    const std::uint32_t b = format::EncodeXScalVersion(right);
    return a < b ? -1 : (a > b ? 1 : 0);
}

}  // namespace

bool DecodeBase64(std::string_view text, std::vector<std::uint8_t>& bytes) noexcept {
    bytes.clear();
    if (text.empty() || text.size() % 4 != 0) {
        return false;
    }
    try {
        bytes.reserve(text.size() / 4 * 3);
        for (std::size_t offset = 0; offset < text.size(); offset += 4) {
            const bool last = offset + 4 == text.size();
            std::uint32_t group = 0;
            int padding = 0;
            for (int index = 0; index < 4; ++index) {
                const char character = text[offset + static_cast<std::size_t>(index)];
                std::uint32_t value;
                if (character >= 'A' && character <= 'Z') value = static_cast<std::uint32_t>(character - 'A');
                else if (character >= 'a' && character <= 'z') value = static_cast<std::uint32_t>(character - 'a' + 26);
                else if (character >= '0' && character <= '9') value = static_cast<std::uint32_t>(character - '0' + 52);
                else if (character == '+') value = 62;
                else if (character == '/') value = 63;
                else if (character == '=' && last && index >= 2) {
                    value = 0;
                    ++padding;
                } else {
                    bytes.clear();
                    return false;
                }
                // Once padding starts, only padding may follow.
                if (padding > 0 && character != '=') {
                    bytes.clear();
                    return false;
                }
                group = (group << 6) | value;
            }
            // Unused low bits must be zero so each byte string has one encoding.
            if ((padding == 2 && (group & 0xFFFF) != 0) ||
                (padding == 1 && (group & 0xFF) != 0)) {
                bytes.clear();
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>(group >> 16));
            if (padding < 2) bytes.push_back(static_cast<std::uint8_t>(group >> 8));
            if (padding < 1) bytes.push_back(static_cast<std::uint8_t>(group));
        }
    } catch (...) {
        bytes.clear();
        return false;
    }
    return true;
}

std::string EncodeBase64(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    text.reserve((size + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < size; offset += 3) {
        const std::size_t count = std::min<std::size_t>(3, size - offset);
        std::uint32_t group = static_cast<std::uint32_t>(bytes[offset]) << 16;
        if (count > 1) group |= static_cast<std::uint32_t>(bytes[offset + 1]) << 8;
        if (count > 2) group |= bytes[offset + 2];
        text += kAlphabet[(group >> 18) & 63];
        text += kAlphabet[(group >> 12) & 63];
        text += count > 1 ? kAlphabet[(group >> 6) & 63] : '=';
        text += count > 2 ? kAlphabet[group & 63] : '=';
    }
    return text;
}

bool SplitFile(std::string_view text, RawFile& file, const char*& detail) noexcept {
    file = {};
    json::Document document;
    if (!json::ParseDocument(text, ParseOptions(), document)) {
        detail = "addlib.json is not valid JSON";
        return false;
    }
    const json::Value root = document.Root();
    std::int64_t format{};
    if (root.GetType() != json::Type::Object ||
        root.ReadIntegerMember("format", format) != MemberStatus::Found || format != 3) {
        detail = "addlib.json is not format 3";
        return false;
    }
    try {
        json::Value part;
        if (root.ReadObjectMember("status", part) == MemberStatus::Found) {
            file.hasStatus = ReadPart(part, false, file.status, file.statusDetail);
        } else {
            file.statusDetail = "status is missing or not an object";
        }
        const json::Value revocations = root.FindMember("revocations");
        if (!revocations) {
            file.revocationsDetail = "revocations is missing";
        } else if (revocations.GetType() != json::Type::Null) {
            file.hasRevocations = ReadPart(revocations, true, file.revocations, file.revocationsDetail);
        }
    } catch (...) {
        detail = "out of memory";
        return false;
    }
    return true;
}

PartCheck CheckStatusPart(
    const RawPart& part,
    std::uint64_t newerThan,
    Status& status,
    const char*& detail) noexcept {
    status = {};
    try {
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> signatureBytes;
        if (!DecodeBase64(part.payload, payload)) {
            detail = "payload is not base64";
            return PartCheck::BadBase64;
        }
        if (!DecodeBase64(part.signature, signatureBytes)) {
            detail = "signature is not base64";
            return PartCheck::BadBase64;
        }
        if (signatureBytes.size() != sizeof(plugins::signing::PluginSignature)) {
            detail = "signature is not 277 bytes";
            return PartCheck::BadSignature;
        }
        plugins::signing::PluginSignature signature{};
        std::memcpy(&signature, signatureBytes.data(), sizeof(signature));
        const auto result = plugins::signing::CheckSignedData(
            payload.data(), payload.size(), signature);
        if (result.check != plugins::signing::SignatureCheck::Valid) {
            detail = plugins::signing::DescribeCheck(result.check);
            return PartCheck::BadSignature;
        }
        if (signature.certificate.idPrefix[0] != '\0') {
            detail = "certificate has an ID prefix";
            return PartCheck::BadSignature;
        }
        if (!ParseStatusPayload(
                std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()),
                status, detail)) {
            status = {};
            return PartCheck::Malformed;
        }
        return status.sequence > newerThan ? PartCheck::Accepted : PartCheck::NotNewer;
    } catch (...) {
        status = {};
        detail = "out of memory";
        return PartCheck::Malformed;
    }
}

PartCheck CheckRevocationsPart(
    const RawPart& part,
    std::uint64_t newerThan,
    Revocations& revocations,
    const char*& detail) noexcept {
    revocations = {};
    try {
        std::vector<std::uint8_t> payload;
        std::vector<std::uint8_t> signatureBytes;
        if (!DecodeBase64(part.payload, payload)) {
            detail = "payload is not base64";
            return PartCheck::BadBase64;
        }
        if (!DecodeBase64(part.signature, signatureBytes)) {
            detail = "signature is not base64";
            return PartCheck::BadBase64;
        }
        std::uint8_t signature[plugins::signing::kSignatureSize];
        if (signatureBytes.size() != sizeof(signature)) {
            detail = "signature is not 64 bytes";
            return PartCheck::BadSignature;
        }
        std::memcpy(signature, signatureBytes.data(), sizeof(signature));
        const auto check = plugins::signing::CheckRootSignedData(
            kRevocationsContext, payload.data(), payload.size(), part.rootKeyIndex, signature);
        if (check != plugins::signing::SignatureCheck::Valid) {
            detail = plugins::signing::DescribeCheck(check);
            return PartCheck::BadSignature;
        }
        if (!ParseRevocationsPayload(
                std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()),
                revocations, detail)) {
            revocations = {};
            return PartCheck::Malformed;
        }
        return revocations.sequence > newerThan ? PartCheck::Accepted : PartCheck::NotNewer;
    } catch (...) {
        revocations = {};
        detail = "out of memory";
        return PartCheck::Malformed;
    }
}

const Build* FindBuild(
    const Status& status,
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable) noexcept {
    for (const Build& build : status.builds) {
        if (build.target == target && build.runtime == runtime &&
            build.executable == executable) {
            return &build;
        }
    }
    return nullptr;
}

const BlockedRevision* FindBlockedRevision(
    const Build& build,
    format::XScalVersion release,
    std::uint32_t revision) noexcept {
    for (const BlockedRevision& blocked : build.blockedRevisions) {
        if (blocked.revision == revision && CompareVersion(blocked.release, release) == 0) {
            return &blocked;
        }
    }
    return nullptr;
}

const DatabaseEntry* FindDatabaseFor(
    const Build& build,
    format::XScalVersion running) noexcept {
    if (build.databases.empty()) {
        return build.hasDatabase ? &build.database : nullptr;
    }
    const DatabaseEntry* best = nullptr;
    for (const DatabaseEntry& entry : build.databases) {
        if (format::IsVersionNewer(entry.minimum, running)) {
            continue;
        }
        if (best == nullptr) {
            best = &entry;
            continue;
        }
        const int byRelease = CompareVersion(entry.release, best->release);
        if (byRelease > 0 || (byRelease == 0 && entry.revision > best->revision)) {
            best = &entry;
        }
    }
    return best;
}

const Message* FindMessage(
    const std::vector<Message>& messages,
    std::string_view id) noexcept {
    if (id.empty()) {
        return nullptr;
    }
    for (const Message& message : messages) {
        if (message.id == id) {
            return &message;
        }
    }
    return nullptr;
}

bool IsMessageCurrent(
    const Message& message,
    format::XScalVersion xscal,
    std::uint64_t nowFileTime) noexcept {
    if (message.hasMinXScal && CompareVersion(xscal, message.minXScal) < 0) {
        return false;
    }
    if (message.hasMaxXScal && CompareVersion(xscal, message.maxXScal) > 0) {
        return false;
    }
    return message.expires == 0 || nowFileTime < message.expires;
}

bool ClaimMessage(
    const Message& message,
    std::uint64_t nowFileTime) noexcept {
    const format::XScalVersion running{
        config::kXScalVersion.major, config::kXScalVersion.minor, config::kXScalVersion.patch};
    if (!IsMessageCurrent(message, running, nowFileTime)) {
        return false;
    }
    ::AcquireSRWLockExclusive(&launchLock);
    bool first = false;
    try {
        first = std::find(launchShown.begin(), launchShown.end(), message.id) == launchShown.end();
        if (first) {
            launchShown.push_back(message.id);
        }
    } catch (...) {
        first = false;
    }
    ::ReleaseSRWLockExclusive(&launchLock);
    return first;
}

bool BuildMessageText(
    const Message& message,
    std::wstring_view lead,
    std::wstring& text,
    UINT& icon) noexcept {
    try {
        std::string body = message.text;
        if (!message.url.empty()) {
            body += "\n\n" + message.url;
        }
        std::wstring wideBody;
        if (!ToWide(body, wideBody)) {
            return false;
        }
        text.assign(lead);
        if (!text.empty()) {
            text += L"\n\n";
        }
        text += wideBody;
    } catch (...) {
        return false;
    }
    icon = message.severity == Severity::Info ? MB_ICONINFORMATION : MB_ICONEXCLAMATION;
    return true;
}

bool LoadCachedStatus(Status& status) noexcept {
    status = {};
    try {
        std::string text;
        RawPart part;
        const char* detail = nullptr;
        if (!ReadCacheFile(kStatusFile, text)) {
            return false;
        }
        if (!ParsePartFile(text, false, part, detail) ||
            CheckStatusPart(part, 0, status, detail) != PartCheck::Accepted) {
            DiagnosticLogFormat("Address Library: cached v3-status.json ignored (%s)", detail);
            status = {};
            return false;
        }
        return true;
    } catch (...) {
        status = {};
        return false;
    }
}

bool LoadCachedRevocations(Revocations& revocations) noexcept {
    revocations = {};
    try {
        std::string text;
        RawPart part;
        const char* detail = nullptr;
        if (!ReadCacheFile(kRevocationsFile, text)) {
            return false;
        }
        if (!ParsePartFile(text, true, part, detail) ||
            CheckRevocationsPart(part, 0, revocations, detail) != PartCheck::Accepted) {
            DiagnosticLogFormat("Address Library: cached v3-revocations.json ignored (%s)", detail);
            revocations = {};
            return false;
        }
        return true;
    } catch (...) {
        revocations = {};
        return false;
    }
}

bool StoreStatusPart(const RawPart& part) noexcept {
    try {
        return WriteCacheFile(kStatusFile, SerializePart(part, false));
    } catch (...) {
        return false;
    }
}

bool StoreRevocationsPart(const RawPart& part) noexcept {
    try {
        return WriteCacheFile(kRevocationsFile, SerializePart(part, true));
    } catch (...) {
        return false;
    }
}

std::uint32_t CachedCheckHours(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    const ExecutableIdentity& executable) noexcept {
    Status status;
    if (!LoadCachedStatus(status)) {
        return kDefaultCheckHours;
    }
    const Build* const build = FindBuild(status, target, runtime, executable);
    return build != nullptr ? build->checkAgainHours : kDefaultCheckHours;
}

void ApplyCachedRevocations() noexcept {
    (void)::InitOnceExecuteOnce(&revocationsOnce, &LoadRevocationsOnce, nullptr, nullptr);
}

void ApplyRevocations(const Revocations& revocations) noexcept {
    try {
        std::vector<std::uint32_t> developers;
        std::vector<Hash> hashes;
        for (const auto& developer : revocations.developers) developers.push_back(developer.id);
        for (const auto& entry : revocations.pluginHashes) hashes.push_back(entry.sha256);
        onlineRevocations = revocations;
        if (!plugins::signing::SetOnlineRevocations(developers, hashes)) {
            onlineRevocations = {};
            DiagnosticLog("Address Library: online revocations could not be applied (out of memory)");
            return;
        }
        DiagnosticLogFormat(
            "Address Library: online revocations sequence %llu (%zu developers, %zu plugin builds)",
            static_cast<unsigned long long>(revocations.sequence),
            revocations.developers.size(),
            revocations.pluginHashes.size());
    } catch (...) {
        onlineRevocations = {};
        (void)plugins::signing::SetOnlineRevocations({}, {});
    }
}

bool FindOnlineRejection(
    const plugins::signing::SignatureResult& result,
    OnlineRejection& rejection) noexcept {
    rejection = {};
    if (!result.onlineRevocation) {
        return false;
    }
    if (result.check == plugins::signing::SignatureCheck::DeveloperRevoked) {
        for (const auto& developer : onlineRevocations.developers) {
            if (developer.id == result.developerId) {
                rejection.reason = developer.reason.c_str();
                rejection.message = FindMessage(onlineRevocations.messages, developer.messageId);
                return true;
            }
        }
    } else if (result.check == plugins::signing::SignatureCheck::BuildRevoked) {
        for (const auto& entry : onlineRevocations.pluginHashes) {
            if (entry.sha256 == result.fileHash) {
                rejection.reason = entry.reason.c_str();
                rejection.message = FindMessage(onlineRevocations.messages, entry.messageId);
                return true;
            }
        }
    }
    return false;
}

}  // namespace sf::address::v3
