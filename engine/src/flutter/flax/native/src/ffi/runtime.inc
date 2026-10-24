// Shared JS -> C ABI implementation. Included after the engine-owned runtime.
#include "flax/engine_ffi.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <dart_flax_api.h>
#include <dart_native_api.h>
#include <deque>
#include <dlfcn.h>
#include <ffi.h>
#include <functional>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>

void flaxFfiCollectJs(fbjsi::Runtime &);
std::unique_ptr<fbjsi::Runtime> flaxFfiCreateCallbackRuntime();
flax::Uint8View flaxFfiReadBytes(fbjsi::Runtime &, const fbjsi::Object &);

namespace ffi_dlopen {
std::atomic<uint64_t> liveLibraries{0}, liveBindings{0}, livePointers{0},
    calls{0};
std::atomic<uint64_t> liveMemories{0}, liveViews{0}, liveCallbacks{0},
    liveFinalizers{0}, liveFinalizerPeers{0}, liveAsyncCalls{0};
#if defined(FLAX_FFI_TESTING)
std::atomic<uint64_t> postedWakeups{0}, largestCleanupBatch{0};
#endif
#include "types.inc"

struct SharedImage {
  ~SharedImage();
  std::string key;
  void *handle = nullptr;
};
std::mutex imagesMutex;
std::unordered_map<std::string, std::weak_ptr<SharedImage>> images;
SharedImage::~SharedImage() {
  {
    std::lock_guard<std::mutex> lock(imagesMutex);
    auto found = images.find(key);
    if (found != images.end() && found->second.expired()) images.erase(found);
  }
  if (handle) ::dlclose(handle);
}
// Each Session pins its own references without counting other Sessions' pins.
struct Image {
  std::shared_ptr<SharedImage> shared;
};
struct Callback;
struct BindingPeer;
struct Finalization;
struct Message;
struct AsyncCall;
struct State;
struct Library {
  Library() { ++liveLibraries; }
  ~Library();
  std::weak_ptr<State> state;
  std::shared_ptr<Image> image;
  void *lookup = RTLD_DEFAULT;
  bool closed = false;
  size_t active = 0;
};
struct State : std::enable_shared_from_this<State> {
  std::atomic<bool> closed{false};
  std::atomic<bool> retiring{false};
  std::atomic<bool> closing{false};
  FlaxRuntime *runtime = nullptr;
  fbjsi::Runtime *js = nullptr;
  std::thread::id owner = std::this_thread::get_id();
  std::vector<std::weak_ptr<Library>> libraries;
  std::vector<std::shared_ptr<Image>> ownedImages;
  std::shared_ptr<Library> addressLibrary = std::make_shared<Library>();
  std::unique_ptr<fbjsi::Function> byteView, uint8View, attachFinalizer,
      detachFinalizer;
  std::unordered_map<void *, std::shared_ptr<Callback>> callbacks;
  std::unordered_map<const fbjsi::NativeState *, std::weak_ptr<BindingPeer>>
      bindings;
  std::unordered_map<uint64_t, std::shared_ptr<Finalization>> finalizers;
  std::mutex queueMutex;
  std::deque<std::shared_ptr<Message>> messages;
  std::deque<uint64_t> readyFinalizers;
  std::vector<std::shared_ptr<AsyncCall>> asyncCalls;
  size_t ownerCalls = 0; // Guarded with queueMutex, like synchronous deliveries.
  uint64_t nextCall = 1;
  std::string callbackError;
  std::string foreignCallbackError; // Guarded by queueMutex.
  std::atomic<Dart_Port> port{ILLEGAL_PORT};
  std::atomic<bool> wakePending{false}, metadataDirty{false};
  bool sweeping = false;
  uint64_t nextFinalizer = 1;
  void wake();
  void metadataChanged() { metadataDirty = true; wake(); }
  void sweep(size_t limit = 128, bool finishAll = false);
  void checkpoint();
  void drain(fbjsi::Runtime &js);
  void close();
  bool retire(bool reportCallbackErrors);
  void completeCalls(fbjsi::Runtime &js);
  void cancelDeliveries(Library *library = nullptr, Callback *target = nullptr);
};
Library::~Library() {
  image.reset();
  if (auto owner = state.lock()) owner->metadataChanged();
  --liveLibraries;
}
thread_local std::string *asyncError = nullptr;
struct OwnerCall {
  State &state;
  explicit OwnerCall(State &state) : state(state) {
    std::lock_guard<std::mutex> lock(state.queueMutex);
    ++state.ownerCalls;
  }
  ~OwnerCall() {
    std::lock_guard<std::mutex> lock(state.queueMutex);
    --state.ownerCalls;
  }
};
std::mutex statesMutex;
std::unordered_map<FlaxRuntime *, std::shared_ptr<State>> states;

std::string text(fbjsi::Runtime &js, const fbjsi::Value &value) {
  if (!value.isString())
    fail(js, "Expected an FFI string", "ERR_INVALID_ARG_TYPE");
  auto result = value.getString(js).utf8(js);
  if (result.find('\0') != std::string::npos)
    fail(js, "FFI names and paths must be nonempty and contain no NUL");
  return result;
}
void *pointer(fbjsi::Runtime &js, const fbjsi::Value &value, bool raw = false) {
  if (!raw && (value.isNull() || value.isUndefined()))
    return nullptr;
  if (!value.isBigInt())
    fail(js, "Expected a bigint pointer or null",
         raw ? "ERR_INVALID_ARG_TYPE" : "ERR_INVALID_ARG_VALUE");
  auto address = value.getBigInt(js);
  if (!address.isUint64(js) || address.getUint64(js) > UINTPTR_MAX)
    fail(js, "FFI pointer address out of range");
  return reinterpret_cast<void *>(
      static_cast<uintptr_t>(address.getUint64(js)));
}
fbjsi::Value wrapPointer(fbjsi::Runtime &js, const void *address) {
  return fbjsi::BigInt::fromUint64(js, reinterpret_cast<uintptr_t>(address));
}
void range(fbjsi::Runtime &js, const void *address, size_t length) {
  if (!address && length)
    fail(js, "Null pointer has no accessible bytes", "ERR_FFI_INVALID_POINTER");
  if (length > UINTPTR_MAX - reinterpret_cast<uintptr_t>(address))
    fail(js, "FFI pointer range overflows address space");
}
template <typename T> T integer(fbjsi::Runtime &js, const fbjsi::Value &value) {
  if (!value.isNumber())
    fail(js, "Expected an FFI integer");
  const auto number = value.getNumber();
  if (!std::isfinite(number) || std::floor(number) != number ||
      number < static_cast<double>(std::numeric_limits<T>::min()) ||
      number > static_cast<double>(std::numeric_limits<T>::max()))
    fail(js, "FFI integer out of range");
  return static_cast<T>(number);
}
struct Slot {
  std::vector<std::max_align_t> words;
  std::vector<uint8_t> string;
  flax::Uint8View source{};
  explicit Slot(size_t size = sizeof(ffi_arg))
      : words((std::max(size, sizeof(ffi_arg)) + sizeof(std::max_align_t) - 1) /
              sizeof(std::max_align_t)) {}
  void *data() { return words.data(); }
};
template <class T> T load(const void *source) {
  T value;
  std::memcpy(&value, source, sizeof(T));
  return value;
}
template <class T> void store(void *destination, T value) {
  std::memcpy(destination, &value, sizeof(T));
}
void scalarInput(fbjsi::Runtime &js, const NativeType &type,
                 const fbjsi::Value &value, void *out) {
  switch (type.kind) {
  case Type::Bool:
    if (!value.isBool())
      fail(js, "Expected an FFI boolean");
    store(out, static_cast<uint8_t>(value.getBool()));
    return;
  case Type::I8:
    store(out, integer<int8_t>(js, value));
    return;
  case Type::U8:
    store(out, integer<uint8_t>(js, value));
    return;
  case Type::I16:
    store(out, integer<int16_t>(js, value));
    return;
  case Type::U16:
    store(out, integer<uint16_t>(js, value));
    return;
  case Type::I32:
    store(out, integer<int32_t>(js, value));
    return;
  case Type::U32:
    store(out, integer<uint32_t>(js, value));
    return;
  case Type::I64:
  case Type::U64: {
    if (value.isBigInt()) {
      auto number = value.getBigInt(js);
      if (type.kind == Type::I64 && number.isInt64(js)) {
        store(out, number.getInt64(js));
        return;
      }
      if (type.kind == Type::U64 && number.isUint64(js)) {
        store(out, number.getUint64(js));
        return;
      }
    } else if (value.isNumber() && std::isfinite(value.getNumber()) &&
               std::floor(value.getNumber()) == value.getNumber() &&
               std::abs(value.getNumber()) <= 9007199254740991.0) {
      if (type.kind == Type::I64) {
        store(out, static_cast<int64_t>(value.getNumber()));
        return;
      }
      if (value.getNumber() >= 0) {
        store(out, static_cast<uint64_t>(value.getNumber()));
        return;
      }
    }
    fail(js, "FFI 64-bit integer out of range or not exact");
  }
  case Type::F32:
  case Type::F64:
    if (!value.isNumber())
      fail(js, "Expected an FFI number");
    if (type.kind == Type::F32)
      store(out, static_cast<float>(value.getNumber()));
    else
      store(out, value.getNumber());
    return;
  case Type::Pointer:
    store(out, pointer(js, value));
    return;
  default:
    fail(js, "Expected a scalar native value");
  }
}
fbjsi::Value output(fbjsi::Runtime &js, const NativeType &type,
                    const void *data);
std::vector<uint8_t> encode(const std::u16string &, const std::string &);
flax::Uint8View sourceBytes(FlaxRuntime *, fbjsi::Runtime &, State &,
                            const fbjsi::Value &, int);
void input(fbjsi::Runtime &js, State &state, const NativeType &type,
           const fbjsi::Value &value, Slot &slot) {
  if (type.kind == Type::Aggregate) {
    auto source =
        value.isObject() && value.getObject(js).hasProperty(js, "$bytes")
            ? value.getObject(js).getProperty(js, "$bytes")
            : fbjsi::Value(js, value);
    slot.source = sourceBytes(state.runtime, js, state, source, 0);
    if (slot.source.length < type.size())
      fail(js, "Aggregate byte storage is too small");
    if (type.size())
      std::memcpy(slot.data(), slot.source.storage->data() + slot.source.offset,
                  type.size());
  } else if (type.kind == Type::Pointer && value.isString()) {
    slot.string = encode(value.getString(js).utf16(js), "utf8");
    store(slot.data(), static_cast<void *>(slot.string.data()));
  } else if (type.kind == Type::Pointer && value.isObject()) {
    slot.source = sourceBytes(state.runtime, js, state, value, 3);
    auto *base = slot.source.storage->data();
    store(slot.data(),
          static_cast<void *>(slot.source.offset ? base + slot.source.offset
                                                 : base));
  } else
    scalarInput(js, type, value, slot.data());
}

#include "memory.inc"

struct Binding {
  Binding() { ++liveBindings; }
  ~Binding() { --liveBindings; }
  std::string name;
  std::shared_ptr<Library> library;
  std::vector<TypeRef> arguments;
  std::vector<ffi_type *> argumentTypes;
  TypeRef result;
  ffi_cif cif{};
  void *address = nullptr;
};
struct BindingPeer : fbjsi::NativeState {
  BindingPeer(std::shared_ptr<Binding> binding, std::weak_ptr<State> state)
      : binding(std::move(binding)), state(std::move(state)) {}
  ~BindingPeer() override {
    binding.reset();
    if (auto owner = state.lock()) owner->metadataChanged();
  }
  std::shared_ptr<Binding> binding;
  std::weak_ptr<State> state;
};
void checkOpen(fbjsi::Runtime &js, const State &state, const Library &library) {
  if (state.closed || state.retiring || library.closed)
    fail(js, "FFI library is closed", "ERR_FFI_LIBRARY_CLOSED");
}
std::shared_ptr<Binding> prepare(fbjsi::Runtime &js,
                                 const std::shared_ptr<Library> &library,
                                 const fbjsi::Value &definition) {
  if (!definition.isObject() || definition.getObject(js).isArray(js))
    fail(js, "Expected an FFI signature");
  auto binding = std::make_shared<Binding>();
  binding->library = library;
  auto signature = definition.getObject(js);
  auto types = signature.getPropertyAsObject(js, "arguments").getArray(js);
  for (size_t i = 0, count = types.size(js); i < count; ++i) {
    auto type = parseType(js, types.getValueAtIndex(js, i), false);
    if (type->flexible || type->shape == "array")
      fail(js, "C array parameters must use pointer types");
    binding->arguments.push_back(type);
    binding->argumentTypes.push_back(type->abi);
  }
  binding->result = parseType(js, signature.getProperty(js, "return"), true);
  if (binding->result->flexible || binding->result->shape == "array")
    fail(js, "C functions cannot return arrays");
  auto fixed = signature.getProperty(js, "fixedArguments");
  ffi_status status;
  if (!fixed.isUndefined()) {
    const auto count = checkedSize(js, fixed);
    if (count > binding->arguments.size())
      fail(js, "Invalid fixed variadic argument count");
    for (size_t i = count; i < binding->arguments.size(); ++i) {
      auto kind = binding->arguments[i]->kind;
      if (kind == Type::Bool || kind == Type::F32 || kind == Type::I8 ||
          kind == Type::U8 || kind == Type::I16 || kind == Type::U16)
        fail(js,
             "Variadic types require C default promotions to int32 or double");
    }
    status = ffi_prep_cif_var(
        &binding->cif, FFI_DEFAULT_ABI, count, binding->argumentTypes.size(),
        binding->result->abi, binding->argumentTypes.data());
  } else
    status = ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI,
                          binding->argumentTypes.size(), binding->result->abi,
                          binding->argumentTypes.data());
  if (status != FFI_OK)
    fail(js, "Unsupported native ABI signature", "ERR_FFI_CALL_FAILED");
  return binding;
}
fbjsi::Value invoke(FlaxRuntime *runtime, fbjsi::Runtime &js, State &state,
                    Binding &binding, const fbjsi::Value *args, size_t count,
                    bool captureErrno = false) {
  checkOpen(js, state, *binding.library);
  if (count != binding.arguments.size())
    fail(js, "Invalid FFI argument count for " + binding.name);
  if (!runtime) state.sweep();
  std::vector<Slot> slots;
  std::vector<void *> addresses;
  slots.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    slots.emplace_back(binding.arguments[i]->size());
    input(js, state, *binding.arguments[i], args[i], slots.back());
    addresses.push_back(slots.back().data());
  }
  Slot result(binding.result->size());
  checkOpen(js, state, *binding.library);
  auto savedError = std::move(state.callbackError);
  state.callbackError.clear();
  ++binding.library->active;
  ++calls;
  OwnerCall ownerCall(state);
  state.cancelDeliveries();
  if (captureErrno) errno = 0;
  ffi_call(&binding.cif, FFI_FN(binding.address), result.data(),
           addresses.data());
  const int nativeError = errno;
  {
    std::lock_guard<std::mutex> lock(state.queueMutex);
    if (state.callbackError.empty()) state.callbackError = std::move(state.foreignCallbackError);
    state.foreignCallbackError.clear();
  }
  --binding.library->active;
  auto callbackError = std::move(state.callbackError);
  state.callbackError = std::move(savedError);
  if (!callbackError.empty())
    fail(js, callbackError, "ERR_FFI_CALLBACK_FAILED");
  if (!runtime)
    state.drain(js);
  auto value = output(js, *binding.result, result.data());
  if (!captureErrno) return value;
  fbjsi::Object captured(js);
  captured.setProperty(js, "value", std::move(value));
  captured.setProperty(js, "errno", nativeError);
  return captured;
}
fbjsi::Function callable(FlaxRuntime *runtime, fbjsi::Runtime &js,
                         const std::shared_ptr<State> &state,
                         const std::shared_ptr<Binding> &binding) {
  auto fn = fbjsi::Function::createFromHostFunction(
      js, fbjsi::PropNameID::forUtf8(js, binding->name),
      binding->arguments.size(),
      [runtime, state, binding](fbjsi::Runtime &js, const fbjsi::Value &,
                                const fbjsi::Value *args, size_t count) {
        std::optional<Entry> entry;
        if (runtime)
          entry.emplace(runtime);
        return invoke(runtime, js, *state, *binding, args, count);
      });
  fn.setProperty(js, "pointer", wrapPointer(js, binding->address));
  auto peer = std::make_shared<BindingPeer>(binding, state);
  fn.setNativeState(js, peer);
  state->bindings[peer.get()] = peer;
  return fn;
}
std::shared_ptr<Binding> boundFunction(fbjsi::Runtime &js, State &state,
                                       const fbjsi::Value &value) {
  if (!value.isObject() || !value.getObject(js).isFunction(js) ||
      !value.getObject(js).hasNativeState(js))
    fail(js, "Expected a function bound by this FFI installation");
  auto native = value.getObject(js).getNativeState(js);
  auto found = state.bindings.find(native.get());
  auto peer = found == state.bindings.end() ? nullptr : found->second.lock();
  if (!peer || peer.get() != native.get())
    fail(js, "Expected a function bound by this FFI installation");
  checkOpen(js, state, *peer->binding->library);
  return peer->binding;
}
void *symbol(fbjsi::Runtime &js, const std::shared_ptr<Library> &library,
             const std::string &name, bool required = true) {
  ::dlerror();
  auto address = ::dlsym(library->lookup, name.c_str());
  const char *error = ::dlerror();
  if ((error || !address) && required)
    fail(js,
         "Cannot resolve FFI symbol " + name + ": " +
             (error ? error : "null address"),
         "ERR_FFI_CALL_FAILED");
  return error ? nullptr : address;
}
#include "ffi-script.inc"
void installGlobals(FlaxRuntime *, fbjsi::Runtime &,
                    const std::shared_ptr<State> &);
