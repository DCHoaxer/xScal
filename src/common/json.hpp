#pragma once

#include <yyjson.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sf::json {

inline constexpr std::size_t kMaximumParserDepth = 64;

enum class NumberSyntax {
    Any,
    SignedInteger,
};

enum class Type {
    Null,
    Boolean,
    Integer,
    Real,
    String,
    Object,
    Array,
    Invalid,
};

enum class MemberStatus {
    Found,
    Missing,
    WrongType,
};

enum class ParseErrorCode {
    None,
    InvalidLimit,
    InputTooLarge,
    InvalidUtf8,
    InvalidSyntax,
    DepthLimit,
    DuplicateMember,
    UnsupportedNumber,
    AllocationFailure,
};

struct ParseOptions final {
    NumberSyntax numberSyntax{NumberSyntax::Any};
    std::size_t maximumInputBytes{};
    std::size_t maximumDepth{};
    bool rejectDuplicateMembers{};
};

struct ParseError final {
    ParseErrorCode code{ParseErrorCode::None};
    std::size_t offset{};
};

class MemberIterator;
class ArrayIterator;

class Value final {
public:
    Value() noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] Type GetType() const noexcept;
    [[nodiscard]] bool ReadBoolean(bool& value) const noexcept;
    [[nodiscard]] bool ReadInteger(std::int64_t& value) const noexcept;
    // Reads an integer or a real number as a double.
    [[nodiscard]] bool ReadNumber(double& value) const noexcept;
    [[nodiscard]] bool ReadString(std::string_view& value) const noexcept;

    [[nodiscard]] std::size_t ArraySize() const noexcept;
    [[nodiscard]] Value ArrayItem(std::size_t index) const noexcept;

    [[nodiscard]] MemberIterator Members() const noexcept;
    // Visits the items in order; ArrayItem walks the array from the start.
    [[nodiscard]] ArrayIterator Items() const noexcept;
    [[nodiscard]] Value FindMember(std::string_view name) const noexcept;

    [[nodiscard]] MemberStatus ReadBooleanMember(
        std::string_view name,
        bool& value) const noexcept;
    [[nodiscard]] MemberStatus ReadIntegerMember(
        std::string_view name,
        std::int64_t& value) const noexcept;
    [[nodiscard]] MemberStatus ReadStringMember(
        std::string_view name,
        std::string_view& value) const noexcept;
    [[nodiscard]] MemberStatus ReadObjectMember(
        std::string_view name,
        Value& value) const noexcept;
    [[nodiscard]] MemberStatus ReadArrayMember(
        std::string_view name,
        Value& value) const noexcept;

private:
    explicit Value(yyjson_val* value) noexcept;

    yyjson_val* value_{};

    friend class Document;
    friend class MemberIterator;
    friend class ArrayIterator;
};

class MemberIterator final {
public:
    [[nodiscard]] bool Next(
        std::string_view& name,
        Value& value) noexcept;

private:
    explicit MemberIterator(yyjson_val* object) noexcept;

    yyjson_obj_iter iterator_{};

    friend class Value;
};

class ArrayIterator final {
public:
    [[nodiscard]] bool Next(Value& value) noexcept;

private:
    explicit ArrayIterator(yyjson_val* array) noexcept;

    yyjson_arr_iter iterator_{};

    friend class Value;
};

class Document final {
public:
    Document() noexcept = default;
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;

    [[nodiscard]] Value Root() const noexcept;
    void Reset() noexcept;

private:
    yyjson_doc* document_{};

    friend bool ParseDocument(
        std::string_view,
        ParseOptions,
        Document&,
        ParseError*) noexcept;
};

[[nodiscard]] bool ParseDocument(
    std::string_view text,
    ParseOptions options,
    Document& document,
    ParseError* error = nullptr) noexcept;

[[nodiscard]] bool IsValidUtf8(std::string_view text) noexcept;

void AppendString(std::string_view value, std::string& output);
[[nodiscard]] bool AppendString(
    std::string_view value,
    std::string& output,
    std::size_t maximumOutputBytes) noexcept;
[[nodiscard]] bool AppendInteger(
    std::int64_t value,
    std::string& output,
    std::size_t maximumOutputBytes) noexcept;
[[nodiscard]] bool AppendFloat(float value, std::string& output);

}  // namespace sf::json
