#include <dart_flax_api.h>
#include "engine.h"
#include "flax/engine_binding.h"
#include "flax/engine_gc.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "third_party/tonic/scopes/dart_isolate_scope.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "value_id.h"

namespace fbjsi = facebook::jsi;
struct BoundValue;

struct ProxyMember {
  fbjsi::String name;
  fbjsi::PropNameID key;
  uint32_t kind;
  size_t minimum;
  size_t maximum;
  fbjsi::Value base;
  bool fallback;
};

struct FlaxRuntime {
  ~FlaxRuntime() {
    if (js)
      flax::closeEngineRuntime(*js);
  }
  Dart_Isolate owner = Dart_CurrentIsolate();
  std::recursive_mutex mutex;
  size_t depth = 0;
  bool disposing = false;
  std::unique_ptr<fbjsi::Runtime> js;
  // Declared after js so references are released before the engine.
  std::unique_ptr<fbjsi::Function> byteView;
  std::unique_ptr<fbjsi::Function> arrayBufferConstructor;
  std::unique_ptr<fbjsi::Function> bigIntConstructor;
  std::unique_ptr<fbjsi::Function> bigIntText;
  std::unique_ptr<fbjsi::Function> bufferView;
  std::unique_ptr<fbjsi::Function> memberLookup;
  std::vector<std::unique_ptr<ProxyMember>> members;
  std::unordered_map<FlaxValueId, fbjsi::Value> values;
  std::unordered_map<FlaxValueId, BoundValue*> boundValues;
};

struct BoundValue {
  FlaxRuntime* runtime;
  FlaxValueId id;
  flax::JointCell* cell = nullptr;
  static void collected(void* data) {
    auto* bound = static_cast<BoundValue*>(data);
    bound->runtime->values.erase(bound->id);
    bound->runtime->boundValues.erase(bound->id);
    delete bound;
  }
};

