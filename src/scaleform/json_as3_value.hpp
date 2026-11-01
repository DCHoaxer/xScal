#pragma once

#include "common/json.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace sf {

// Builds one ActionScript value from a parsed JSON value. Writer creates and
// links the values: Game.GetAS3Object passes one that calls into the movie,
// the tests pass one that records the tree.
//
// Writer has a Value type and these members, each returning false on failure:
//   Null(Value&), Boolean(Value&, bool), Int(Value&, std::int32_t),
//   Number(Value&, double), String(Value&, const char*),
//   Object(Value&), Array(Value&),
//   SetMember(Value& object, const char* name, Value& member),
//   SetElement(Value& array, std::uint32_t index, Value& element),
//   and void Release(Value&), which leaves the value empty: a failed child is
//   released by itself and again by its parent.
// Strings and member names are passed as yyjson's NUL-terminated copies, which
// live as long as the parsed document.
template <class Writer>
[[nodiscard]] bool WriteJsonAsAs3Value(
    Writer& writer,
    const json::Value& source,
    typename Writer::Value& out,
    std::size_t depth = 0) noexcept {
    switch (source.GetType()) {
        case json::Type::Null:
            return writer.Null(out);
        case json::Type::Boolean: {
            bool value{};
            return source.ReadBoolean(value) && writer.Boolean(out, value);
        }
        case json::Type::Integer: {
            // Integers that fit an ActionScript int stay int, as the
            // ActionScript JSON decoders return them.
            std::int64_t integer{};
            if (source.ReadInteger(integer) &&
                integer >= (std::numeric_limits<std::int32_t>::min)() &&
                integer <= (std::numeric_limits<std::int32_t>::max)()) {
                return writer.Int(out, static_cast<std::int32_t>(integer));
            }
            double number{};
            return source.ReadNumber(number) && writer.Number(out, number);
        }
        case json::Type::Real: {
            double number{};
            return source.ReadNumber(number) && writer.Number(out, number);
        }
        case json::Type::String: {
            std::string_view text;
            return source.ReadString(text) && writer.String(out, text.data());
        }
        case json::Type::Object: {
            if (depth >= json::kMaximumParserDepth || !writer.Object(out)) {
                return false;
            }
            auto members = source.Members();
            std::string_view name;
            json::Value member;
            while (members.Next(name, member)) {
                typename Writer::Value child{};
                const bool written =
                    WriteJsonAsAs3Value(writer, member, child, depth + 1) &&
                    writer.SetMember(out, name.data(), child);
                writer.Release(child);
                if (!written) {
                    writer.Release(out);
                    return false;
                }
            }
            return true;
        }
        case json::Type::Array: {
            if (depth >= json::kMaximumParserDepth || !writer.Array(out)) {
                return false;
            }
            auto items = source.Items();
            json::Value item;
            std::uint32_t index{};
            while (items.Next(item)) {
                typename Writer::Value child{};
                const bool written =
                    WriteJsonAsAs3Value(writer, item, child, depth + 1) &&
                    writer.SetElement(out, index, child);
                writer.Release(child);
                if (!written) {
                    writer.Release(out);
                    return false;
                }
                ++index;
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace sf
