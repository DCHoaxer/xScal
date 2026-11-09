#include "api/json_interface.hpp"

#include <yyjson.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace sf::jsonapi {
namespace {

// The handles are yyjson's own pointers; plugins only see opaque types.
[[nodiscard]] yyjson_val* AsValue(const XScalJsonValue* value) noexcept {
    return reinterpret_cast<yyjson_val*>(const_cast<XScalJsonValue*>(value));
}

[[nodiscard]] const XScalJsonValue* AsHandle(yyjson_val* value) noexcept {
    return reinterpret_cast<const XScalJsonValue*>(value);
}

[[nodiscard]] yyjson_mut_doc* AsBuilder(const XScalJsonBuilder* builder) noexcept {
    return reinterpret_cast<yyjson_mut_doc*>(const_cast<XScalJsonBuilder*>(builder));
}

[[nodiscard]] yyjson_mut_val* AsNode(XScalJsonNode* node) noexcept {
    return reinterpret_cast<yyjson_mut_val*>(node);
}

[[nodiscard]] XScalJsonNode* AsHandle(yyjson_mut_val* node) noexcept {
    return reinterpret_cast<XScalJsonNode*>(node);
}

// What XScalJsonIterator::state holds.
struct LoopState final {
    std::uintptr_t isObject;
    union {
        yyjson_arr_iter array;
        yyjson_obj_iter object;
    };
};
static_assert(sizeof(LoopState) <= sizeof(XScalJsonIterator::state));

XScalJsonDocument* __cdecl Parse(
    const char* text, size_t length, uint32_t flags, XScalJsonReadError* error) {
    if (error != nullptr) {
        *error = {};
    }
    if (text == nullptr) {
        if (error != nullptr) {
            error->code = YYJSON_READ_ERROR_INVALID_PARAMETER;
            error->message = "input text is null";
        }
        return nullptr;
    }
    yyjson_read_flag readFlags = YYJSON_READ_NOFLAG;
    if ((flags & XSCAL_JSON_READ_ALLOW_COMMENTS) != 0) {
        readFlags |= YYJSON_READ_ALLOW_COMMENTS;
    }
    if ((flags & XSCAL_JSON_READ_ALLOW_TRAILING_COMMAS) != 0) {
        readFlags |= YYJSON_READ_ALLOW_TRAILING_COMMAS;
    }
    if ((flags & XSCAL_JSON_READ_ALLOW_BOM) != 0) {
        readFlags |= YYJSON_READ_ALLOW_BOM;
    }
    yyjson_read_err readError{};
    // Without YYJSON_READ_INSITU the reader does not write to the text.
    yyjson_doc* document = yyjson_read_opts(
        const_cast<char*>(text), length, readFlags, nullptr, &readError);
    if (document == nullptr && error != nullptr) {
        error->position = readError.pos;
        error->code = readError.code;
        error->message = readError.msg;
    }
    return reinterpret_cast<XScalJsonDocument*>(document);
}

void __cdecl FreeDocument(XScalJsonDocument* document) {
    yyjson_doc_free(reinterpret_cast<yyjson_doc*>(document));
}

const XScalJsonValue* __cdecl Root(const XScalJsonDocument* document) {
    return AsHandle(yyjson_doc_get_root(
        reinterpret_cast<yyjson_doc*>(const_cast<XScalJsonDocument*>(document))));
}

uint32_t __cdecl Type(const XScalJsonValue* value) {
    switch (yyjson_get_type(AsValue(value))) {
        case YYJSON_TYPE_NULL:
            return XSCAL_JSON_TYPE_NULL;
        case YYJSON_TYPE_BOOL:
            return XSCAL_JSON_TYPE_BOOL;
        case YYJSON_TYPE_NUM:
            return XSCAL_JSON_TYPE_NUMBER;
        case YYJSON_TYPE_STR:
            return XSCAL_JSON_TYPE_STRING;
        case YYJSON_TYPE_ARR:
            return XSCAL_JSON_TYPE_ARRAY;
        case YYJSON_TYPE_OBJ:
            return XSCAL_JSON_TYPE_OBJECT;
        default:
            return XSCAL_JSON_TYPE_NONE;
    }
}

uint32_t __cdecl NumberType(const XScalJsonValue* value) {
    yyjson_val* const number = AsValue(value);
    if (!yyjson_is_num(number)) {
        return 0;
    }
    switch (yyjson_get_subtype(number)) {
        case YYJSON_SUBTYPE_UINT:
            return XSCAL_JSON_NUMBER_UINT;
        case YYJSON_SUBTYPE_SINT:
            return XSCAL_JSON_NUMBER_SINT;
        case YYJSON_SUBTYPE_REAL:
            return XSCAL_JSON_NUMBER_REAL;
        default:
            return 0;
    }
}

int32_t __cdecl GetBool(const XScalJsonValue* value) {
    return yyjson_get_bool(AsValue(value)) ? 1 : 0;
}

uint64_t __cdecl GetUint(const XScalJsonValue* value) {
    return yyjson_get_uint(AsValue(value));
}

int64_t __cdecl GetSint(const XScalJsonValue* value) {
    return yyjson_get_sint(AsValue(value));
}

double __cdecl GetReal(const XScalJsonValue* value) {
    return yyjson_get_real(AsValue(value));
}

double __cdecl GetNumber(const XScalJsonValue* value) {
    return yyjson_get_num(AsValue(value));
}

const char* __cdecl GetString(const XScalJsonValue* value, size_t* length) {
    yyjson_val* const text = AsValue(value);
    const char* const characters = yyjson_get_str(text);
    if (length != nullptr) {
        *length = characters != nullptr ? yyjson_get_len(text) : 0;
    }
    return characters;
}

size_t __cdecl Length(const XScalJsonValue* value) {
    return yyjson_get_len(AsValue(value));
}

const XScalJsonValue* __cdecl ObjectGet(
    const XScalJsonValue* object, const char* key, size_t keyLength) {
    if (key == nullptr) {
        return nullptr;
    }
    return AsHandle(yyjson_obj_getn(AsValue(object), key, keyLength));
}

const XScalJsonValue* __cdecl ArrayGet(const XScalJsonValue* array, size_t index) {
    return AsHandle(yyjson_arr_get(AsValue(array), index));
}

int32_t __cdecl Iterate(const XScalJsonValue* container, XScalJsonIterator* iterator) {
    if (iterator == nullptr) {
        return 0;
    }
    *iterator = {};
    LoopState loop{};
    yyjson_val* const value = AsValue(container);
    if (yyjson_is_arr(value)) {
        loop.isObject = 0;
        if (!yyjson_arr_iter_init(value, &loop.array)) {
            return 0;
        }
    } else if (yyjson_is_obj(value)) {
        loop.isObject = 1;
        if (!yyjson_obj_iter_init(value, &loop.object)) {
            return 0;
        }
    } else {
        return 0;
    }
    std::memcpy(iterator->state, &loop, sizeof(loop));
    return 1;
}

const XScalJsonValue* __cdecl Next(XScalJsonIterator* iterator, const XScalJsonValue** key) {
    if (key != nullptr) {
        *key = nullptr;
    }
    if (iterator == nullptr) {
        return nullptr;
    }
    LoopState loop{};
    std::memcpy(&loop, iterator->state, sizeof(loop));
    yyjson_val* item = nullptr;
    if (loop.isObject != 0) {
        yyjson_val* const memberKey = yyjson_obj_iter_next(&loop.object);
        item = yyjson_obj_iter_get_val(memberKey);
        if (key != nullptr) {
            *key = AsHandle(memberKey);
        }
    } else {
        item = yyjson_arr_iter_next(&loop.array);
    }
    std::memcpy(iterator->state, &loop, sizeof(loop));
    return AsHandle(item);
}

XScalJsonBuilder* __cdecl NewBuilder() {
    return reinterpret_cast<XScalJsonBuilder*>(yyjson_mut_doc_new(nullptr));
}

void __cdecl FreeBuilder(XScalJsonBuilder* builder) {
    yyjson_mut_doc_free(AsBuilder(builder));
}

XScalJsonNode* __cdecl CreateNull(XScalJsonBuilder* builder) {
    return AsHandle(yyjson_mut_null(AsBuilder(builder)));
}

XScalJsonNode* __cdecl CreateBool(XScalJsonBuilder* builder, int32_t value) {
    return AsHandle(yyjson_mut_bool(AsBuilder(builder), value != 0));
}

XScalJsonNode* __cdecl CreateUint(XScalJsonBuilder* builder, uint64_t value) {
    return AsHandle(yyjson_mut_uint(AsBuilder(builder), value));
}

XScalJsonNode* __cdecl CreateSint(XScalJsonBuilder* builder, int64_t value) {
    return AsHandle(yyjson_mut_sint(AsBuilder(builder), value));
}

XScalJsonNode* __cdecl CreateReal(XScalJsonBuilder* builder, double value) {
    return AsHandle(yyjson_mut_real(AsBuilder(builder), value));
}

XScalJsonNode* __cdecl CreateString(
    XScalJsonBuilder* builder, const char* text, size_t length) {
    if (text == nullptr) {
        return nullptr;
    }
    return AsHandle(yyjson_mut_strncpy(AsBuilder(builder), text, length));
}

XScalJsonNode* __cdecl CreateArray(XScalJsonBuilder* builder) {
    return AsHandle(yyjson_mut_arr(AsBuilder(builder)));
}

XScalJsonNode* __cdecl CreateObject(XScalJsonBuilder* builder) {
    return AsHandle(yyjson_mut_obj(AsBuilder(builder)));
}

int32_t __cdecl ArrayAppend(XScalJsonNode* array, XScalJsonNode* value) {
    if (value == nullptr) {
        return 0;
    }
    return yyjson_mut_arr_append(AsNode(array), AsNode(value)) ? 1 : 0;
}

int32_t __cdecl ObjectAdd(XScalJsonBuilder* builder, XScalJsonNode* object,
    const char* key, size_t keyLength, XScalJsonNode* value) {
    if (key == nullptr || value == nullptr || !yyjson_mut_is_obj(AsNode(object))) {
        return 0;
    }
    yyjson_mut_val* const keyNode =
        yyjson_mut_strncpy(AsBuilder(builder), key, keyLength);
    return yyjson_mut_obj_add(AsNode(object), keyNode, AsNode(value)) ? 1 : 0;
}

void __cdecl SetRoot(XScalJsonBuilder* builder, XScalJsonNode* root) {
    yyjson_mut_doc_set_root(AsBuilder(builder), AsNode(root));
}

char* __cdecl Write(const XScalJsonBuilder* builder, uint32_t flags, size_t* length) {
    if (length != nullptr) {
        *length = 0;
    }
    yyjson_write_flag writeFlags = YYJSON_WRITE_NOFLAG;
    if ((flags & XSCAL_JSON_WRITE_PRETTY_TWO_SPACES) != 0) {
        writeFlags |= YYJSON_WRITE_PRETTY_TWO_SPACES;
    } else if ((flags & XSCAL_JSON_WRITE_PRETTY) != 0) {
        writeFlags |= YYJSON_WRITE_PRETTY;
    }
    size_t written = 0;
    // The text is allocated with the CRT malloc of xScal, so FreeText frees it.
    char* const text = yyjson_mut_write_opts(
        AsBuilder(builder), writeFlags, nullptr, &written, nullptr);
    if (text != nullptr && length != nullptr) {
        *length = written;
    }
    return text;
}

void __cdecl FreeText(char* text) {
    std::free(text);
}

constexpr XScalJsonV1 kInterface{
    sizeof(XScalJsonV1),
    XSCAL_JSON_API_VERSION,
    &Parse,
    &FreeDocument,
    &Root,
    &Type,
    &NumberType,
    &GetBool,
    &GetUint,
    &GetSint,
    &GetReal,
    &GetNumber,
    &GetString,
    &Length,
    &ObjectGet,
    &ArrayGet,
    &Iterate,
    &Next,
    &NewBuilder,
    &FreeBuilder,
    &CreateNull,
    &CreateBool,
    &CreateUint,
    &CreateSint,
    &CreateReal,
    &CreateString,
    &CreateArray,
    &CreateObject,
    &ArrayAppend,
    &ObjectAdd,
    &SetRoot,
    &Write,
    &FreeText,
};

}  // namespace

const XScalJsonV1* Interface() noexcept {
    return &kInterface;
}

}  // namespace sf::jsonapi