namespace {
flax::ValueIdAllocator<FLAX_ENGINE_ID> nextValue;
std::mutex runtimesMutex;
std::unordered_map<Dart_Isolate, std::unordered_set<FlaxRuntime*>> runtimes;

struct BridgeError : std::runtime_error {
  int32_t code;
  BridgeError(int32_t code, const char* message)
      : std::runtime_error(message), code(code) {}
};

void clearError(FlaxError* error) noexcept {
  if (!error)
    return;
  std::free(error->message);
  std::free(error->stack);
  *error = {};
}

uint8_t* copyBytes(const uint8_t* data, size_t length) noexcept {
  if (!length)
    return nullptr;
  if (!data)
    return nullptr;
  auto* result = static_cast<uint8_t*>(std::malloc(length));
  if (result)
    std::memcpy(result, data, length);
  return result;
}

void setError(FlaxError* error,
              int32_t code,
              const uint8_t* message,
              size_t messageLength,
              const uint8_t* stack,
              size_t stackLength) noexcept {
  if (!error)
    return;
  clearError(error);
  error->code = code;
  error->message = copyBytes(message, messageLength);
  error->message_length = error->message ? messageLength : 0;
  error->stack = copyBytes(stack, stackLength);
  error->stack_length = error->stack ? stackLength : 0;
}

int32_t fail(FlaxError* error,
             int32_t code,
             const std::string& message,
             const std::string& stack = {}) noexcept {
  setError(error, code, reinterpret_cast<const uint8_t*>(message.data()),
           message.size(), reinterpret_cast<const uint8_t*>(stack.data()),
           stack.size());
  return code;
}

template <typename Action>
int32_t protect(FlaxError* error, Action action) noexcept {
  clearError(error);
  try {
    action();
    return FLAX_OK;
  } catch (const fbjsi::JSError& e) {
    return fail(error, FLAX_JS_ERROR, e.getMessage(), e.getStack());
  } catch (const fbjsi::JSIException& e) {
    return fail(error, FLAX_JS_ERROR, e.what());
  } catch (const BridgeError& e) {
    return fail(error, e.code, e.what());
  } catch (const std::exception& e) {
    return fail(error, FLAX_NATIVE_ERROR, e.what());
  } catch (...) {
    return fail(error, FLAX_NATIVE_ERROR, "Unknown native exception");
  }
}

struct Entry {
  FlaxRuntime& runtime;
  std::unique_lock<std::recursive_mutex> lock;
  explicit Entry(FlaxRuntime* runtime)
      : runtime(*runtime), lock(runtime->mutex, std::try_to_lock) {
    if (!lock.owns_lock()) {
      throw BridgeError(FLAX_STATE_ERROR, "Concurrent runtime access");
    }
    if (runtime->disposing) {
      throw BridgeError(FLAX_STATE_ERROR, "Runtime is disposing");
    }
    if (runtime->owner != Dart_CurrentIsolate())
      throw BridgeError(FLAX_STATE_ERROR,
                        "Runtime belongs to another Dart isolate");
    ++runtime->depth;
  }
  ~Entry() {
    if (--runtime.depth == 0)
      flax::notifyEngineWork(*runtime.js);
  }
};

template <typename Action>
int32_t enter(FlaxRuntime* runtime, FlaxError* error, Action action) noexcept {
  return protect(error, [&] {
    if (!runtime)
      throw BridgeError(FLAX_STATE_ERROR, "Null runtime");
    Entry entry(runtime);
    action();
  });
}

std::string text(const uint8_t* data, size_t length) {
  if (!length)
    return {};
  if (!data)
    throw BridgeError(FLAX_ARGUMENT_ERROR, "Null string buffer");
  return {reinterpret_cast<const char*>(data), length};
}

fbjsi::Value& value(FlaxRuntime* runtime, FlaxValueId id) {
  auto found = runtime->values.find(id);
  if (found == runtime->values.end()) {
    throw BridgeError(FLAX_STATE_ERROR,
                      "Released value or foreign runtime value");
  }
  return found->second;
}

FlaxValueId keep(FlaxRuntime* runtime, fbjsi::Value value) {
  const auto id = nextValue.next();
  runtime->values.emplace(
      id, flax::prepareEngineValue(*runtime->js, std::move(value)));
  return id;
}

fbjsi::Object object(FlaxRuntime* runtime, FlaxValueId id) {
  auto& v = value(runtime, id);
  if (!v.isObject())
    throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected an object");
  return v.getObject(*runtime->js);
}

const char16_t* utf16(const uint16_t* data, size_t length) {
  if (!length)
    return u"";
  if (!data)
    throw BridgeError(FLAX_ARGUMENT_ERROR, "Null UTF-16 buffer");
  return reinterpret_cast<const char16_t*>(data);
}

fbjsi::PropNameID propertyName(FlaxRuntime* runtime,
                               const uint16_t* name,
                               size_t length) {
  return fbjsi::PropNameID::forUtf16(*runtime->js, utf16(name, length), length);
}

int32_t create(FlaxRuntime** output, FlaxError* error) {
  return protect(error, [&] {
    auto runtime = std::make_unique<FlaxRuntime>();
    runtime->js = flax::createEngineRuntime(&runtime->depth);
    runtime->arrayBufferConstructor = std::make_unique<fbjsi::Function>(
        runtime->js->global().getPropertyAsFunction(*runtime->js,
                                                    "ArrayBuffer"));
    runtime->bigIntConstructor = std::make_unique<fbjsi::Function>(
        runtime->js->global().getPropertyAsFunction(*runtime->js, "BigInt"));
    runtime->bigIntText = std::make_unique<fbjsi::Function>(
        runtime->bigIntConstructor
            ->getPropertyAsObject(*runtime->js, "prototype")
            .getPropertyAsFunction(*runtime->js, "toString"));
    // Reinterpret views over the existing byte lease; no payload copy or GC
    // path.
    auto views = std::make_shared<fbjsi::StringBuffer>(R"JS((() => {
      const constructors = [null, Int8Array, Uint8Array, Uint8ClampedArray,
        Int16Array, Uint16Array, Int32Array, Uint32Array, BigInt64Array,
        BigUint64Array, Float32Array, Float64Array, DataView];
      const names = ['', 'Int8Array', 'Uint8Array', 'Uint8ClampedArray',
        'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array', 'BigInt64Array',
        'BigUint64Array', 'Float32Array', 'Float64Array', 'DataView'];
      const widths = [1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 1];
      const apply = Reflect.apply;
      const getter = (p, n) => Object.getOwnPropertyDescriptor(p, n).get;
      const typed = Object.getPrototypeOf(Uint8Array.prototype);
      const tag = getter(typed, Symbol.toStringTag);
      const tb = getter(typed, 'buffer'), to = getter(typed, 'byteOffset');
      const tl = getter(typed, 'byteLength');
      const db = getter(DataView.prototype, 'buffer');
      const dO = getter(DataView.prototype, 'byteOffset');
      const dl = getter(DataView.prototype, 'byteLength');
      const bl = getter(ArrayBuffer.prototype, 'byteLength');
      const Uint8 = Uint8Array;
      return (kind, value, offset, length) => {
        if (kind < 0 || kind >= constructors.length || kind % 1 !== 0)
          throw new TypeError('Unknown native buffer kind');
        if (offset === undefined) {
          let buffer;
          if (kind === 0) {
            buffer = value; offset = 0; length = apply(bl, value, []);
          } else if (kind === 12) {
            buffer = apply(db, value, []); offset = apply(dO, value, []);
            length = apply(dl, value, []);
          } else {
            if (apply(tag, value, []) !== names[kind])
              throw new TypeError('Expected an actual ' + names[kind]);
            buffer = apply(tb, value, []); offset = apply(to, value, []);
            length = apply(tl, value, []);
          }
          return new Uint8(buffer, offset, length);
        }
        if (kind === 0) {
          if (offset !== 0 || length !== apply(bl, value, []))
            throw new TypeError('Expected the complete ByteBuffer range');
          return value;
        }
        return new constructors[kind](value, offset, length / widths[kind]);
      };
    })())JS");
    runtime->bufferView = std::make_unique<fbjsi::Function>(
        runtime->js->evaluateJavaScript(views, "flax:shared-buffers")
            .getObject(*runtime->js)
            .getFunction(*runtime->js));
    // Capture intrinsic getters: a view's own buffer/offset properties may lie.
    auto helper = std::make_shared<fbjsi::StringBuffer>(R"JS((() => {
      const getter = (p, n) => Object.getOwnPropertyDescriptor(p, n).get;
      const typed = Object.getPrototypeOf(Uint8Array.prototype);
      const tb = getter(typed, 'buffer'), to = getter(typed, 'byteOffset');
      const tl = getter(typed, 'byteLength');
      const db = getter(DataView.prototype, 'buffer');
      const dO = getter(DataView.prototype, 'byteOffset');
      const dl = getter(DataView.prototype, 'byteLength');
      const length = getter(ArrayBuffer.prototype, 'byteLength');
      const isView = ArrayBuffer.isView;
      return v => {
        if (!isView(v)) return [v, 0, length.call(v)];
        try { return [tb.call(v), to.call(v), tl.call(v)]; }
        catch (_) { return [db.call(v), dO.call(v), dl.call(v)]; }
      };
    })())JS");
    runtime->byteView = std::make_unique<fbjsi::Function>(
        runtime->js->evaluateJavaScript(helper, "flax:bytes")
            .getObject(*runtime->js)
            .getFunction(*runtime->js));
    // Capture intrinsics before application JS can replace them. Detection
    // never invokes an accessor; a method getter is evaluated once by normal
    // access.
    runtime->memberLookup = std::make_unique<fbjsi::Function>(
        runtime->js
            ->evaluateJavaScript(
                std::make_shared<fbjsi::StringBuffer>(R"JS((() => {
          const own = Object.getOwnPropertyDescriptor;
          const prototype = Object.getPrototypeOf;
          const get = Reflect.get, has = Reflect.has;
          const Error = TypeError;
          return (receiver, name, kind) => {
            if (kind === 0) {
              const value = get(receiver, name);
              if (value === undefined && !has(receiver, name)) return undefined;
              if (typeof value !== 'function') throw new Error('Missing proxy method');
              return value;
            }
            for (let object = receiver; object !== null; object = prototype(object)) {
              const descriptor = own(object, name);
              if (!descriptor) continue;
              if ('value' in descriptor) return descriptor.writable ? 1 : 0;
              return kind === 1 ? descriptor.get : descriptor.set;
            }
            return -1;
          };
        })())JS"),
                "flax:member-lookup")
            .getObject(*runtime->js)
            .getFunction(*runtime->js));
    std::lock_guard<std::mutex> lock(runtimesMutex);
    runtimes[runtime->owner].insert(runtime.get());
    *output = runtime.release();
  });
}

