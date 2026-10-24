struct ByteView final : fbjsi::MutableBuffer {
  ByteView(void *address, size_t length) : address(address), length(length) {
    ++liveViews;
  }
  ~ByteView() override { --liveViews; }
  size_t size() const override { return length; }
  uint8_t *data() override { return static_cast<uint8_t *>(address); }
  void *address;
  size_t length;
};
struct CopyBytes final : fbjsi::MutableBuffer {
  explicit CopyBytes(size_t size) : bytes(size) { ++liveMemories; }
  ~CopyBytes() override { --liveMemories; }
  size_t size() const override { return bytes.size(); }
  uint8_t *data() override { return bytes.data(); }
  std::vector<uint8_t> bytes;
};
size_t byteLength(fbjsi::Runtime &js, const fbjsi::Value &value) {
  return checkedSize(js, value);
}
flax::Uint8View sourceBytes(FlaxRuntime *runtime, fbjsi::Runtime &js,
                            State &state, const fbjsi::Value &value, int kind) {
  auto normalized = state.byteView->call(js, value, kind).getObject(js);
  try {
    return flaxFfiReadBytes(js, normalized);
  } catch (const std::invalid_argument &error) {
    fail(js, error.what());
  }
}
fbjsi::Value output(fbjsi::Runtime &js, const NativeType &type,
                    const void *data) {
  switch (type.kind) {
  case Type::I8:
    return static_cast<double>(load<int8_t>(data));
  case Type::U8:
    return static_cast<double>(load<uint8_t>(data));
  case Type::I16:
    return static_cast<double>(load<int16_t>(data));
  case Type::U16:
    return static_cast<double>(load<uint16_t>(data));
  case Type::I32:
    return static_cast<double>(load<int32_t>(data));
  case Type::U32:
    return static_cast<double>(load<uint32_t>(data));
  case Type::I64:
    return fbjsi::BigInt::fromInt64(js, load<int64_t>(data));
  case Type::U64:
    return fbjsi::BigInt::fromUint64(js, load<uint64_t>(data));
  case Type::F32:
    return static_cast<double>(load<float>(data));
  case Type::F64:
    return load<double>(data);
  case Type::Pointer:
    return wrapPointer(js, load<void *>(data));
  case Type::Bool:
    return load<uint8_t>(data) != 0;
  case Type::Void:
    return {};
  case Type::Aggregate: {
    auto copy = std::make_shared<CopyBytes>(type.size());
    if (type.size())
      std::memcpy(copy->data(), data, type.size());
    fbjsi::ArrayBuffer buffer(js, copy);
    return js.createUint8Array(buffer, 0, type.size());
  }
  }
  std::abort();
}
std::vector<uint8_t> encode(const std::u16string &text,
                            const std::string &encoding) {
  std::vector<uint8_t> result;
  const bool utf16 = encoding == "utf16le" || encoding == "utf-16le" ||
                     encoding == "ucs2" || encoding == "ucs-2";
  if (encoding == "hex") {
    auto digit = [](char16_t c) {
      return c >= '0' && c <= '9'   ? c - '0'
             : c >= 'a' && c <= 'f' ? c - 'a' + 10
             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                    : -1;
    };
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
      auto high = digit(text[i]), low = digit(text[i + 1]);
      if (high < 0 || low < 0)
        break;
      result.push_back((high << 4) | low);
    }
  } else if (encoding == "base64" || encoding == "base64url") {
    uint32_t word = 0;
    unsigned bits = 0;
    for (auto c : text) {
      if (c == '=')
        break;
      int value = c >= 'A' && c <= 'Z'   ? c - 'A'
                  : c >= 'a' && c <= 'z' ? c - 'a' + 26
                  : c >= '0' && c <= '9' ? c - '0' + 52
                  : c == '+' || c == '-' ? 62
                  : c == '/' || c == '_' ? 63
                                         : -1;
      if (value < 0)
        continue;
      word = (word << 6) | value;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        result.push_back((word >> bits) & 255);
      }
    }
  } else
    for (size_t i = 0; i < text.size(); ++i) {
      uint32_t value = text[i];
      if (encoding == "ascii" || encoding == "latin1" || encoding == "binary") {
        result.push_back(value & 255);
        continue;
      }
      if (utf16) {
        result.push_back(value & 255);
        result.push_back(value >> 8);
        continue;
      }
      if (value >= 0xd800 && value <= 0xdbff && i + 1 < text.size() &&
          text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
        value = 0x10000 + ((value - 0xd800) << 10) + (text[++i] - 0xdc00);
      else if (value >= 0xd800 && value <= 0xdfff)
        value = 0xfffd;
      if (value < 0x80)
        result.push_back(value);
      else {
        const auto count = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
        result.push_back((count == 2   ? 0xc0
                          : count == 3 ? 0xe0
                                       : 0xf0) |
                         (value >> (6 * (count - 1))));
        for (int j = count - 2; j >= 0; --j)
          result.push_back(0x80 | ((value >> (6 * j)) & 0x3f));
      }
    }
  result.push_back(0);
  if (utf16)
    result.push_back(0);
  return result;
}
fbjsi::Value decode(fbjsi::Runtime &js, const uint8_t *bytes, size_t length) {
  std::u16string result;
  for (size_t i = 0; i < length;) {
    const auto first = bytes[i];
    uint32_t value = first;
    size_t count = first < 0x80                     ? 1
                   : first >= 0xc2 && first <= 0xdf ? 2
                   : first >= 0xe0 && first <= 0xef ? 3
                   : first >= 0xf0 && first <= 0xf4 ? 4
                                                    : 0;
    size_t consumed = 1;
    if (count > 1) {
      value = first & (count == 2 ? 0x1f : count == 3 ? 0xf : 7);
      for (; consumed < count && i + consumed < length; ++consumed) {
        const auto byte = bytes[i + consumed];
        const auto low = consumed == 1 && first == 0xe0   ? 0xa0
                         : consumed == 1 && first == 0xf0 ? 0x90
                                                          : 0x80;
        const auto high = consumed == 1 && first == 0xed   ? 0x9f
                          : consumed == 1 && first == 0xf4 ? 0x8f
                                                           : 0xbf;
        if (byte < low || byte > high)
          break;
        value = (value << 6) | (byte & 0x3f);
      }
    }
    if (!count || consumed != count)
      value = 0xfffd;
    i += consumed;
    if (value <= 0xffff)
      result.push_back(value);
    else {
      value -= 0x10000;
      result.push_back(0xd800 + (value >> 10));
      result.push_back(0xdc00 + (value & 0x3ff));
    }
  }
  return fbjsi::String::createFromUtf16(js, result);
}
fbjsi::Value arrayBuffer(FlaxRuntime *runtime, fbjsi::Runtime &js,
                         const fbjsi::Value &value, size_t length, bool copy) {
  auto address = pointer(js, value);
  range(js, address, length);
  if (copy || !length) {
    if (!runtime) {
      auto bytes = std::make_shared<CopyBytes>(length);
      if (length)
        std::memcpy(bytes->data(), address, length);
      return fbjsi::ArrayBuffer(js, bytes);
    }
    auto result = allocateBytes(runtime, length);
    if (length)
      std::memcpy(result.data(js), address, length);
    return result;
  }
  return fbjsi::ArrayBuffer(js, std::make_shared<ByteView>(address, length));
}
fbjsi::Object layoutValue(fbjsi::Runtime &js, const TypeRef &type) {
  fbjsi::Object result(js);
  result.setProperty(js, "size", static_cast<double>(type->size()));
  result.setProperty(js, "alignment", static_cast<double>(type->alignment()));
  result.setProperty(js, "flexible",
                     type->flexible || (!type->fields.empty() &&
                                        type->fields.back().type->flexible));
  if (type->kind != Type::Aggregate) {
    const char *names[] = {"int8",    "uint8", "int16",  "uint16", "int32",
                           "uint32",  "int64", "uint64", "float",  "double",
                           "pointer", "void",  "c_bool"};
    result.setProperty(js, "scalar",
                       fbjsi::String::createFromAscii(
                           js, names[static_cast<size_t>(type->kind)]));
  }
  fbjsi::Array fields(js, type->fields.size());
  for (size_t i = 0; i < type->fields.size(); ++i) {
    fbjsi::Object field(js);
    field.setProperty(js, "name",
                      fbjsi::String::createFromUtf8(js, type->fields[i].name));
    field.setProperty(js, "offset",
                      static_cast<double>(type->fields[i].offset));
    fields.setValueAtIndex(js, i, std::move(field));
  }
  result.setProperty(js, "fields", std::move(fields));
  return result;
}
fbjsi::Value memoryBind(FlaxRuntime *, fbjsi::Runtime &,
                        const std::shared_ptr<State> &, const fbjsi::Value *,
                        size_t);