#include "callbacks.inc"
#include "async.inc"

void closeLibrary(fbjsi::Runtime &js, State &state, Library &library) {
  if (library.closed)
    return;
  if (library.active)
    fail(js, "Cannot close a library during an active native call",
         "ERR_INVALID_STATE");
  if (executingCallback &&
      executingCallback->binding->library.get() == &library)
    fail(js, "Cannot close a library during an active callback",
         "ERR_INVALID_STATE");
  for (auto &[address, callback] : state.callbacks)
    if (callback->binding->library.get() == &library && callback->stop &&
        executingCallback && executingCallback->state.lock().get() == &state)
      fail(js, "Cannot stop a producer from inside a native callback",
           "ERR_INVALID_STATE");
  // One producer can call several callbacks: revoke all before any stop/join.
  for (auto &[address, callback] : state.callbacks)
    if (callback->binding->library.get() == &library)
      callback->accepting = false;
  state.cancelDeliveries(&library);
  for (auto &[address, callback] : state.callbacks)
    if (callback->binding->library.get() == &library)
      callback->stopProducer();
  for (auto &[address, callback] : state.callbacks)
    if (callback->binding->library.get() == &library && callback->active)
      fail(js, "Cannot close a library during an active callback",
           "ERR_INVALID_STATE");
  for (auto it = state.callbacks.begin(); it != state.callbacks.end();) {
    if (it->second->binding->library.get() == &library)
      it = state.callbacks.erase(it);
    else
      ++it;
  }
  library.closed = true;
  library.image.reset();
  state.metadataDirty = true;
}
fbjsi::Value open(FlaxRuntime *runtime, fbjsi::Runtime &js,
                  const std::shared_ptr<State> &state, const fbjsi::Value *args,
                  size_t count) {
  if (state->closed || state->retiring)
    fail(js, "FFI installation is closed", "ERR_INVALID_STATE");
  if ((count != 1 && count != 2) || (!args[0].isNull() && !args[0].isString()))
    fail(js, "Expected DynamicLibrary(path or null)", "ERR_INVALID_ARG_TYPE");
  auto library = std::make_shared<Library>();
  library->state = state;
  if (count == 2 && !args[1].isUndefined()) {
    if (!args[0].isNull() || text(js, args[1]) != "executable")
      fail(js, "Invalid library scope");
    library->lookup = RTLD_MAIN_ONLY;
  } else if (!args[0].isNull()) {
    const auto path = text(js, args[0]);
    auto *resolved = ::realpath(path.c_str(), nullptr);
    const std::string key = resolved ? resolved : path;
    std::free(resolved);
    for (const auto &image : state->ownedImages)
      if (image->shared->key == key) { library->image = image; break; }
    if (!library->image) {
      auto image = std::make_shared<Image>();
      {
        std::lock_guard<std::mutex> lock(imagesMutex);
        auto found = images.find(key);
        if (found != images.end()) image->shared = found->second.lock();
      }
      if (!image->shared) {
        auto loaded = std::make_shared<SharedImage>();
        loaded->key = key;
        // Loading/unloading can run native constructors; never hold the cache lock.
        loaded->handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!loaded->handle) {
          const char *error = ::dlerror();
          fail(js,
               "Cannot load FFI library " + path +
                   " (file does not exist or loader rejected it): " +
                   (error ? error : "unknown loader error"),
               "ERR_FFI_CALL_FAILED");
        }
        {
          std::lock_guard<std::mutex> lock(imagesMutex);
          auto &cached = images[key];
          image->shared = cached.lock();
          if (!image->shared) {
            cached = loaded;
            image->shared = loaded;
          }
        }
      }
      library->image = image;
      state->ownedImages.push_back(std::move(image));
    }
    library->lookup = library->image->shared->handle;
  }
  fbjsi::Object api(js);
  for (const char *name :
       {"check", "close", "getFunction", "bindFunction", "getSymbol",
        "providesSymbol", "handle", "registerCallback", "unregisterCallback",
        "refCallback", "unrefCallback"}) {
    api.setProperty(
        js, name,
        fbjsi::Function::createFromHostFunction(
            js, fbjsi::PropNameID::forAscii(js, name), 0,
            [runtime, state, library,
             name](fbjsi::Runtime &js, const fbjsi::Value &,
                   const fbjsi::Value *args, size_t count) -> fbjsi::Value {
              std::optional<Entry> entry;
              if (runtime)
                entry.emplace(runtime);
              std::string operation(name);
              if (operation == "close") {
                if (count)
                  fail(js, "close expects no arguments");
                closeLibrary(js, *state, *library);
                state->sweep();
                return {};
              }
              checkOpen(js, *state, *library);
              if (operation == "getFunction" || operation == "bindFunction") {
                if (count != 2)
                  fail(js, "Expected function name/address and signature");
                auto binding = prepare(js, library, args[1]);
                binding->name = operation == "getFunction" ? text(js, args[0])
                                                           : "nativeFunction";
                binding->address = operation == "getFunction"
                                       ? symbol(js, library, binding->name)
                                       : pointer(js, args[0]);
                if (!binding->address)
                  fail(js, "Cannot bind a null function pointer");
                checkOpen(js, *state, *library);
                return callable(runtime, js, state, binding);
              }
              if (operation == "getSymbol" || operation == "providesSymbol") {
                if (count != 1)
                  fail(js, "getSymbol expects one name");
                auto address = symbol(js, library, text(js, args[0]),
                                      operation == "getSymbol");
                return operation == "getSymbol"
                           ? wrapPointer(js, address)
                           : fbjsi::Value(address != nullptr);
              }
              if (operation == "handle")
                return wrapPointer(js, library->lookup);
              if (operation.find("Callback") != std::string::npos)
                return callbackOperation(js, state, library, operation, args,
                                         count);
              if (count)
                fail(js, "check expects no arguments");
              return {};
            }));
  }
  state->libraries.push_back(library);
  return api;
}
void installGlobals(FlaxRuntime *runtime, fbjsi::Runtime &js,
                    const std::shared_ptr<State> &state) {
  state->js = &js;
  installMemory(runtime, js, state);
  installCalls(js, state);
  js.global().setProperty(
      js, "__flaxFfiDlopen",
      fbjsi::Function::createFromHostFunction(
          js, fbjsi::PropNameID::forAscii(js, "DynamicLibrary"), 1,
          [runtime, state](fbjsi::Runtime &js, const fbjsi::Value &,
                           const fbjsi::Value *args, size_t count) {
            std::optional<Entry> entry;
            if (runtime)
              entry.emplace(runtime);
            return open(runtime, js, state, args, count);
          }));
}
void State::wake() {
  auto nativePort = port.load();
  if (nativePort != ILLEGAL_PORT && !closed && !wakePending.exchange(true)) {
    Dart_CObject message{};
    message.type = Dart_CObject_kNull;
    if (!Dart_PostCObject(nativePort, &message)) wakePending = false;
#if defined(FLAX_FFI_TESTING)
    else ++postedWakeups;
#endif
  }
}
void State::sweep(size_t limit, bool finishAll) {
  if (sweeping) return;
  sweeping = true;
#if defined(FLAX_FFI_TESTING)
  uint64_t completed = 0;
#endif
  try {
    for (size_t turn = 0; turn < limit; ++turn) {
      uint64_t id;
      if (finishAll) {
        if (finalizers.empty()) break;
        id = finalizers.begin()->first;
      } else {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (readyFinalizers.empty()) break;
        id = readyFinalizers.front();
        readyFinalizers.pop_front();
      }
      auto found = finalizers.find(id);
      if (found == finalizers.end()) continue; // A successful detach consumed it.
      auto record = found->second;
      try {
        record->releasePeer(*js);
      } catch (...) {
        std::lock_guard<std::mutex> lock(queueMutex);
        readyFinalizers.push_front(id);
        throw;
      }
      finalizers.erase(id);
      record->finish();
#if defined(FLAX_FFI_TESTING)
      ++completed;
#endif
    }
    if (metadataDirty.exchange(false)) {
      std::erase_if(libraries, [](const auto &entry) { return entry.expired(); });
      std::erase_if(bindings, [](const auto &entry) { return entry.second.expired(); });
      // GC only drops references; loader handles are released on the owner thread.
      std::erase_if(ownedImages, [](const auto &image) { return image.use_count() == 1; });
    }
    sweeping = false;
  } catch (...) {
    sweeping = false;
    wake();
    throw;
  }
#if defined(FLAX_FFI_TESTING)
  if (limit != std::numeric_limits<size_t>::max()) {
    auto previous = largestCleanupBatch.load();
    while (previous < completed && !largestCleanupBatch.compare_exchange_weak(previous, completed)) {}
  }
#endif
  std::lock_guard<std::mutex> lock(queueMutex);
  if (!readyFinalizers.empty() || metadataDirty) wake();
}
void State::checkpoint() {
  // Clear before draining so a concurrent producer can schedule the next turn.
  wakePending = false;
  try {
    if (!retiring) drain(*js);
    sweep();
  } catch (...) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (!readyFinalizers.empty() || metadataDirty) wake();
    throw;
  }
}
void State::close() {
  if (executingCallback && executingCallback->state.lock().get() == this)
    throw BridgeError(FLAX_STATE_ERROR,
                      "FFI retirement is waiting for an active callback");
  if (ownerCalls)
    throw BridgeError(FLAX_STATE_ERROR, "FFI retirement is waiting for an owner call");
  if (!asyncCalls.empty()) {
    retiring = true;
    closing = true;
    cancelDeliveries();
    for (auto &[address, callback] : callbacks) callback->stopProducer();
    for (auto &call : asyncCalls) call->cancelled = true;
    for (auto &call : asyncCalls) {
      call->worker.join();
      call->discardResult();
      --call->binding->library->active;
      if (call->discard) --call->discard->library->active;
    }
    asyncCalls.clear();
  }
  for (auto &entry : libraries)
    if (auto lib = entry.lock())
      if (lib->active)
        throw BridgeError(FLAX_STATE_ERROR,
                          "FFI retirement is waiting for active calls");
  if (!retire(false))
    throw BridgeError(FLAX_STATE_ERROR,
                      "FFI retirement is waiting for active callbacks");
}
bool State::retire(bool reportCallbackErrors) {
  wakePending = false;
  closing = true;
  cancelDeliveries();
  if (sweeping || ownerCalls || (executingCallback && executingCallback->state.lock().get() == this))
    return false;
  // Stop producers before waiting: an active call may itself wait for stop/join.
  for (auto &[address, callback] : callbacks) callback->stopProducer();
  if (!asyncCalls.empty()) {
    const auto pending = asyncCalls.size();
    completeCalls(*js);
    // Promise settlements must run before finalizers and VM destruction.
    if (asyncCalls.size() != pending) wake();
    return false;
  }
  retiring = true;
  for (auto &entry : libraries)
    if (auto library = entry.lock())
      if (library->active)
        return false;
  for (auto &[address, callback] : callbacks)
    if (callback->active)
      return false;
  if (reportCallbackErrors) {
    try {
      drain(*js);
    } catch (...) {
      // Reporting must schedule another retirement turn so close can finish.
      wake();
      throw;
    }
  }
  sweep(reportCallbackErrors ? 128 : std::numeric_limits<size_t>::max(), true);
  if (!finalizers.empty()) { wake(); return false; }
  closed = true;
  port = ILLEGAL_PORT;
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    messages.clear();
    readyFinalizers.clear();
  }
  callbacks.clear();
  bindings.clear();
  for (auto &entry : libraries)
    if (auto lib = entry.lock()) {
      lib->closed = true;
      lib->image.reset();
    }
  libraries.clear();
  addressLibrary->closed = true;
  byteView.reset();
  uint8View.reset();
  attachFinalizer.reset();
  detachFinalizer.reset();
  // Closed factories may be collected on Hermes's finalizer thread later.
  for (auto &image : ownedImages) image->shared.reset();
  ownedImages.clear();
  return true;
}
} // namespace ffi_dlopen