int32_t destroy(FlaxRuntime* runtime, FlaxError* error) {
  return protect(error, [&] {
    if (!runtime)
      return;
    {
      std::unique_lock lock(runtime->mutex, std::try_to_lock);
      if (!lock.owns_lock() || runtime->depth != 0 || runtime->disposing) {
        throw BridgeError(FLAX_STATE_ERROR, "Cannot destroy an active runtime");
      }
      runtime->disposing = true;
      runtime->values.clear();
      runtime->byteView.reset();
      runtime->arrayBufferConstructor.reset();
      runtime->bigIntConstructor.reset();
      runtime->bigIntText.reset();
      runtime->bufferView.reset();
      runtime->members.clear();
      runtime->memberLookup.reset();
      flax::closeEngineRuntime(*runtime->js);
      runtime->js.reset();
    }
    {
      std::lock_guard<std::mutex> lock(runtimesMutex);
      auto it = runtimes.find(runtime->owner);
      if (it != runtimes.end()) {
        it->second.erase(runtime);
        if (it->second.empty())
          runtimes.erase(it);
      }
    }
    delete runtime;
  });
}

int32_t evaluate(FlaxRuntime* runtime,
                 const uint8_t* source,
                 size_t sourceLength,
                 const uint8_t* url,
                 size_t urlLength,
                 FlaxValueId* output,
                 FlaxError* error) {
  return enter(runtime, error, [&] {
    auto buffer =
        std::make_shared<fbjsi::StringBuffer>(text(source, sourceLength));
    *output = keep(
        runtime, runtime->js->evaluateJavaScript(buffer, text(url, urlLength)));
  });
}

int32_t globalObject(FlaxRuntime* runtime,
                     FlaxValueId* output,
                     FlaxError* error) {
  return enter(runtime, error,
               [&] { *output = keep(runtime, runtime->js->global()); });
}

int32_t makeValue(FlaxRuntime* runtime,
                  int32_t kind,
                  double number,
                  const uint16_t* data,
                  size_t length,
                  FlaxValueId* output,
                  FlaxError* error) {
  return enter(runtime, error, [&] {
    switch (kind) {
      case FLAX_UNDEFINED:
        *output = keep(runtime, fbjsi::Value::undefined());
        break;
      case FLAX_NULL:
        *output = keep(runtime, fbjsi::Value::null());
        break;
      case FLAX_BOOLEAN:
        *output = keep(runtime, fbjsi::Value(number != 0));
        break;
      case FLAX_NUMBER:
        *output = keep(runtime, fbjsi::Value(number));
        break;
      case FLAX_STRING:
        *output = keep(runtime, fbjsi::String::createFromUtf16(
                                    *runtime->js, utf16(data, length), length));
        break;
      case FLAX_BIGINT:
        *output = keep(
            runtime,
            runtime->bigIntConstructor->call(
                *runtime->js, fbjsi::String::createFromUtf16(
                                  *runtime->js, utf16(data, length), length)));
        break;
      default:
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Unsupported primitive kind");
    }
  });
}

