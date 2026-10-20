#include "archive/archive_stream_hook.hpp"

#include "hook/vtable_hook.hpp"
#include "platform/diagnostics.hpp"
#include "platform/native_access.hpp"

#include <Windows.h>

#include <libdeflate.h>

#include <atomic>
#include <cstddef>
#include <cstring>

namespace sf::archive {
namespace {

// zlib's z_stream on 64-bit Windows, where uLong is 32 bits.
constexpr std::size_t kAvailInOffset = 0x8;
constexpr std::size_t kTotalInOffset = 0xC;
constexpr std::size_t kNextOutOffset = 0x10;
constexpr std::size_t kTotalOutOffset = 0x1C;
// A buffered stream keeps its Entry in next_out, and total_out holds this
// value. inflateEnd does not read either field, and the game's zlib never runs
// on such a stream again. A real total_out cannot reach this value because
// entries larger than kMaxEntryBytes stay with the game.
constexpr std::uint32_t kEntryMarker = 0xFA57DEF1u;
constexpr std::uint32_t kMaxEntryBytes = 256u * 1024u * 1024u;
constexpr std::uint32_t kMaxPackedBytes = 64u * 1024u * 1024u;
// Same value as SEEK_CUR. A zlib stream supports only a forward skip from the
// current position.
constexpr std::uint32_t kSeekCurrent = 1;
constexpr ULONGLONG kTotalsIntervalMs = 60000;

using Read = std::uint32_t (*)(void* stream, void* buffer, std::uint64_t length,
    std::uint64_t* bytesRead);
using Seek = std::uint32_t (*)(void* stream, std::int64_t offset, std::uint32_t whence,
    std::uint64_t* newPosition);
using Close = void (*)(void* stream);
using BackingRead = std::uint32_t (*)(void* backing, void* buffer, std::uint64_t length,
    std::uint64_t offset, std::uint64_t* bytesRead);

// One decompressed entry, allocated together with its data.
struct Entry final {
    std::uintptr_t stream;
    std::uintptr_t state;
    std::uint32_t size;
    std::uint32_t position;
    std::uint8_t* data() noexcept { return reinterpret_cast<std::uint8_t*>(this + 1); }
};

config::ArchiveStreamProfile layout{};
VtableHook readHook;
VtableHook seekHook;
VtableHook closeHook;
Read gameRead{};
Seek gameSeek{};
Close gameClose{};
// Set when all three slots are replaced. Without it the replacements only
// forward, because a stream buffered through one slot must not reach the
// game's code through another.
std::atomic<bool> installed{false};
std::atomic<ULONGLONG> nextTotalsTick{0};

std::atomic<std::uint64_t> entryCount{0};
std::atomic<std::uint64_t> entryBytes{0};
std::atomic<std::uint64_t> entryTicks{0};
std::atomic<std::uint64_t> bufferedReads{0};
std::atomic<std::uint64_t> fallbackCount{0};

template <typename T>
[[nodiscard]] T field(std::uintptr_t base, std::size_t offset) noexcept
{
    return *reinterpret_cast<const volatile T*>(base + offset);
}

template <typename T>
void setField(std::uintptr_t base, std::size_t offset, T value) noexcept
{
    *reinterpret_cast<volatile T*>(base + offset) = value;
}

[[nodiscard]] std::uint32_t unsupportedResult() noexcept
{
    return static_cast<std::uint32_t>(layout.unsupportedResult);
}

[[nodiscard]] std::uint64_t ticks() noexcept
{
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}

[[nodiscard]] std::uint64_t ticksToMicroseconds(std::uint64_t value) noexcept
{
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER result{};
        QueryPerformanceFrequency(&result);
        return static_cast<std::uint64_t>(result.QuadPart);
    }();
    return frequency == 0 ? 0 : value * 1000000 / frequency;
}

// Per-thread decompressor and packed-data buffer, kept for the thread's life.
struct ThreadScratch final {
    libdeflate_decompressor* decompressor{};
    void* packed{};
    std::uint32_t capacity{};