fbjsi::Value makeFinalizer(fbjsi::Runtime &, const std::shared_ptr<State> &,
                           const fbjsi::Value &);
fbjsi::Value memoryCall(FlaxRuntime *runtime, fbjsi::Runtime &js, State &state,
                        const std::string &name, const fbjsi::Value *args,
                        size_t count) {
  if (state.closed || state.retiring)
    fail(js, "FFI installation is closed", "ERR_INVALID_STATE");
  if (!state.runtime) state.sweep();
  auto arity = [&](size_t minimum, size_t maximum) {
    if (count < minimum || count > maximum)
      fail(js, "Invalid FFI memory argument count");
  };
  if (name == "bindFunction")
    return memoryBind(runtime, js, state.shared_from_this(), args, count);
  if (name == "finalizer") {
    arity(1, 1);
    return makeFinalizer(js, state.shared_from_this(), args[0]);
  }
  if (name == "layout") {
    arity(1, 1);
    return layoutValue(js, parseType(js, args[0], false));
  }
  if (name == "read" || name == "write") {
    const bool write = name == "write";
    arity(write ? 4 : 3, write ? 5 : 4);
    const bool node = count == (write ? 5 : 4) && args[count - 1].isBool() &&
                      args[count - 1].getBool();
    auto type = parseType(js, args[0], false);
    if (type->kind == Type::Aggregate)
      fail(js, "Use a byte view for aggregate memory access");
    auto address = reinterpret_cast<uintptr_t>(pointer(js, args[1], node));
    if (!address)
      fail(js, "Null pointer has no accessible bytes", "ERR_FFI_INVALID_POINTER");
    if (node) {
      if (write && args[2].isUndefined())
        fail(js, "Expected an offset argument");
      byteLength(js, args[2]);
      if (write && args[3].isUndefined())
        fail(js, "Expected a value argument");
    }
    if (!args[2].isNumber() || !std::isfinite(args[2].getNumber()) ||
        std::floor(args[2].getNumber()) != args[2].getNumber() ||
        std::abs(args[2].getNumber()) > 9007199254740991.0)
      fail(js, "Expected a safe integer byte offset");
    auto offset = static_cast<int64_t>(args[2].getNumber());
    if ((offset < 0 && static_cast<uint64_t>(-offset) > address) ||
        (offset >= 0 && static_cast<uint64_t>(offset) > UINTPTR_MAX - address))
      fail(js, "FFI pointer offset overflows");
    auto destination = reinterpret_cast<void *>(
        offset < 0 ? address - static_cast<uint64_t>(-offset)
                   : address + offset);
    range(js, destination, type->size());
    if (write) {
      scalarInput(js, *type, args[3], destination);
      return {};
    }
    return output(js, *type, destination);
  }
  if (name == "allocate") {
    arity(3, 3);
    auto size = byteLength(js, args[0]);
    auto alignment = args[1].isUndefined() ? alignof(std::max_align_t)
                                           : byteLength(js, args[1]);
    if (!alignment || (alignment & (alignment - 1)))
      fail(js, "Native alignment must be a positive power of two");
    if (!args[2].isBool())
      fail(js, "Allocation zero flag must be boolean");
    void *address = nullptr;
    if (posix_memalign(&address, std::max(alignment, sizeof(void *)),
                       std::max(size, size_t(1))) != 0 ||
        !address)
      fail(js, "Native allocation failed", "ERR_MEMORY_ALLOCATION_FAILED");
    if (args[2].getBool() && size)
      std::memset(address, 0, size);
    return wrapPointer(js, address);
  }
  if (name == "free") {
    arity(1, 1);
    std::free(pointer(js, args[0]));
    return {};
  }
  if (name == "getRawPointer") {
    arity(1, 1);
    auto source = sourceBytes(runtime, js, state, args[0], 0);
    auto base = source.storage->data();
    return wrapPointer(js, source.offset ? base + source.offset : base);
  }
  if (name == "toArrayBuffer" || name == "toUint8Array") {
    arity(2, 3);
    auto address = pointer(js, args[0], true);
    auto length = byteLength(js, args[1]);
    range(js, address, length);
    const bool copy = count < 3 || args[2].isUndefined() ? true
                      : args[2].isBool()
                          ? args[2].getBool()
                          : (fail(js, "copy must be a boolean", "ERR_INVALID_ARG_TYPE"), false);
    auto result = arrayBuffer(runtime, js, args[0], length, copy);
    return name == "toUint8Array" ? state.uint8View->call(js, result)
                                  : std::move(result);
  }
  if (name == "toString" || name == "toUtf16String") {
    arity(1, 2);
    auto *bytes = static_cast<const uint8_t *>(pointer(js, args[0], true));
    if (!bytes)
      return fbjsi::Value::null();
    if (name == "toUtf16String") {
      const size_t maximum = count == 2 && !args[1].isUndefined()
                                 ? byteLength(js, args[1])
                                 : SIZE_MAX / 2;
      std::u16string text;
      for (size_t i = 0; i < maximum; ++i) {
        range(js, bytes, (i + 1) * 2);
        const auto code = static_cast<uint16_t>(bytes[i * 2]) |
                          (static_cast<uint16_t>(bytes[i * 2 + 1]) << 8);
        if (!code)
          return fbjsi::String::createFromUtf16(js, text);
        text.push_back(code);
      }
      fail(js, "String has no NUL within the readable UTF-16 range");
    }
    size_t length;
    if (count == 2 && !args[1].isUndefined()) {
      length = byteLength(js, args[1]);
      range(js, bytes, length);
      auto end =
          length ? static_cast<const uint8_t *>(std::memchr(bytes, 0, length))
                 : nullptr;
      if (!end)
        fail(js, "String has no NUL within the readable byte range");
      length = end - bytes;
    } else
      length = std::strlen(reinterpret_cast<const char *>(bytes));
    return decode(js, bytes, length);
  }
  if (name == "exportString" || name == "exportArrayBuffer" ||
      name == "exportArrayBufferView") {
    arity(3, name == "exportString" ? 4 : 3);
    auto capacity = byteLength(js, args[2]);
    std::vector<uint8_t> encoded;
    flax::Uint8View source{};
    const uint8_t *data;
    size_t length;
    if (name == "exportString") {
      if (!args[0].isString())
        fail(js, "Expected a JavaScript string", "ERR_INVALID_ARG_TYPE");
      std::string encoding = "utf8";
      if (count == 4 && !args[3].isUndefined()) {
        if (!args[3].isString())
          fail(js, "Expected an encoding string", "ERR_INVALID_ARG_TYPE");
        encoding = args[3].getString(js).utf8(js);
        std::transform(encoding.begin(), encoding.end(), encoding.begin(),
                       [](unsigned char c) { return std::tolower(c); });
      }
      if (encoding != "utf8" && encoding != "utf-8" && encoding != "utf16le" &&
          encoding != "utf-16le" && encoding != "ucs2" && encoding != "ucs-2" &&
          encoding != "ascii" && encoding != "latin1" && encoding != "binary" &&
          encoding != "hex" && encoding != "base64" && encoding != "base64url")
        fail(js, "Unknown string encoding", "ERR_UNKNOWN_ENCODING");
      encoded = encode(args[0].getString(js).utf16(js), encoding);
      data = encoded.data();
      length = encoded.size();
    } else {
      source = sourceBytes(runtime, js, state, args[0],
                           name == "exportArrayBuffer" ? 1 : 2);
      auto base = source.storage->data();
      data = source.offset ? base + source.offset : base;
      length = source.length;
    }
    if (length > capacity)
      fail(js, "Destination capacity is too small", "ERR_OUT_OF_RANGE");
    auto destination = pointer(js, args[1], true);
    range(js, destination, length);
    if (capacity > UINTPTR_MAX - reinterpret_cast<uintptr_t>(destination))
      fail(js, "FFI pointer range overflows address space");
    if (length)
      std::memmove(destination, data, length);
    return {};
  }
  fail(js, "Unknown FFI memory operation");
}
void installIntrinsics(fbjsi::Runtime &js, State &state) {
  auto helpers =
      js.evaluateJavaScript(std::make_shared<fbjsi::StringBuffer>(R"JS((() => {
    if (!Symbol.dispose) Object.defineProperty(Symbol, 'dispose', { value: Symbol.for('Symbol.dispose') });
    const apply = Reflect.apply, getter = (p, n) => Object.getOwnPropertyDescriptor(p, n)?.get;
    const invalid = (message, code) => { throw Object.assign(new TypeError(message), { code }); };
    const typed = Object.getPrototypeOf(Uint8Array.prototype);
    const buffer = getter(typed, 'buffer'), offset = getter(typed, 'byteOffset'), length = getter(typed, 'byteLength');
    const size = getter(ArrayBuffer.prototype, 'byteLength');
    const sharedSize = typeof SharedArrayBuffer === 'function' ? getter(SharedArrayBuffer.prototype, 'byteLength') : undefined;
    const db = getter(DataView.prototype, 'buffer'), dO = getter(DataView.prototype, 'byteOffset'), dL = getter(DataView.prototype, 'byteLength');
    const isView = ArrayBuffer.isView, Uint8 = Uint8Array, finalizers = new WeakMap();
    // Weak peer membership permits unlinking without retaining the target.
    const memberships = new WeakMap();
    // Cleanup must not invoke replaceable collection methods after cancellation.
    const weakGet = WeakMap.prototype.get, weakDelete = WeakMap.prototype.delete, setDelete = Set.prototype.delete;
    return {
      view(value, kind) {
        const badType = kind === 3 ? 'ERR_INVALID_ARG_VALUE' : 'ERR_INVALID_ARG_TYPE';
        const view = isView(value);
        if ((kind === 1 && view) || (kind === 2 && !view)) invalid('Expected the declared ArrayBuffer or ArrayBufferView', 'ERR_INVALID_ARG_TYPE');
        let b = value, o = 0, n;
        if (view) {
          try { b = apply(buffer, value, []); o = apply(offset, value, []); n = apply(length, value, []); }
          catch (_) { b = apply(db, value, []); o = apply(dO, value, []); n = apply(dL, value, []); }
        } else {
          try { n = apply(size, value, []); }
          catch (error) {
            if (kind === 1 || !sharedSize) invalid('Expected an ArrayBuffer', badType);
            try { n = apply(sharedSize, value, []); }
            catch (_) { invalid('Expected a native byte source', badType); }
          }
        }
        try { apply(size, b, []); }
        catch (error) { if (!sharedSize) throw error; apply(sharedSize, b, []); }
        try { return new Uint8(b, o, n); }
        catch (_) { invalid('ArrayBuffer is detached', 'ERR_INVALID_ARG_VALUE'); }
      },
      uint8(buffer) { return new Uint8(buffer); },
      attach(target, peer) {
        let peers = finalizers.get(target);
        if (!peers) finalizers.set(target, peers = new Set());
        memberships.set(peer, peers);
        peers.add(peer);
      },
      detach(peer) {
        const peers = apply(weakGet, memberships, [peer]);
        if (peers) apply(setDelete, peers, [peer]);
        apply(weakDelete, memberships, [peer]);
      },
    };
  })())JS"),
                            "flax:ffi-memory")
          .getObject(js);
  state.byteView = std::make_unique<fbjsi::Function>(
      helpers.getPropertyAsFunction(js, "view"));
  state.uint8View = std::make_unique<fbjsi::Function>(
      helpers.getPropertyAsFunction(js, "uint8"));
  state.attachFinalizer = std::make_unique<fbjsi::Function>(
      helpers.getPropertyAsFunction(js, "attach"));
  state.detachFinalizer = std::make_unique<fbjsi::Function>(
      helpers.getPropertyAsFunction(js, "detach"));
}
void installMemory(FlaxRuntime *runtime, fbjsi::Runtime &js,
                   const std::shared_ptr<State> &state) {
  installIntrinsics(js, *state);
  fbjsi::Object module(js);
  for (const char *name :
       {"getRawPointer", "toString", "toUtf16String", "toArrayBuffer",
        "toUint8Array", "exportString", "exportArrayBuffer",
        "exportArrayBufferView", "layout", "read", "write", "allocate", "free",
        "bindFunction", "finalizer"}) {
    module.setProperty(
        js, name,
        fbjsi::Function::createFromHostFunction(
            js, fbjsi::PropNameID::forAscii(js, name), 0,
            [runtime, state, name](fbjsi::Runtime &js, const fbjsi::Value &,
                                   const fbjsi::Value *args, size_t count) {
              std::optional<Entry> entry;
              if (runtime)
                entry.emplace(runtime);
              return memoryCall(runtime, js, *state, name, args, count);
            }));
  }
  js.global().setProperty(js, "__flaxFfiMemory", std::move(module));
  fbjsi::Object platform(js);
  platform.setProperty(js, "suffix",
                       fbjsi::String::createFromAscii(js, "dylib"));
  platform.setProperty(js, "architecture",
                       fbjsi::String::createFromAscii(js, "arm64"));
  const uint16_t word = 1;
  platform.setProperty(js, "littleEndian",
                       *reinterpret_cast<const uint8_t *>(&word) == 1);
  platform.setProperty(js, "pointerSize", static_cast<double>(sizeof(void *)));
  platform.setProperty(js, "longSize", static_cast<double>(sizeof(long)));
  platform.setProperty(js, "wcharSize", static_cast<double>(sizeof(wchar_t)));
  fbjsi::Object aliases(js);
  const char *integers[] = {"int8", "uint8", "int16", "uint16", "int32",
                            "uint32", "int64", "uint64"};
  for (const char *name : {"char", "short", "ushort", "int", "uint", "long",
                           "ulong", "intptr", "uintptr", "size_t", "ssize_t", "wchar_t"}) {
    auto type = parseType(js, fbjsi::String::createFromAscii(js, name), false);
    aliases.setProperty(js, name, fbjsi::String::createFromAscii(
                                     js, integers[static_cast<size_t>(type->kind)]));
  }
  js.global().setProperty(js, "__flaxFfiTypeAliases", std::move(aliases));
  platform.setProperty(
      js, "callbackBackend",
      fbjsi::String::createFromAscii(js, "precompiled-libffi-table"));
  js.global().setProperty(js, "__flaxFfiPlatform", std::move(platform));
}