int32_t inspect(FlaxRuntime* runtime,
                FlaxValueId id,
                FlaxValueInfo* info,
                FlaxError* error) {
  return enter(runtime, error, [&] {
    *info = {};
    auto& v = value(runtime, id);
    if (v.isUndefined())
      info->kind = FLAX_UNDEFINED;
    else if (v.isNull())
      info->kind = FLAX_NULL;
    else if (v.isBool()) {
      info->kind = FLAX_BOOLEAN;
      info->number = v.getBool();
    } else if (v.isNumber()) {
      info->kind = FLAX_NUMBER;
      info->number = v.getNumber();
    } else if (v.isString() || v.isBigInt()) {
      info->kind = v.isBigInt() ? FLAX_BIGINT : FLAX_STRING;
      auto& js = *runtime->js;
      auto string = v.isBigInt() ? js.call(*runtime->bigIntText, v, nullptr, 0)
                                       .getString(js)
                                       .utf16(js)
                                 : v.getString(js).utf16(js);
      info->string_data = reinterpret_cast<uint16_t*>(
          copyBytes(reinterpret_cast<const uint8_t*>(string.data()),
                    string.size() * sizeof(char16_t)));
      if (!info->string_data && !string.empty())
        throw std::bad_alloc();
      info->string_length = string.size();
    } else if (v.isObject()) {
      info->kind = v.getObject(*runtime->js).isFunction(*runtime->js)
                       ? FLAX_FUNCTION
                       : FLAX_OBJECT;
    } else {
      throw BridgeError(FLAX_ARGUMENT_ERROR,
                        "Symbol conversion is not implemented");
    }
  });
}

int32_t cloneValue(FlaxRuntime* runtime,
                   FlaxValueId id,
                   FlaxValueId* output,
                   FlaxError* error) {
  return enter(runtime, error, [&] {
    *output = keep(runtime, fbjsi::Value(*runtime->js, value(runtime, id)));
  });
}

int32_t releaseValue(FlaxRuntime* runtime, FlaxValueId id, FlaxError* error) {
  return enter(runtime, error, [&] {
    auto bound = runtime->boundValues.find(id);
    if (bound != runtime->boundValues.end()) {
      flax::releaseDartPeer(*runtime->js, bound->second->cell);
      return;
    }
    if (!runtime->values.erase(id))
      throw BridgeError(FLAX_STATE_ERROR, "Released or foreign value");
  });
}

int32_t getProperty(FlaxRuntime* runtime,
                    FlaxValueId id,
                    const uint16_t* name,
                    size_t length,
                    FlaxValueId* output,
                    FlaxError* error) {
  return enter(runtime, error, [&] {
    *output = keep(
        runtime,
        object(runtime, id)
            .getProperty(*runtime->js, propertyName(runtime, name, length)));
  });
}

int32_t setProperty(FlaxRuntime* runtime,
                    FlaxValueId id,
                    const uint16_t* name,
                    size_t length,
                    FlaxValueId input,
                    FlaxError* error) {
  return enter(runtime, error, [&] {
    // Copy before a setter can reenter and mutate the handle table.
    auto v = fbjsi::Value(*runtime->js, value(runtime, input));
    object(runtime, id)
        .setProperty(*runtime->js, propertyName(runtime, name, length), v);
  });
}

int32_t call(FlaxRuntime* runtime,
             FlaxValueId id,
             FlaxValueId thisValue,
             const FlaxValueId* arguments,
             size_t count,
             FlaxValueId* output,
             FlaxError* error) {
  return enter(runtime, error, [&] {
    auto obj = object(runtime, id);
    if (!obj.isFunction(*runtime->js))
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected a function");
    std::vector<fbjsi::Value> args;
    args.reserve(count);
    for (size_t i = 0; i < count; ++i)
      args.emplace_back(*runtime->js, value(runtime, arguments[i]));
    auto receiver = thisValue
                        ? fbjsi::Value(*runtime->js, value(runtime, thisValue))
                        : fbjsi::Value::undefined();
    auto function = obj.getFunction(*runtime->js);
    // Runtime::call supports an arbitrary JS receiver, including primitives.
    *output = keep(runtime, runtime->js->call(function, receiver, args.data(),
                                              args.size()));
  });
}

int32_t strictEquals(FlaxRuntime* runtime,
                     FlaxValueId left,
                     FlaxValueId right,
                     int32_t* output,
                     FlaxError* error) {
  return enter(runtime, error, [&] {
    *output = fbjsi::Value::strictEquals(*runtime->js, value(runtime, left),
                                         value(runtime, right));
  });
}

struct CallbackValues {
  FlaxRuntime* runtime;
  std::vector<FlaxValueId> ids;
  FlaxValueId result = 0;
  FlaxError error{};
  ~CallbackValues() {
    for (auto id : ids)
      runtime->values.erase(id);
    if (result)
      runtime->values.erase(result);
    clearError(&error);
  }
  FlaxValueId add(const fbjsi::Value& v) {
    const auto id = keep(runtime, fbjsi::Value(*runtime->js, v));
    try {
      ids.push_back(id);
    } catch (...) {
      runtime->values.erase(id);
      throw;
    }
    return id;
  }
};

int32_t registerHostPeer(FlaxRuntime* runtime,
                         const uint16_t* name,
                         size_t length,
                         FlaxHostCallback callback,
                         void* context,
                         Dart_Handle target,
                         FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!callback)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null host callback");
    auto prop = propertyName(runtime, name, length);
    auto function = fbjsi::Function::createFromHostFunction(
        *runtime->js, prop, 0,
        [runtime, callback, context](
            fbjsi::Runtime& js, const fbjsi::Value& receiver,
            const fbjsi::Value* arguments, size_t count) {
          CallbackValues frame{runtime};
          frame.ids.reserve(count + 1);
          const auto thisId = frame.add(receiver);
          for (size_t i = 0; i < count; ++i)
            frame.add(arguments[i]);
          const auto status =
              callback(context, runtime, thisId, frame.ids.data() + 1, count,
                       &frame.result, &frame.error);
          if (status != FLAX_OK) {
            auto message =
                text(frame.error.message, frame.error.message_length);
            auto stack = text(frame.error.stack, frame.error.stack_length);
            if (!stack.empty())
              message += "\nDart host stack:\n" + stack;
            throw fbjsi::JSError(
                js, message.empty() ? "Dart host callback failed" : message);
          }
          return fbjsi::Value(js, value(runtime, frame.result));
        });
    if (target) {
      flax::bindDartPeer(*runtime->js, function, target, nullptr, false);
    }
    runtime->js->global().setProperty(*runtime->js, prop, std::move(function));
  });
}