    ~ThreadScratch()
    {
        if (decompressor != nullptr)
            libdeflate_free_decompressor(decompressor);
        if (packed != nullptr)
            VirtualFree(packed, 0, MEM_RELEASE);
    }

    [[nodiscard]] void* packedBuffer(std::uint32_t size) noexcept
    {
        if (size <= capacity)
            return packed;
        if (packed != nullptr)
            VirtualFree(packed, 0, MEM_RELEASE);
        // Grow in 1 MB steps so that most entries reuse the buffer.
        const std::uint32_t rounded = (size + 0xFFFFFu) & ~0xFFFFFu;
        packed = VirtualAlloc(nullptr, rounded, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        capacity = packed != nullptr ? rounded : 0;
        return packed;
    }
};

thread_local ThreadScratch scratch;

[[nodiscard]] std::uintptr_t streamState(std::uintptr_t stream) noexcept
{
    return field<std::uintptr_t>(stream, layout.stateOffset);
}

[[nodiscard]] bool isZlibStream(std::uintptr_t stream) noexcept
{
    const auto flags = field<std::uint32_t>(stream, layout.flagsOffset);
    return (flags & layout.zlibFlag) != 0 && (flags & layout.unsupportedFlags) == 0;
}

// The Entry of a buffered stream, or null.
[[nodiscard]] Entry* findEntry(std::uintptr_t stream) noexcept
{
    const std::uintptr_t state = streamState(stream);
    if (state == 0 || field<std::uint32_t>(state, kTotalOutOffset) != kEntryMarker)
        return nullptr;
    auto* const entry = reinterpret_cast<Entry*>(field<std::uintptr_t>(state, kNextOutOffset));
    return entry != nullptr && entry->stream == stream && entry->state == state ? entry
                                                                                : nullptr;
}

// A zlib stream on which nothing has been read since it was opened.
[[nodiscard]] bool isUntouchedZlibStream(std::uintptr_t stream) noexcept
{
    if (!isZlibStream(stream))
        return false;
    const std::uintptr_t state = streamState(stream);
    const auto backing = field<std::uintptr_t>(stream, layout.backingOffset);
    return state != 0 && backing != 0 &&
        (field<std::uint8_t>(backing, layout.backingErrorOffset) & 1) == 0 &&
        field<std::uint64_t>(state, layout.deliveredOffset) == 0 &&
        field<std::uint32_t>(state, layout.decodedWaitingOffset) == 0 &&
        field<std::uint32_t>(state, kAvailInOffset) == 0 &&
        field<std::uint32_t>(state, kTotalInOffset) == 0 &&
        field<std::uint32_t>(state, kTotalOutOffset) == 0 &&
        field<std::uint32_t>(state, layout.issueIndexOffset) == 0 &&
        field<std::uint32_t>(state, layout.waitIndexOffset) == 0;
}

// Reads the packed entry and decompresses it into a new Entry. Leaves the
// stream unchanged and returns null on any failure.
[[nodiscard]] Entry* decompressEntry(std::uintptr_t stream) noexcept
{
    const auto packedSize = field<std::uint32_t>(stream, layout.packedSizeOffset);
    const auto unpackedSize = field<std::uint32_t>(stream, layout.unpackedSizeOffset);
    if (packedSize == 0 || packedSize > kMaxPackedBytes || unpackedSize == 0 ||
        unpackedSize > kMaxEntryBytes)
        return nullptr;
    if (scratch.decompressor == nullptr) {
        scratch.decompressor = libdeflate_alloc_decompressor();
        if (scratch.decompressor == nullptr)
            return nullptr;
    }
    void* const packed = scratch.packedBuffer(packedSize);
    if (packed == nullptr)
        return nullptr;

    const auto backing = field<std::uintptr_t>(stream, layout.backingOffset);
    const auto backingVtable = *reinterpret_cast<const std::uintptr_t*>(backing);
    const auto backingRead =
        *reinterpret_cast<const BackingRead*>(backingVtable + layout.backingReadSlot);
    std::uint64_t got = 0;
    if (backingRead(reinterpret_cast<void*>(backing), packed, packedSize,
            field<std::uint64_t>(stream, layout.entryOffsetOffset), &got) != 0 ||
        static_cast<std::uint32_t>(got) != packedSize)
        return nullptr;

    auto* const entry = static_cast<Entry*>(
        HeapAlloc(GetProcessHeap(), 0, sizeof(Entry) + unpackedSize));
    if (entry == nullptr)
        return nullptr;
    std::size_t produced = 0;
    if (libdeflate_zlib_decompress(scratch.decompressor, packed, packedSize, entry->data(),
            unpackedSize, &produced) != LIBDEFLATE_SUCCESS ||
        produced != unpackedSize) {
        HeapFree(GetProcessHeap(), 0, entry);
        return nullptr;
    }
    entry->stream = stream;
    entry->state = streamState(stream);
    entry->size = unpackedSize;
    entry->position = 0;
    return entry;
}

// Ties the entry to the stream and stops the game's zlib state: nothing is
// waiting, fed or requested, so the game would read no more packed data.
void attachEntry(std::uintptr_t stream, Entry* entry) noexcept
{
    const std::uintptr_t state = streamState(stream);
    setField<std::uint32_t>(state, layout.decodedWaitingOffset, 0);
    setField<std::uint32_t>(state, kAvailInOffset, 0);
    setField<std::uint32_t>(state, layout.packedUnfedOffset, 0);
    setField<std::uint32_t>(state, layout.packedUnrequestedOffset, 0);
    setField<std::uint64_t>(state, layout.deliveredOffset, 0);
    setField<std::uintptr_t>(state, kNextOutOffset, reinterpret_cast<std::uintptr_t>(entry));
    setField<std::uint32_t>(state, kTotalOutOffset, kEntryMarker);
}

void logTotals() noexcept
{
    const ArchiveStreamTotals totals = ReadArchiveStreamTotals();
    DiagnosticLogFormat(
        "xScal ArchiveStream: %llu entries, %llu MB, %llu ms in libdeflate, %llu reads served, %llu handed back",
        static_cast<unsigned long long>(totals.entries),
        static_cast<unsigned long long>(totals.bytes / (1024 * 1024)),
        static_cast<unsigned long long>(totals.microseconds / 1000),
        static_cast<unsigned long long>(totals.bufferedReads),
        static_cast<unsigned long long>(totals.fallbacks));
}

// Writes the totals at most once a minute, from the thread that just added an
// entry, so nothing is logged while no archive is read. The line is queued to
// the file service, not written on the game thread.
void logTotalsIfDue() noexcept
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG due = nextTotalsTick.load(std::memory_order_relaxed);
    if (now < due ||
        !nextTotalsTick.compare_exchange_strong(due, now + kTotalsIntervalMs, std::memory_order_relaxed))
        return;
    // The first entry only starts the interval.
    if (due != 0)
        logTotals();
}

// The entry for a stream: an existing one, or a new one when the stream is
// untouched. Null means the game handles the call.
[[nodiscard]] Entry* entryForCall(std::uintptr_t stream) noexcept
{
    if (Entry* const existing = findEntry(stream))
        return existing;
    if (!isUntouchedZlibStream(stream))
        return nullptr;
    const std::uint64_t start = ticks();
    Entry* const entry = decompressEntry(stream);
    if (entry == nullptr) {
        fallbackCount.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    attachEntry(stream, entry);
    entryTicks.fetch_add(ticks() - start, std::memory_order_relaxed);
    entryCount.fetch_add(1, std::memory_order_relaxed);
    entryBytes.fetch_add(entry->size, std::memory_order_relaxed);
    logTotalsIfDue();
    return entry;
}

// Moves the entry's position by up to length bytes, copying them to buffer when
// it is not null, and keeps the game's delivered count equal to the position.
[[nodiscard]] std::uint32_t advance(Entry* entry, void* buffer, std::uint64_t length) noexcept
{
    const std::uint32_t remaining = entry->size - entry->position;
    const std::uint32_t count = length < remaining ? static_cast<std::uint32_t>(length) : remaining;
    if (buffer != nullptr && count != 0)
        std::memcpy(buffer, entry->data() + entry->position, count);
    entry->position += count;
    setField<std::uint64_t>(entry->state, layout.deliveredOffset, entry->position);
    return count;
}

// True when the call is for a zlib stream and the buffered path may run.
[[nodiscard]] bool isBufferedCall(std::uintptr_t stream) noexcept
{
    return installed.load(std::memory_order_acquire) && isZlibStream(stream);
}

std::uint32_t archiveRead(
    void* streamPointer, void* buffer, std::uint64_t length, std::uint64_t* bytesRead) noexcept
{
    const auto stream = reinterpret_cast<std::uintptr_t>(streamPointer);
    if (bytesRead != nullptr && isBufferedCall(stream)) {
        if (Entry* const entry = entryForCall(stream)) {
            *bytesRead = advance(entry, buffer, length);
            bufferedReads.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
    }
    return gameRead(streamPointer, buffer, length, bytesRead);
}

std::uint32_t archiveSeek(
    void* streamPointer, std::int64_t offset, std::uint32_t whence, std::uint64_t* newPosition) noexcept
{
    const auto stream = reinterpret_cast<std::uintptr_t>(streamPointer);
    if (newPosition == nullptr || !isBufferedCall(stream))
        return gameSeek(streamPointer, offset, whence, newPosition);
    Entry* entry = findEntry(stream);
    if (entry == nullptr && whence == kSeekCurrent && offset >= 0)
        entry = entryForCall(stream);
    if (entry == nullptr)
        return gameSeek(streamPointer, offset, whence, newPosition);
    if (whence != kSeekCurrent || offset < 0)
        return unsupportedResult();
    (void)advance(entry, nullptr, static_cast<std::uint64_t>(offset));
    *newPosition = entry->position;
    return 0;
}

// Frees the stream's Entry and clears the two z_stream fields that held it.
void releaseEntry(std::uintptr_t stream) noexcept
{
    if (Entry* const entry = findEntry(stream)) {
        setField<std::uintptr_t>(entry->state, kNextOutOffset, 0);
        setField<std::uint32_t>(entry->state, kTotalOutOffset, 0);
        HeapFree(GetProcessHeap(), 0, entry);
    }
}

void archiveClose(void* streamPointer) noexcept
{
    if (installed.load(std::memory_order_acquire))
        releaseEntry(reinterpret_cast<std::uintptr_t>(streamPointer));
    gameClose(streamPointer);
}

[[nodiscard]] const char* installSlot(
    VtableHook& hook,
    std::uintptr_t slotRva,
    std::uintptr_t moduleBase,
    std::uintptr_t expected,
    void* replacement) noexcept
{
    const VtableHookStatus status = hook.InstallSlot(slotRva, moduleBase, replacement);
    if (status != VtableHookStatus::Installed)
        return VtableHookStatusName(status);
    if (reinterpret_cast<std::uintptr_t>(hook.Original()) != expected) {
        (void)hook.Restore();
        return "slot changed during install";
    }
    return nullptr;
}

}

bool InstallArchiveStreamHook(
    const config::ArchiveStreamProfile& profile,
    std::uintptr_t moduleBase) noexcept
{
    if (profile.readerStreamVtable == 0) {
        DiagnosticLog("xScal ArchiveStream: no archive stream records for this build; archives stay with the game's zlib");
        return false;
    }
    std::uintptr_t vtable{};
    std::uintptr_t readAddress{};
    std::uintptr_t seekAddress{};
    std::uintptr_t closeAddress{};
    if (!platform::ResolveRequiredRvas(moduleBase, {
            {profile.readerStreamVtable, &vtable},
            {profile.read, &readAddress},
            {profile.seek, &seekAddress},
            {profile.close, &closeAddress},
        }) ||
        !platform::AreReadableAddresses(&::VirtualQuery,
            {vtable + profile.closeSlot, vtable + profile.readSlot, vtable + profile.seekSlot})) {
        DiagnosticLog("xScal ArchiveStream: the archive stream vtable is not readable; archives stay with the game's zlib");
        return false;
    }
    // All three slots must hold the game's functions before any is replaced.
    const auto slotValue = [vtable](std::size_t slot) {
        return *reinterpret_cast<const volatile std::uintptr_t*>(vtable + slot);
    };
    if (slotValue(profile.readSlot) != readAddress || slotValue(profile.seekSlot) != seekAddress ||
        slotValue(profile.closeSlot) != closeAddress) {
        DiagnosticLog("xScal ArchiveStream: the archive stream vtable does not hold the game's Read, Seek and Close; archives stay with the game's zlib");
        return false;
    }
    layout = profile;
    gameRead = reinterpret_cast<Read>(readAddress);
    gameSeek = reinterpret_cast<Seek>(seekAddress);
    gameClose = reinterpret_cast<Close>(closeAddress);

    // Close first, then Seek, then Read: a stream gets an Entry only through
    // Read or Seek, so Close always knows how to free it.
    const char* failed = installSlot(closeHook, profile.readerStreamVtable + profile.closeSlot,
        moduleBase, closeAddress, reinterpret_cast<void*>(&archiveClose));
    const char* slot = "Close";
    if (failed == nullptr) {
        failed = installSlot(seekHook, profile.readerStreamVtable + profile.seekSlot,
            moduleBase, seekAddress, reinterpret_cast<void*>(&archiveSeek));
        slot = "Seek";
    }
    if (failed == nullptr) {
        failed = installSlot(readHook, profile.readerStreamVtable + profile.readSlot,
            moduleBase, readAddress, reinterpret_cast<void*>(&archiveRead));
        slot = "Read";
    }
    if (failed != nullptr) {
        // The slots already replaced keep forwarding if they cannot be restored.
        (void)seekHook.Restore();
        (void)closeHook.Restore();
        DiagnosticLogFormat(
            "xScal ArchiveStream: replacing the %s slot failed (%s); archives stay with the game's zlib",
            slot, failed);
        return false;
    }
    installed.store(true, std::memory_order_release);
    DiagnosticLog("xScal ArchiveStream Installed");
    return true;
}

ArchiveStreamTotals ReadArchiveStreamTotals() noexcept
{
    ArchiveStreamTotals totals;
    totals.entries = entryCount.load(std::memory_order_relaxed);
    totals.bytes = entryBytes.load(std::memory_order_relaxed);
    totals.microseconds = ticksToMicroseconds(entryTicks.load(std::memory_order_relaxed));
    totals.bufferedReads = bufferedReads.load(std::memory_order_relaxed);
    totals.fallbacks = fallbackCount.load(std::memory_order_relaxed);
    return totals;
}

void SetArchiveStreamLayoutForTest(const config::ArchiveStreamProfile& profile) noexcept
{
    layout = profile;
}

std::uint32_t ReadBufferedForTest(
    void* stream, void* buffer, std::uint64_t length, std::uint64_t* bytesRead) noexcept
{
    Entry* const entry = entryForCall(reinterpret_cast<std::uintptr_t>(stream));
    if (entry == nullptr)
        return unsupportedResult();
    *bytesRead = advance(entry, buffer, length);
    return 0;
}

std::uint32_t SeekBufferedForTest(void* stream, std::int64_t offset, std::uint64_t* newPosition) noexcept
{
    Entry* const entry = entryForCall(reinterpret_cast<std::uintptr_t>(stream));
    if (entry == nullptr || offset < 0)
        return unsupportedResult();
    (void)advance(entry, nullptr, static_cast<std::uint64_t>(offset));
    *newPosition = entry->position;
    return 0;
}

void CloseBufferedForTest(void* stream) noexcept
{
    releaseEntry(reinterpret_cast<std::uintptr_t>(stream));
}

}
