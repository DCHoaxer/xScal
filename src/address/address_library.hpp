#pragma once

#include "address/address_library_format.hpp"
#include "config/target_profile.hpp"
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
};

[[nodiscard]] bool DownloadAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string& contents) noexcept;
[[nodiscard]] bool StoreAddressLibrary(
    TargetKind target,
    const config::RuntimeVersion& runtime,
    std::string_view contents) noexcept;

class Library final {
public:
    Library() noexcept;

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    [[nodiscard]] LoadStatus Load(
        TargetKind target,
        const config::RuntimeVersion& runtime,
        std::uintptr_t moduleBase) noexcept;
    [[nodiscard]] bool Parse(
        std::string_view contents,
        TargetKind target,
        const config::RuntimeVersion& runtime,
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

    [[nodiscard]] LoadStatus ParseContents(
        std::string_view contents,
        TargetKind target,
        const config::RuntimeVersion& runtime,
        std::uintptr_t moduleBase) noexcept;

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
    XScalAddressLibraryV1 interface_{};
};

}  // namespace sf::address