int32_t registerHost(FlaxRuntime* runtime,
                     const uint16_t* name,
                     size_t length,
                     FlaxHostCallback callback,
                     void* context,
                     FlaxError* error) {
  return registerHostPeer(runtime, name, length, callback, context, nullptr,
                          error);
}

int32_t drainMicrotasks(FlaxRuntime* runtime,
                        int32_t hint,
                        int32_t* complete,
                        FlaxError* error) {
  return enter(runtime, error, [&] {
    if (hint < -1)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid microtask hint");
    if (runtime->depth > 1)
      throw BridgeError(FLAX_STATE_ERROR,
                        "Cannot drain microtasks during a callback");
    *complete = runtime->js->drainMicrotasks(hint);
  });
}

fbjsi::ArrayBuffer allocateBytes(FlaxRuntime* runtime, size_t length) {
  if (length > 9007199254740991ULL)
    throw BridgeError(FLAX_ARGUMENT_ERROR,
                      "Byte length exceeds JS integer precision");
  auto& js = *runtime->js;
  // Engine-owned storage can transfer even when JS properties are frozen.
  auto buffer = runtime->arrayBufferConstructor
                    ->callAsConstructor(js, static_cast<double>(length))
                    .getObject(js)
                    .getArrayBuffer(js);
  if (buffer.size(js) != length)
    throw BridgeError(FLAX_STATE_ERROR, "ArrayBuffer allocation size mismatch");
  return buffer;
}

int32_t makeBytes(FlaxRuntime* runtime,
                  const uint8_t* data,
                  size_t length,
                  FlaxValueId* output,
                  FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!data && length)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null byte buffer");
    auto& js = *runtime->js;
    auto buffer = allocateBytes(runtime, length);
    if (length)
      std::memcpy(buffer.data(js), data, length);
    *output = keep(runtime, std::move(buffer));
  });
}

int32_t readBytes(FlaxRuntime* runtime,
                  FlaxValueId id,
                  uint8_t** output,
                  size_t* length,
                  FlaxError* error) {
  return enter(runtime, error, [&] {
    *output = nullptr;
    *length = 0;
    auto& js = *runtime->js;
    auto range = runtime->byteView->call(js, value(runtime, id))
                     .getObject(js)
                     .getArray(js);
    auto bufferObject = range.getValueAtIndex(js, 0).getObject(js);
    if (!bufferObject.isArrayBuffer(js))
      throw BridgeError(FLAX_ARGUMENT_ERROR,
                        "Expected an unshared ArrayBuffer");
    auto buffer = bufferObject.getArrayBuffer(js);
    if (buffer.detached(js))
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Detached ArrayBuffer");
    const auto offset =
        static_cast<size_t>(range.getValueAtIndex(js, 1).getNumber());
    const auto count =
        static_cast<size_t>(range.getValueAtIndex(js, 2).getNumber());
    if (offset > buffer.size(js) || count > buffer.size(js) - offset)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Byte view is out of bounds");
    if (count) {
      *output = copyBytes(buffer.data(js) + offset, count);
      if (!*output)
        throw std::bad_alloc();
    }
    *length = count;
  });
}

fbjsi::Value bindingArgument(FlaxRuntime* runtime,
                             const FlaxBindingArgument& input) {
  auto& js = *runtime->js;
  switch (input.kind) {
    case FLAX_UNDEFINED:
      return fbjsi::Value::undefined();
    case FLAX_NULL:
      return fbjsi::Value::null();
    case FLAX_BOOLEAN:
      if (input.number != 0 && input.number != 1)
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid boolean argument");
      return fbjsi::Value(input.number != 0);
    case FLAX_NUMBER:
      return fbjsi::Value(input.number);
    case FLAX_STRING:
      return fbjsi::String::createFromUtf16(js, utf16(input.text, input.length),
                                            input.length);
    case FLAX_BIGINT:
      return runtime->bigIntConstructor->call(
          js, fbjsi::String::createFromUtf16(
                  js, utf16(input.text, input.length), input.length));
    case FLAX_OBJECT:
    case FLAX_FUNCTION: {
      auto target = object(runtime, input.id);
      if (input.kind == FLAX_FUNCTION && !target.isFunction(js))
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected a function argument");
      return target;
    }
    default:
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid binding argument");
  }
}

std::vector<fbjsi::Value> bindingArguments(FlaxRuntime* runtime,
                                           const FlaxBindingArgument* input,
                                           size_t count) {
  if (count && !input)
    throw BridgeError(FLAX_ARGUMENT_ERROR, "Null binding arguments");
  std::vector<fbjsi::Value> result;
  result.reserve(count);
  for (size_t i = 0; i < count; ++i)
    result.emplace_back(bindingArgument(runtime, input[i]));
  return result;
}