bool flaxFfiExperimentInstalled(FlaxRuntime *runtime) {
  std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
  return ffi_dlopen::states.count(runtime) != 0;
}
void flaxFfiExperimentShutdown(FlaxRuntime *runtime) {
  std::shared_ptr<ffi_dlopen::State> state;
  {
    std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
    auto found = ffi_dlopen::states.find(runtime);
    if (found == ffi_dlopen::states.end())
      return;
    state = found->second;
  }
  state->close();
  std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
  ffi_dlopen::states.erase(runtime);
}
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_install(FlaxRuntime *runtime, FlaxError *error) {
  return enter(runtime, error, [&] {
    auto state = std::make_shared<ffi_dlopen::State>();
    state->runtime = runtime;
    {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      if (!ffi_dlopen::states.emplace(runtime, state).second)
        throw BridgeError(FLAX_STATE_ERROR, "dlopen FFI already installed");
    }
    try {
      auto &js = *runtime->js;
      ffi_dlopen::installGlobals(runtime, js, state);
    } catch (...) {
      flaxFfiExperimentShutdown(runtime);
      throw;
    }
  });
}
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_close(FlaxRuntime *runtime, FlaxError *error) {
  return enter(runtime, error, [&] { flaxFfiExperimentShutdown(runtime); });
}
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_checkpoint(FlaxRuntime *runtime, int64_t port,
                           FlaxError *error) {
  return enter(runtime, error, [&] {
    std::shared_ptr<ffi_dlopen::State> state;
    {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      state = ffi_dlopen::states.at(runtime);
    }
    state->port = port;
    state->checkpoint();
  });
}
namespace {
int32_t installFfi(FlaxRuntime *runtime, int64_t port, FlaxError *error) {
  const auto status = flax_ffi_dlopen_install(runtime, error);
  if (status != FLAX_OK)
    return status;
  std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
  ffi_dlopen::states.at(runtime)->port = port;
  return FLAX_OK;
}
int32_t checkpointFfi(FlaxRuntime *runtime, FlaxError *error) {
  return enter(runtime, error, [&] {
    std::shared_ptr<ffi_dlopen::State> state;
    {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      auto found = ffi_dlopen::states.find(runtime);
      if (found == ffi_dlopen::states.end())
        return;
      state = found->second;
    }
    state->checkpoint();
  });
}
int32_t retireFfi(FlaxRuntime *runtime, uint8_t *complete, FlaxError *error) {
  return enter(runtime, error, [&] {
    if (!complete)
      throw BridgeError(FLAX_ARGUMENT_ERROR, "Expected retirement result");
    std::shared_ptr<ffi_dlopen::State> state;
    {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      auto found = ffi_dlopen::states.find(runtime);
      if (found == ffi_dlopen::states.end()) {
        *complete = 1;
        return;
      }
      state = found->second;
    }
    *complete = state->retire(true);
    if (*complete) {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      ffi_dlopen::states.erase(runtime);
    }
  });
}
const FlaxEngineFfiApi ffiApi{FLAX_ENGINE_FFI_VERSION, sizeof(FlaxEngineFfiApi),
                              installFfi, checkpointFfi, retireFfi};
} // namespace
extern "C" __attribute__((visibility("default"))) const FlaxEngineFfiApi *
flax_engine_get_ffi_api(uint32_t version) {
  return version == FLAX_ENGINE_FFI_VERSION ? &ffiApi : nullptr;
}

#if defined(FLAX_FFI_TESTING)
#include "test-controls.inc"
#endif
