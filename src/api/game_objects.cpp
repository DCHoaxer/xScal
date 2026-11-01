#include "api/game_objects.hpp"

#include "common/json.hpp"
#include "scaleform/json_as3_value.hpp"
#include "scaleform/scaleform_layout.hpp"
#include "scaleform/value.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <mutex>
#include <string>

namespace sf {
namespace {

// GFx::Value::VT_Null.
constexpr std::uint8_t kNullType = 1;
constexpr std::size_t kMaximumCachedMovies = 16;

constexpr json::ParseOptions kGameJsonOptions{
    json::NumberSyntax::Any,
    layout::kMaxReturnStringLength,
    json::kMaximumParserDepth,
    false,
};

// Decimal text of an index, used as a member name on arrays and on the holder.
struct IndexName final {
    char text[12]{};

    explicit IndexName(std::uint32_t index) noexcept {
        const auto result = std::to_chars(text, text + sizeof(text) - 1, index);
        *result.ptr = '\0';
    }
};

class ScaleformValueWriter final {
public:
    using Value = ScaleformValue;

    explicit ScaleformValueWriter(const MovieRootContext& context) noexcept
        : context_{context} {}

    bool Null(Value& value) noexcept {
        InitializeScaleformValue(value, kNullType, std::uint64_t{});
        return true;
    }

    bool Boolean(Value& value, bool boolean) noexcept {
        InitializeScaleformBoolean(value, boolean);
        return true;
    }

    bool Int(Value& value, std::int32_t integer) noexcept {
        InitializeScaleformValue(value, layout::kIntType, integer);
        return true;
    }

    bool Number(Value& value, double number) noexcept {
        InitializeScaleformValue(value, layout::kNumberType, number);
        return true;
    }

    // SetMember copies the text into the movie, so the borrowed pointer only
    // has to outlive the SetMember call.
    bool String(Value& value, const char* text) noexcept {
        InitializeBorrowedScaleformString(value, text);
        return true;
    }

    bool Object(Value& value) noexcept {
        return CreateUnnamedScaleformObject(context_, value);
    }

    bool Array(Value& value) noexcept {
        return CreateScaleformObject(context_, "Array", nullptr, 0, value);
    }

    bool SetMember(Value& object, const char* name, Value& member) noexcept {
        return SetScaleformMember(context_, object, name, member);
    }

    // ActionScript arrays take numeric member names as indexes.
    bool SetElement(Value& array, std::uint32_t index, Value& element) noexcept {
        return SetScaleformMember(context_, array, IndexName{index}.text, element);
    }

    void Release(Value& value) noexcept {
        ReleaseValue(context_, value);
        value = {};
    }

private:
    const MovieRootContext& context_;
};

struct MovieObjects final {
    void* movieRoot{};
    // Set for movies cached when the destructor hook was removed. Their
    // holders are still linked into live movies, so the slot is not reused.
    bool retired{};
    ScaleformReleaseValueRoutine release{};
    // Unnamed object whose member "<slot>" holds the last object built for
    // that slot. Reading the member returns a new reference to it.
    ScaleformValue holder{};
    std::array<std::string, kGameObjectSlotCount> sourceJson;
};

std::mutex cacheMutex;
bool cacheAvailable{};
// Owned values are list nodes, so the holders stay at fixed addresses.
std::array<MovieObjects, kMaximumCachedMovies> cachedMovies;

[[nodiscard]] MovieObjects* FindMovieObjects(void* movieRoot) noexcept {
    for (auto& movie : cachedMovies) {
        if (!movie.retired && movie.movieRoot == movieRoot) {
            return &movie;
        }
    }
    return nullptr;
}

// Returns the movie's cache entry, creating it and its holder object when the
// movie has none. Returns null when every entry is in use.
[[nodiscard]] MovieObjects* FindOrAddMovieObjects(
    const MovieRootContext& context) noexcept {
    if (auto* movie = FindMovieObjects(context.movieRoot); movie != nullptr) {
        return movie;
    }
    for (auto& movie : cachedMovies) {
        if (movie.retired || movie.movieRoot != nullptr) {
            continue;
        }
        if (!CreateUnnamedScaleformObject(context, movie.holder)) {
            movie.holder = {};
            return nullptr;
        }
        movie.movieRoot = context.movieRoot;
        movie.release = context.api.releaseValue;
        return &movie;
    }
    return nullptr;
}

[[nodiscard]] bool BuildGameObject(
    const MovieRootContext& context,
    std::string_view json,
    ScaleformValue& out) noexcept {
    json::Document document;
    if (!json::ParseDocument(json, kGameJsonOptions, document)) {
        return false;
    }
    // A string root would borrow text from the document, which is freed here.
    const auto root = document.Root();
    if (root.GetType() != json::Type::Object) {
        return false;
    }
    ScaleformValueWriter writer{context};
    return WriteJsonAsAs3Value(writer, root, out);
}

}  // namespace

bool ReadGameObject(
    const MovieRootContext& context,
    std::size_t slot,
    std::string_view json,
    ScaleformValue& out) noexcept {
    out = {};
    if (slot >= kGameObjectSlotCount) {
        return false;
    }
    const IndexName name{static_cast<std::uint32_t>(slot)};

    std::scoped_lock lock{cacheMutex};
    MovieObjects* movie = cacheAvailable ? FindOrAddMovieObjects(context) : nullptr;
    if (movie != nullptr && movie->sourceJson[slot] == json) {
        if (GetScaleformMember(context, movie->holder, name.text, out) &&
            IsObjectLikeScaleformValue(out)) {
            return true;
        }
        ReleaseValue(context, out);
        out = {};
    }

    if (!BuildGameObject(context, json, out)) {
        return false;
    }
    if (movie == nullptr) {
        return true;
    }
    try {
        movie->sourceJson[slot].assign(json);
    } catch (...) {
        movie->sourceJson[slot].clear();
        return true;
    }
    if (!SetScaleformMember(context, movie->holder, name.text, out)) {
        movie->sourceJson[slot].clear();
    }
    return true;
}

void ReleaseGameObjects(void* movieRoot) noexcept {
    if (movieRoot == nullptr) {
        return;
    }
    std::scoped_lock lock{cacheMutex};
    auto* movie = FindMovieObjects(movieRoot);
    if (movie == nullptr) {
        return;
    }
    MovieRootContext context{};
    context.api.releaseValue = movie->release;
    ReleaseValue(context, movie->holder);
    movie->holder = {};
    movie->movieRoot = nullptr;
    movie->release = nullptr;
    for (auto& source : movie->sourceJson) {
        source.clear();
    }
}

void SetGameObjectCacheAvailable(bool available) noexcept {
    std::scoped_lock lock{cacheMutex};
    if (!available) {
        for (auto& movie : cachedMovies) {
            if (movie.movieRoot != nullptr) {
                movie.retired = true;
            }
        }
    }
    cacheAvailable = available;
}

}  // namespace sf