int32_t registerMembers(FlaxRuntime* runtime,
                        FlaxValueId layout,
                        uint64_t* first,
                        FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!first)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null member output");
    auto& js = *runtime->js;
    auto input = object(runtime, layout);
    if (!input.isArray(js))
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected a member layout array");
    auto entries = input.getArray(js);
    std::vector<std::unique_ptr<ProxyMember>> prepared;
    for (size_t i = 0; i < entries.size(js); ++i) {
      auto row = entries.getValueAtIndex(js, i);
      if (!row.isObject() || !row.getObject(js).isArray(js))
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected a member array");
      auto entry = row.getObject(js).getArray(js);
      if (entry.size(js) != 6)
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid member definition");
      auto nameValue = entry.getValueAtIndex(js, 0);
      auto kindValue = entry.getValueAtIndex(js, 1);
      auto minimumValue = entry.getValueAtIndex(js, 2);
      auto maximumValue = entry.getValueAtIndex(js, 3);
      auto base = entry.getValueAtIndex(js, 4);
      auto fallbackValue = entry.getValueAtIndex(js, 5);
      if (!nameValue.isString() || !kindValue.isNumber() ||
          !minimumValue.isNumber() || !maximumValue.isNumber() ||
          !fallbackValue.isBool())
        throw BridgeError(FLAX_ARGUMENT_ERROR,
                          "Invalid member definition types");
      auto name = nameValue.getString(js).utf8(js);
      auto kind = kindValue.getNumber();
      auto minimum = minimumValue.getNumber();
      auto maximum = maximumValue.getNumber();
      auto fallback = fallbackValue.getBool();
      if (name.empty() || !std::isfinite(kind) || kind < 0 || kind > 2 ||
          kind != std::floor(kind) || !std::isfinite(minimum) || minimum < 0 ||
          minimum != std::floor(minimum) || !std::isfinite(maximum) ||
          maximum != std::floor(maximum) || maximum < minimum ||
          maximum > UINT32_MAX || (kind == 1 && maximum != 0) ||
          (kind == 2 && (minimum != 1 || maximum != 1)) ||
          (!base.isUndefined() && (!fallback || !base.isObject() ||
                                   !base.getObject(js).isFunction(js))))
        throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid member definition");
      prepared.push_back(std::make_unique<ProxyMember>(ProxyMember{
          fbjsi::String::createFromUtf8(js, name),
          fbjsi::PropNameID::forUtf8(js, name), uint32_t(kind), size_t(minimum),
          size_t(maximum), std::move(base), fallback}));
    }
    *first = runtime->members.size();
    for (auto& entry : prepared)
      runtime->members.push_back(std::move(entry));
  });
}

int32_t invokeMember(FlaxRuntime* runtime,
                     FlaxValueId receiver,
                     uint64_t slot,
                     const FlaxBindingArgument* arguments,
                     size_t count,
                     int32_t* handled,
                     FlaxValueId* output,
                     FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!handled || !output || slot >= runtime->members.size())
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid member slot or output");
    *handled = 0;
    *output = 0;
    auto& js = *runtime->js;
    // Stable across getter reentry that grows the member table.
    auto* member = runtime->members[slot].get();
    if (count < member->minimum || count > member->maximum)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Invalid member argument count");
    // Arguments must be rooted before member lookup can call business JS.
    auto args = bindingArguments(runtime, arguments, count);
    auto target = object(runtime, receiver);
    auto function = runtime->memberLookup->call(
        js, target, member->name, static_cast<double>(member->kind));
    bool missing = member->kind == 0
                       ? function.isUndefined()
                       : function.isNumber() && function.getNumber() == -1;
    if (member->fallback &&
        (missing || (!member->base.isUndefined() &&
                     fbjsi::Value::strictEquals(js, function, member->base))))
      return;
    bool data = member->kind != 0 && function.isNumber() && !missing;
    if (!data &&
        (!function.isObject() || !function.getObject(js).isFunction(js)))
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Missing proxy member");
    if (data && member->kind == 2 && function.getNumber() != 1)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Readonly proxy property");
    auto result = fbjsi::Value::undefined();
    if (data) {
      if (member->kind == 1)
        result = target.getProperty(js, member->key);
      else
        target.setProperty(js, member->key, args[0]);
    } else {
      auto fn = function.getObject(js).getFunction(js);
      result = js.call(fn, fbjsi::Value(js, target), args.data(), args.size());
    }
    // Getter values use typed Dart conversion; setter execution stays
    // synchronous.
    if (member->kind == 2 && result.isObject()) {
      auto then = result.getObject(js).getProperty(js, "then");
      if (then.isObject() && then.getObject(js).isFunction(js))
        throw fbjsi::JSError(js, "Proxy property must be synchronous");
    }
    *output = keep(runtime, std::move(result));
    *handled = 1;
  });
}

int32_t bindingCall(FlaxRuntime* runtime,
                    FlaxValueId id,
                    const FlaxBindingArgument* receiver,
                    const FlaxBindingArgument* arguments,
                    size_t count,
                    FlaxValueId* output,
                    FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!output || !receiver)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null call output or receiver");
    auto& js = *runtime->js;
    auto fn = object(runtime, id).getFunction(js);
    auto args = bindingArguments(runtime, arguments, count);
    auto target = bindingArgument(runtime, *receiver);
    *output = keep(runtime, js.call(fn, target, args.data(), args.size()));
  });
}

// Only Dart-backed stores need this index: re-importing them must return a
// Dart heap view, since external typed data cannot point into the Dart heap.
struct PinnedBytes {
  std::weak_ptr<fbjsi::MutableBuffer> storage;
  void* token;
  Dart_Isolate owner;
};
thread_local std::unordered_map<void*, PinnedBytes> pinnedBytes;

