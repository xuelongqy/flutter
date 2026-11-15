// Native ABI types, shared by calls, callbacks, layouts and memory access.
enum class Type {
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F32,
  F64,
  Pointer,
  Void,
  Bool,
  Aggregate
};
struct NativeType;
using TypeRef = std::shared_ptr<NativeType>;
struct Field {
  std::string name;
  TypeRef type;
  size_t offset;
};
struct NativeType {
  Type kind = Type::Void;
  ffi_type *abi = &ffi_type_void;
  ffi_type aggregate{};
  std::vector<ffi_type *> elements;
  std::vector<Field> fields;
  TypeRef element;
  size_t length = 0;
  bool flexible = false;
  std::string shape;
  size_t size() const { return abi->size; }
  size_t alignment() const { return abi->alignment; }
};
[[noreturn]] void fail(fbjsi::Runtime &js, const std::string &message,
                       const char *code = "ERR_INVALID_ARG_VALUE") {
  const std::string kind(code);
  const char *constructor = kind == "ERR_OUT_OF_RANGE" ||
                                    kind == "ERR_BUFFER_TOO_LARGE" ||
                                    kind == "ERR_STRING_TOO_LONG"
                                ? "RangeError"
                            : kind == "ERR_INVALID_ARG_TYPE" ||
                                      kind == "ERR_INVALID_ARG_VALUE" ||
                                      kind == "ERR_UNKNOWN_ENCODING"
                                ? "TypeError"
                                : "Error";
  fbjsi::Value error = js.global().getPropertyAsFunction(js, constructor).callAsConstructor(
      js, fbjsi::String::createFromUtf8(js, message));
  error.getObject(js).setProperty(js, "code",
                                  fbjsi::String::createFromAscii(js, code));
  throw fbjsi::JSError(js, std::move(error));
}
size_t checkedSize(fbjsi::Runtime &js, const fbjsi::Value &value) {
  if (!value.isNumber())
    fail(js, "FFI byte range/native size must be a number", "ERR_INVALID_ARG_TYPE");
  if (!std::isfinite(value.getNumber()) ||
      value.getNumber() < 0 ||
      std::floor(value.getNumber()) != value.getNumber())
    fail(js, "FFI byte range/native size must be a nonnegative safe integer");
  if (value.getNumber() > 9007199254740991.0 ||
      value.getNumber() >= static_cast<double>(SIZE_MAX))
    fail(js, "FFI byte range/native size is too large", "ERR_OUT_OF_RANGE");
  return static_cast<size_t>(value.getNumber());
}
size_t alignedSize(fbjsi::Runtime &js, size_t size, size_t alignment) {
  if (!alignment || size > SIZE_MAX - (alignment - 1))
    fail(js, "Native layout overflows");
  return (size + alignment - 1) / alignment * alignment;
}
TypeRef scalar(Type kind, ffi_type *abi) {
  auto type = std::make_shared<NativeType>();
  type->kind = kind;
  type->abi = abi;
  return type;
}
TypeRef parseType(fbjsi::Runtime &js, const fbjsi::Value &value, bool result,
                  unsigned depth = 0) {
  if (depth > 64)
    fail(js, "Native type nesting exceeds 64 levels");
  if (value.isString()) {
    const auto name = value.getString(js).utf8(js);
    static const auto primitives = std::vector<std::pair<std::string, TypeRef>>{
        {"int8", scalar(Type::I8, &ffi_type_sint8)},
        {"uint8", scalar(Type::U8, &ffi_type_uint8)},
        {"int16", scalar(Type::I16, &ffi_type_sint16)},
        {"uint16", scalar(Type::U16, &ffi_type_uint16)},
        {"int32", scalar(Type::I32, &ffi_type_sint32)},
        {"uint32", scalar(Type::U32, &ffi_type_uint32)},
        {"int64", scalar(Type::I64, &ffi_type_sint64)},
        {"uint64", scalar(Type::U64, &ffi_type_uint64)},
        {"float", scalar(Type::F32, &ffi_type_float)},
        {"double", scalar(Type::F64, &ffi_type_double)},
        {"c_bool", scalar(Type::Bool, &ffi_type_uint8)},
        {"pointer", scalar(Type::Pointer, &ffi_type_pointer)},
        {"void", scalar(Type::Void, &ffi_type_void)}};
    for (const auto &entry : primitives) {
      if (name == entry.first && (result || entry.second->kind != Type::Void))
        return entry.second;
    }
    auto integerType = [&](size_t size, bool sign) -> TypeRef {
      return primitives[(size == 1   ? 0
                         : size == 2 ? 2
                         : size == 4 ? 4
                                     : 6) +
                        (sign ? 0 : 1)]
          .second;
    };
    if (name == "char")
      return integerType(sizeof(char), std::is_signed<char>::value);
    if (name == "short")
      return integerType(sizeof(short), true);
    if (name == "ushort")
      return integerType(sizeof(unsigned short), false);
    if (name == "int")
      return integerType(sizeof(int), true);
    if (name == "uint")
      return integerType(sizeof(unsigned int), false);
    if (name == "long")
      return integerType(sizeof(long), true);
    if (name == "ulong")
      return integerType(sizeof(unsigned long), false);
    if (name == "intptr" || name == "ssize_t")
      return integerType(sizeof(intptr_t), true);
    if (name == "uintptr" || name == "size_t")
      return integerType(sizeof(uintptr_t), false);
    if (name == "wchar_t")
      return integerType(sizeof(wchar_t), std::is_signed<wchar_t>::value);
    fail(js, "Unsupported FFI type (void is return-only): " + name);
  }
  if (!value.isObject() || value.getObject(js).isArray(js))
    fail(js, "Expected a native type");
  auto definition = value.getObject(js);
  auto shapeValue = definition.getProperty(js, "kind");
  if (!shapeValue.isString())
    fail(js, "Expected a native aggregate descriptor");
  auto type = std::make_shared<NativeType>();
  type->kind = Type::Aggregate;
  type->shape = shapeValue.getString(js).utf8(js);
  size_t size = 0, alignment = 1;
  if (type->shape == "array") {
    type->element =
        parseType(js, definition.getProperty(js, "element"), false, depth + 1);
    auto length = definition.getProperty(js, "length");
    type->flexible = length.isNull();
    type->length = type->flexible ? 0 : checkedSize(js, length);
    if (!type->flexible && !type->length)
      fail(js, "A fixed array must have a positive length");
    if (type->element->flexible)
      fail(js, "Only the outermost array dimension can be variable");
    if (type->length && type->element->size() > SIZE_MAX / type->length)
      fail(js, "Native array overflows");
    size = type->element->size() * type->length;
    alignment = type->element->alignment();
    // Aggregate classification needs at most four homogeneous float elements.
    if (type->length > 65536) {
      type->elements.push_back(&ffi_type_uint8);
    } else
      for (size_t i = 0; i < type->length; ++i)
        type->elements.push_back(type->element->abi);
  } else if (type->shape == "struct" || type->shape == "union") {
    const bool isUnion = type->shape == "union";
    auto fieldsValue = definition.getProperty(js, "fields");
    if (!fieldsValue.isObject() || !fieldsValue.getObject(js).isArray(js))
      fail(js, "Expected aggregate fields");
    auto fields = fieldsValue.getObject(js).getArray(js);
    if (!fields.size(js))
      fail(js, "An aggregate must have fields");
    auto packValue = definition.getProperty(js, "pack");
    size_t pack =
        packValue.isUndefined() ? SIZE_MAX : checkedSize(js, packValue);
    if (pack != SIZE_MAX &&
        (isUnion || !pack || pack > 16 || (pack & (pack - 1))))
      fail(js, "Struct packing must be 1, 2, 4, 8 or 16");
    std::unordered_set<std::string> names;
    for (size_t i = 0, count = fields.size(js); i < count; ++i) {
      auto field = fields.getValueAtIndex(js, i).getObject(js);
      auto nameValue = field.getProperty(js, "name");
      if (!nameValue.isString())
        fail(js, "Expected a field name");
      auto name = nameValue.getString(js).utf8(js);
      if (name.empty() || !names.insert(name).second)
        fail(js, "Invalid or duplicate native field");
      auto child =
          parseType(js, field.getProperty(js, "type"), false, depth + 1);
      if (child->flexible && (isUnion || i + 1 != count))
        fail(js, "A variable array must be the last struct field");
      auto memberAlignment = std::min(pack, child->alignment());
      alignment = std::max(alignment, memberAlignment);
      const auto offset = isUnion ? 0 : alignedSize(js, size, memberAlignment);
      if (child->size() > SIZE_MAX - offset)
        fail(js, "Native aggregate overflows");
      size = std::max(size, offset + child->size());
      type->fields.push_back({std::move(name), child, offset});
      if (!child->flexible)
        type->elements.push_back(child->abi);
    }
    size = alignedSize(js, size, alignment);
    // AArch64 non-HFA aggregates are passed as their memory image. Flatten
    // unions and packed layouts to bytes; preserve an HFA only when every
    // member agrees.
    if (isUnion || pack != SIZE_MAX) {
      std::function<std::pair<ffi_type *, size_t>(const TypeRef &)> hfa =
          [&](const TypeRef &t) -> std::pair<ffi_type *, size_t> {
        if (t->kind == Type::F32 || t->kind == Type::F64)
          return {t->abi, 1};
        if (t->kind != Type::Aggregate || t->flexible)
          return {nullptr, 0};
        if (t->shape == "array") {
          auto inner = hfa(t->element);
          return {inner.first, inner.second * t->length};
        }
        ffi_type *base = nullptr;
        size_t members = 0;
        for (auto &f : t->fields) {
          auto part = hfa(f.type);
          if (!part.first || (base && base != part.first))
            return {nullptr, 0};
          base = part.first;
          members = t->shape == "union" ? std::max(members, part.second)
                                        : members + part.second;
        }
        return {base, members};
      };
      auto homogeneous = hfa(type);
      type->elements.clear();
      if (homogeneous.first && homogeneous.second <= 4 &&
          size == homogeneous.first->size * homogeneous.second)
        type->elements.assign(homogeneous.second, homogeneous.first);
      else
        type->elements.assign(std::min(size, size_t(17)), &ffi_type_uint8);
    }
  } else
    fail(js, "Unknown native aggregate kind");
  if (!size && !type->flexible)
    fail(js, "A native aggregate cannot be empty");
  type->elements.push_back(nullptr);
  type->aggregate = {size, static_cast<unsigned short>(alignment),
                     FFI_TYPE_STRUCT, type->elements.data()};
  type->abi = &type->aggregate;
  return type;
}
