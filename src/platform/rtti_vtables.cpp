#include "platform/rtti_vtables.hpp"

#include "platform/native_access.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace sf::platform {
namespace {

constexpr std::size_t kMaximumSections = 96;
constexpr std::size_t kTypeDescriptorNameOffset = 0x10;
constexpr std::uint32_t kLocatorSignatureX64 = 1;

// MSVC x64 RTTI CompleteObjectLocator. The last three fields are RVAs; the
// locator's own RVA in self tells a real locator from matching data.
struct CompleteObjectLocator final {
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t constructorDisplacementOffset;
    std::uint32_t typeDescriptor;
    std::uint32_t classDescriptor;
    std::uint32_t self;
};

struct DataSection final {
    std::uintptr_t begin;
    std::uintptr_t end;
    bool writable;
};

struct DataSections final {
    std::array<DataSection, kMaximumSections> sections{};
    std::size_t count{};
};

struct QueryMatches final {
    std::uint32_t typeDescriptorRva;
    std::uint32_t typeDescriptorCount;
    std::uint32_t locatorRva;
    std::uint32_t locatorCount;
    std::uintptr_t vtableRva;
    std::uint32_t vtableCount;
};

struct ScanState final {
    std::uintptr_t moduleBase;
    RttiVtableQuery* queries;
    std::size_t count;
    std::array<QueryMatches, kMaximumRttiVtableQueries> matches;
};

using RegionScan = bool (*)(
    std::uintptr_t begin,
    std::uintptr_t end,
    ScanState& state) noexcept;

// Exception data, resources and relocations never hold RTTI. In the 1.7.27.11
// image they are 5.9 MB of the 27.4 MB of read-only data the locator and
// vtable passes would otherwise read.
[[nodiscard]] bool IsSectionWithoutRtti(
    const IMAGE_SECTION_HEADER& section) noexcept {
    constexpr std::array<std::string_view, 3> kNames{".pdata", ".rsrc", ".reloc"};
    const std::string_view name{
        reinterpret_cast<const char*>(section.Name),
        strnlen(reinterpret_cast<const char*>(section.Name),
                IMAGE_SIZEOF_SHORT_NAME)};
    return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
}

[[nodiscard]] bool ReadDataSections(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    DataSections& image) noexcept {
    image.count = 0;
    IMAGE_DOS_HEADER dos{};
    if (!ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(moduleBase),
            &dos,
            sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return false;
    }

    std::uintptr_t ntAddress{};
    IMAGE_NT_HEADERS64 nt{};
    if (!TryAddAddress(
            moduleBase, static_cast<std::size_t>(dos.e_lfanew), ntAddress) ||
        !ReadBytes(
            virtualQuery,
            reinterpret_cast<const void*>(ntAddress),
            &nt,
            sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > kMaximumSections) {
        return false;
    }

    const std::uintptr_t sectionTable = ntAddress +
        offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    for (WORD index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!ReadBytes(
                virtualQuery,
                reinterpret_cast<const void*>(
                    sectionTable + index * sizeof(section)),
                &section,
                sizeof(section))) {
            return false;
        }
        const DWORD characteristics = section.Characteristics;
        const std::uint64_t sectionEnd =
            static_cast<std::uint64_t>(section.VirtualAddress) +
            section.Misc.VirtualSize;
        if (IsSectionWithoutRtti(section) ||
            (characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            (characteristics &
             (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)) != 0 ||
            section.VirtualAddress == 0 || section.Misc.VirtualSize == 0 ||
            sectionEnd > nt.OptionalHeader.SizeOfImage) {
            continue;
        }
        image.sections[image.count++] = {
            moduleBase + section.VirtualAddress,
            moduleBase + static_cast<std::uintptr_t>(sectionEnd),
            (characteristics & IMAGE_SCN_MEM_WRITE) != 0};
    }
    return image.count != 0;
}

// Calls scan for every committed, readable part of the selected sections.
[[nodiscard]] bool ScanSections(
    VirtualQueryFn virtualQuery,
    const DataSections& image,
    bool writable,
    RegionScan scan,
    ScanState& state) noexcept {
    for (std::size_t index = 0; index < image.count; ++index) {
        const auto& section = image.sections[index];
        if (section.writable != writable) {
            continue;
        }
        auto current = section.begin;
        while (current < section.end) {
            MEMORY_BASIC_INFORMATION region{};
            if (virtualQuery(
                    reinterpret_cast<const void*>(current),
                    &region,
                    sizeof(region)) == 0) {
                return false;
            }
            const auto regionBase =
                reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            if (region.RegionSize == 0 ||
                region.RegionSize >
                    (std::numeric_limits<std::uintptr_t>::max)() - regionBase ||
                regionBase + region.RegionSize <= current) {
                return false;
            }
            const auto regionEnd =
                (std::min)(regionBase + region.RegionSize, section.end);
            if (region.State == MEM_COMMIT &&
                IsReadableProtection(region.Protect) &&
                !scan(current, regionEnd, state)) {
                return false;
            }
            current = regionEnd;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::uintptr_t AlignUp(
    std::uintptr_t value,
    std::uintptr_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// TypeDescriptors are 8-byte aligned: the type_info vtable pointer, a spare
// field that is always zero, then the decorated name at +0x10.
[[nodiscard]] bool ScanTypeDescriptors(
    std::uintptr_t begin,
    std::uintptr_t end,
    ScanState& state) noexcept {
    __try {
        for (auto descriptor = AlignUp(begin, alignof(std::uint64_t));
             descriptor < end && end - descriptor > kTypeDescriptorNameOffset;
             descriptor += alignof(std::uint64_t)) {
            const auto* const name = reinterpret_cast<const char*>(
                descriptor + kTypeDescriptorNameOffset);
            if (name[0] != '.') {
                continue;
            }
            std::uint64_t header[2];
            std::memcpy(
                header, reinterpret_cast<const void*>(descriptor), sizeof(header));
            if (header[0] == 0 || header[1] != 0) {
                continue;
            }
            const auto available = end - (descriptor + kTypeDescriptorNameOffset);
            for (std::size_t query = 0; query < state.count; ++query) {
                const auto typeName = state.queries[query].typeName;
                if (typeName.size() + 1 > available ||
                    std::memcmp(name, typeName.data(), typeName.size()) != 0 ||
                    name[typeName.size()] != '\0') {
                    continue;
                }
                auto& matches = state.matches[query];
                matches.typeDescriptorRva =
                    static_cast<std::uint32_t>(descriptor - state.moduleBase);
                ++matches.typeDescriptorCount;
            }
        }
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

[[nodiscard]] bool ScanLocators(
    std::uintptr_t begin,
    std::uintptr_t end,
    ScanState& state) noexcept {
    __try {
        for (auto address = AlignUp(begin, alignof(std::uint32_t));
             address < end && end - address >= sizeof(CompleteObjectLocator);
             address += alignof(std::uint32_t)) {
            CompleteObjectLocator locator;
            std::memcpy(
                &locator, reinterpret_cast<const void*>(address), sizeof(locator));
            const auto rva = address - state.moduleBase;
            if (locator.signature != kLocatorSignatureX64 ||
                locator.self != rva) {
                continue;
            }
            for (std::size_t query = 0; query < state.count; ++query) {
                auto& matches = state.matches[query];
                if (matches.typeDescriptorCount != 1 ||
                    locator.typeDescriptor != matches.typeDescriptorRva ||
                    locator.offset != state.queries[query].subobjectOffset) {
                    continue;
                }
                matches.locatorRva = static_cast<std::uint32_t>(rva);
                ++matches.locatorCount;
            }
        }
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

// The vtable's slot -1 holds the absolute address of its locator.
[[nodiscard]] bool ScanVtables(
    std::uintptr_t begin,
    std::uintptr_t end,
    ScanState& state) noexcept {
    __try {
        for (auto slot = AlignUp(begin, alignof(std::uint64_t));
             slot < end && end - slot >= sizeof(std::uint64_t);
             slot += alignof(std::uint64_t)) {
            std::uint64_t value;
            std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
            for (std::size_t query = 0; query < state.count; ++query) {
                auto& matches = state.matches[query];
                if (matches.locatorCount != 1 ||
                    value != state.moduleBase + matches.locatorRva) {
                    continue;
                }
                matches.vtableRva =
                    slot + sizeof(std::uint64_t) - state.moduleBase;
                ++matches.vtableCount;
            }
        }
    } __except (MemoryAccessExceptionFilter(GetExceptionCode())) {
        return false;
    }
    return true;
}

}

bool FindRttiVtables(
    VirtualQueryFn virtualQuery,
    std::uintptr_t moduleBase,
    RttiVtableQuery* queries,
    std::size_t count) noexcept {
    if (queries == nullptr) {
        return false;
    }
    for (std::size_t query = 0; query < count; ++query) {
        queries[query].vtableRva = 0;
    }
    if (virtualQuery == nullptr || moduleBase == 0 || count == 0 ||
        count > kMaximumRttiVtableQueries) {
        return false;
    }
    for (std::size_t query = 0; query < count; ++query) {
        if (queries[query].typeName.empty() ||
            queries[query].typeName.front() != '.') {
            return false;
        }
    }

    DataSections image;
    ScanState state{
        .moduleBase = moduleBase,
        .queries = queries,
        .count = count,
        .matches = {},
    };
    if (!ReadDataSections(virtualQuery, moduleBase, image) ||
        !ScanSections(virtualQuery, image, true, &ScanTypeDescriptors, state) ||
        !ScanSections(virtualQuery, image, false, &ScanLocators, state) ||
        !ScanSections(virtualQuery, image, false, &ScanVtables, state)) {
        return false;
    }

    bool foundAll = true;
    for (std::size_t query = 0; query < count; ++query) {
        const auto& matches = state.matches[query];
        if (matches.typeDescriptorCount == 1 && matches.locatorCount == 1 &&
            matches.vtableCount == 1) {
            queries[query].vtableRva = matches.vtableRva;
        } else {
            foundAll = false;
        }
    }
    return foundAll;
}

}