void checkDart(Dart_Handle result) {
  if (Dart_IsError(result))
    throw BridgeError(FLAX_ARGUMENT_ERROR, Dart_GetError(result));
}

int32_t copyBuffer(FlaxRuntime* runtime,
                   Dart_Handle bytes,
                   FlaxValueId* output,
                   FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!output)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null copied buffer output");
    if (Dart_GetTypeOfTypedData(bytes) != Dart_TypedData_kUint8)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected Uint8List");
    intptr_t length = 0;
    checkDart(Dart_ListLength(bytes, &length));
    auto buffer = allocateBytes(runtime, length);
    auto* destination = buffer.data(*runtime->js);
    Dart_TypedData_Type type;
    void* data = nullptr;
    intptr_t acquiredLength = 0;
    checkDart(
        Dart_TypedDataAcquireData(bytes, &type, &data, &acquiredLength));
    const bool compatible =
        type == Dart_TypedData_kUint8 && acquiredLength == length;
    // No JS or allocating Dart calls while the borrowed Dart address is
    // acquired.
    if (compatible && length)
      std::memcpy(destination, data, length);
    checkDart(Dart_TypedDataReleaseData(bytes));
    if (!compatible)
      throw BridgeError(FLAX_STATE_ERROR,
                        "Uint8List range changed during copy");
    *output = keep(runtime, std::move(buffer));
  });
}

class DartBuffer final : public fbjsi::MutableBuffer {
 public:
  explicit DartBuffer(const Dart_FlaxBuffer& buffer)
      : data_(static_cast<uint8_t*>(buffer.data)),
        size_(buffer.byte_length),
        token_(buffer.token),
        state_(flutter::UIDartState::Current()->GetWeakPtr()),
        runner_(flutter::UIDartState::Current()
                    ->GetTaskRunners()
                    .GetUITaskRunner()) {}
  ~DartBuffer() override {
    // V8 may release backing stores on a GC thread. Dart roots belong to UI.
    auto token = std::shared_ptr<void>(token_, Dart_FlaxReleaseBuffer);
    runner_->PostTask(
        [token = std::move(token), data = data_, weak = state_]() mutable {
          auto found = pinnedBytes.find(data);
          if (found != pinnedBytes.end() && found->second.token == token.get())
            pinnedBytes.erase(found);
          auto state = weak.lock();
          if (state && !state->IsShuttingDown()) {
            tonic::DartState::Scope scope(state);
            token.reset();
          } else {
            // VM shutdown has already retired the persistent root.
            token.reset();
          }
        });
  }
  size_t size() const override { return size_; }
  uint8_t* data() override { return data_; }

 private:
  uint8_t* data_;
  size_t size_;
  void* token_;
  std::weak_ptr<tonic::DartState> state_;
  fml::RefPtr<fml::TaskRunner> runner_;
};

int32_t shareBuffer(FlaxRuntime* runtime,
                    Dart_Handle bytes,
                    uint32_t kind,
                    FlaxValueId* output,
                    FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!output)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Null shared buffer output");
    Dart_FlaxBuffer buffer{};
    checkDart(Dart_FlaxPinUint8List(bytes, &buffer));
    std::unique_ptr<void, void (*)(void*)> pin(buffer.token,
                                               Dart_FlaxReleaseBuffer);
    auto& saved = pinnedBytes[buffer.data];
    auto storage =
        saved.owner == runtime->owner ? saved.storage.lock() : nullptr;
    if (storage && storage->size() != static_cast<size_t>(buffer.byte_length))
      storage.reset();
    if (!storage) {
      storage = std::make_shared<DartBuffer>(buffer);
      pin.release();
      saved = {storage, buffer.token, runtime->owner};
    }
#if FLAX_ENGINE_ID == 1
    if (storage->size() > UINT32_MAX)
      throw BridgeError(FLAX_ARGUMENT_ERROR,
                        "Hermes ArrayBuffer exceeds its 32-bit size limit");
#endif
    auto& js = *runtime->js;
    fbjsi::ArrayBuffer arrayBuffer(js, storage);
    *output = keep(runtime, runtime->bufferView->call(
                                js, static_cast<double>(kind), arrayBuffer,
                                static_cast<double>(buffer.offset),
                                static_cast<double>(buffer.length)));
  });
}

Dart_Handle readBuffer(FlaxRuntime* runtime,
                       FlaxValueId id,
                       uint32_t kind,
                       FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    flax::Uint8View view;
    try {
      auto& js = *runtime->js;
      auto bytes = runtime->bufferView->call(js, static_cast<double>(kind),
                                             object(runtime, id));
      view = flax::readUint8Array(js, bytes.getObject(js));
    } catch (const fbjsi::JSError& exception) {
      throw BridgeError(FLAX_ARGUMENT_ERROR, exception.getMessage().c_str());
    } catch (const std::invalid_argument& exception) {
      throw BridgeError(FLAX_ARGUMENT_ERROR, exception.what());
    }
    auto found = pinnedBytes.find(view.storage->data());
    if (found != pinnedBytes.end() && found->second.owner == runtime->owner) {
      if (auto saved = found->second.storage.lock();
          saved && saved->size() == view.storage->size()) {
        result = Dart_FlaxUint8ListView(found->second.token, view.offset,
                                        view.length);
        checkDart(result);
        return;
      }
    }
    auto storage =
        std::make_unique<std::shared_ptr<fbjsi::MutableBuffer>>(view.storage);
    auto* data = view.storage->data();
    result = Dart_NewExternalTypedDataWithFinalizer(
        Dart_TypedData_kUint8, data ? data + view.offset : nullptr, view.length,
        storage.get(), view.storage->size(), [](void*, void* peer) {
          delete static_cast<std::shared_ptr<fbjsi::MutableBuffer>*>(peer);
        });
    checkDart(result);
    storage.release();
  });
  return result;
}

