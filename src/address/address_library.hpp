#pragma once

#include "address/address_library_format.hpp"
#include "config/target_profile.hpp"
#include "plugins/plugin_signature.hpp"
#include "xscal/plugin_api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf::address {

enum class LoadStatus {
    Loaded,
    Missing,
    ReadFailed,
    RequiresNewerXScal,
    Invalid,
    // The signature is missing, does not verify, or was issued by a
    // certificate with an ID prefix. LastSignatureCheck() has the reason.
    BadSignature,
    // The file was made for another build of the same runtime version.
    // FileExecutable() has the build the file expects.
    WrongExecutable,
};

// IMAGE_FILE_HEADER::TimeDateStamp and IMAGE_OPTIONAL_HEADER::SizeOfImage.
struct ExecutableIdentity final {
    std::uint32_t timeDateStamp{};
    std::uint32_t imageSize{};

    [[nodiscard]] constexpr bool operator==(
        const ExecutableIdentity&) const noexcept = default;
};

// Reads the PE headers of the module mapped at moduleBase. Does not touch
// any section.
[[nodiscard]] bool ReadExecutableIdentity(
    std::uintptr_t moduleBase,
    ExecutableIdentity& identity) noexcept;

[[nodiscard]] bool DownloadAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string& contents) noexcept;
[[nodiscard]] bool StoreAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string_view contents) noexcept;
// The "declined" marker next to the database stores the release and revision
// of the server file the user declined. IsUpdateDeclined is true unless the
// given file is newer than the declined one.
[[nodiscard]] bool IsUpdateDeclined(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    format::XScalVersion release,
    std::uint32_t revision) noexcept;
[[nodiscard]] bool StoreDeclinedUpdate(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    format::XScalVersion release,
    std::uint32_t revision) noexcept;

// The "checked" marker stores when the server was last asked about this
// runtime (a FILETIME as a decimal number) and the xScal version that asked.
// A check is due when the marker is missing or unreadable, was written by
// another xScal version, is 24 hours or more old, or lies in the future.
[[nodiscard]] bool IsUpdateCheckDue(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uint64_t nowFileTime) noexcept;
[[nodiscard]] bool StoreUpdateCheck(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::uint64_t nowFileTime) noexcept;

class Library final {
public:
    Library() noexcept;

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] LoadStatus Load(
        TargetKind target,
        const config::RuntimeVersion& runtime,
        const ExecutableIdentity& executable,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] LoadStatus Parse(
        std::string_view contents,
        TargetKind target,
        const config::RuntimeVersion& runtime,
        const ExecutableIdentity& executable,
        std::uintptr_t moduleBase) noexcept;

    [[nodiscard]] bool ResolveAddress(
        std::uint64_t id,
        std::uintptr_t& address) const noexcept;
    [[nodiscard]] bool LookupRva(
        std::uint64_t id,
        std::uint64_t& rva) const noexcept;
    [[nodiscard]] bool LookupConstant(
        std::uint64_t id,
        std::uint64_t& value) const noexcept;
    [[nodiscard]] std::size_t EntryCount() const noexcept;
    [[nodiscard]] format::XScalVersion RequiredXScalVersion() const noexcept;
    // The xScal release the file was published with.
    [[nodiscard]] format::XScalVersion ReleaseXScalVersion() const noexcept;
    [[nodiscard]] std::uint32_t Revision() const noexcept;
    // Result of the last Parse that reached the signature check.
    [[nodiscard]] plugins::signing::SignatureCheck LastSignatureCheck() const noexcept;
    // The executable named in the header of the last parsed file; set when
    // Parse returned WrongExecutable.
    [[nodiscard]] ExecutableIdentity FileExecutable() const noexcept;
    [[nodiscard]] const XScalAddressLibraryV1* Interface() const noexcept;

private:
    enum class EntryKind : std::uint8_t {
        Address,
        Constant,
    };

    struct Entry final {
        std::uint64_t id;
        std::uint64_t value;
        EntryKind kind;
    };

    [[nodiscard]] const Entry* Find(std::uint64_t id) const noexcept;
    static int32_t __cdecl ResolveAddressCallback(
        void* context,
        std::uint64_t id,
        std::uintptr_t* address) noexcept;
    static int32_t __cdecl LookupRvaCallback(
        void* context,
        std::uint64_t id,
        std::uint64_t* rva) noexcept;
    static int32_t __cdecl LookupConstantCallback(
        void* context,
        std::uint64_t id,
        std::uint64_t* value) noexcept;

    std::vector<Entry> entries_;
    std::uintptr_t moduleBase_{};
    format::XScalVersion requiredXScalVersion_{};
    format::XScalVersion releaseXScalVersion_{};
    std::uint32_t revision_{};
    plugins::signing::SignatureCheck lastSignatureCheck_{
        plugins::signing::SignatureCheck::MissingSignature};
    ExecutableIdentity fileExecutable_{};
    XScalAddressLibraryV1 interface_{};
};

}  // namespace sf::address