Dart_Handle instanceType(FlaxRuntime* runtime, Dart_Handle value,
                         FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_InstanceGetType(value);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

Dart_Handle typeParts(FlaxRuntime* runtime, Dart_Handle type, FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_FlaxTypeParts(type);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

Dart_Handle nullableType(FlaxRuntime* runtime, Dart_Handle type, FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_FlaxNullableType(type);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

Dart_Handle instantiate(FlaxRuntime* runtime, Dart_Handle closure,
                        Dart_Handle types, FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_FlaxInstantiate(closure, types);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

Dart_Handle recordFields(FlaxRuntime* runtime,
                         Dart_Handle value,
                         FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_FlaxRecordFields(value);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

Dart_Handle newRecord(FlaxRuntime* runtime,
                      Dart_Handle type,
                      Dart_Handle names,
                      Dart_Handle fields,
                      FlaxError* error) {
  Dart_Handle result = Dart_Null();
  enter(runtime, error, [&] {
    result = Dart_FlaxNewRecord(type, names, fields);
    checkDart(result);
  });
  return error && error->code != FLAX_OK ? Dart_Null() : result;
}

const FlaxEngineBindingApi bindingApi{FLAX_ENGINE_BINDING_VERSION,
                                      sizeof(FlaxEngineBindingApi),
                                      registerMembers,
                                      invokeMember,
                                      bindingCall,
                                      shareBuffer,
                                      readBuffer,
                                      copyBuffer,
                                      instanceType,
                                      typeParts,
                                      nullableType,
                                      instantiate,
                                      recordFields,
                                      newRecord};

const FlaxApi api{
    FLAX_ABI_VERSION, sizeof(FlaxApi), create,    destroy,      evaluate,
    globalObject,     makeValue,       inspect,   cloneValue,   releaseValue,
    getProperty,      setProperty,     call,      strictEquals, registerHost,
    drainMicrotasks,  makeBytes,       readBytes, std::free,    clearError,
    setError};

int32_t bindValue(FlaxRuntime* runtime,
                  FlaxValueId id,
                  Dart_Handle origin,
                  FlaxError* error) {
  return enter(runtime, error, [&] {
    auto& saved = value(runtime, id);
    if (!saved.isObject() || !origin || Dart_IsNull(origin) ||
        runtime->boundValues.count(id))
      throw BridgeError(FLAX_ARGUMENT_ERROR,
                        "Expected an unbound object ID and Dart owner");
    auto object = std::move(saved).getObject(*runtime->js);
    auto bound = std::make_unique<BoundValue>();
    bound->runtime = runtime;
    bound->id = id;
    try {
      bound->cell =
          flax::bindDartPeer(*runtime->js, object, nullptr, origin, true,
                             BoundValue::collected, bound.get());
      runtime->boundValues.emplace(id, bound.release());
      saved = std::move(object);
    } catch (...) {
      saved = std::move(object);
      throw;
    }
  });
}

int32_t bindPeer(FlaxRuntime* runtime,
                 FlaxValueId id,
                 Dart_Handle target,
                 Dart_Handle origin,
                 FlaxError* error) {
  return enter(runtime, error, [&] {
    if (!target || Dart_IsNull(target))
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected a Dart peer");
    auto object = ::object(runtime, id);
    flax::bindDartPeer(*runtime->js, object, target, origin, false);
  });
}

uint64_t cellCount(FlaxRuntime* runtime) {
  return runtime && runtime->js ? flax::jointCellCount(*runtime->js) : 0;
}

const FlaxEngineGcApi gcApi{FLAX_ENGINE_GC_VERSION,
                            sizeof(FlaxEngineGcApi),
                            FLAX_FLUTTER_REVISION,
                            FLAX_DART_REVISION,
                            bindValue,
                            bindPeer,
                            registerHostPeer,
                            cellCount};
}  // namespace

namespace flax {
const FlaxApi* runtimeApi() {
  return &api;
}
}  // namespace flax

extern "C" __attribute__((visibility("default"))) const FlaxEngineGcApi*
flax_engine_get_gc_api(uint32_t version) {
  return version == FLAX_ENGINE_GC_VERSION ? &gcApi : nullptr;
}

extern "C" __attribute__((visibility("default"))) const FlaxEngineBindingApi*
flax_engine_get_binding_api(uint32_t version) {
  return version == FLAX_ENGINE_BINDING_VERSION ? &bindingApi : nullptr;
}

extern "C" void flax_engine_shutdown_current_isolate() {
  std::vector<FlaxRuntime*> live;
  {
    std::lock_guard<std::mutex> lock(runtimesMutex);
    auto it = runtimes.find(Dart_CurrentIsolate());
    if (it != runtimes.end())
      live.assign(it->second.begin(), it->second.end());
  }
  flax::shutdownJointHeap();
  for (auto* runtime : live) {
    FlaxError error{};
    if (destroy(runtime, &error) != FLAX_OK)
      std::abort();
  }
}

extern "C" void flax_engine_notify_idle_current_isolate() {
  flax::notifyIdleJointHeap();
}
