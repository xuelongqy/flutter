struct IsolatedHandler {
  std::unique_ptr<fbjsi::Runtime> js;
  std::shared_ptr<State> state;
  std::unique_ptr<fbjsi::Function> function;
  IsolatedHandler(const std::string &source, const std::string &context)
      : js(flaxFfiCreateCallbackRuntime()), state(std::make_shared<State>()) {
    try {
      installGlobals(nullptr, *js, state);
      js->evaluateJavaScript(std::make_shared<fbjsi::StringBuffer>(ffiScript),
                             "flax:isolated-ffi");
      auto code = "(function(context) { 'use strict'; return (" + source +
                  "); })(" + context + ")";
      auto result =
          js->evaluateJavaScript(std::make_shared<fbjsi::StringBuffer>(code),
                                 "flax:isolated-ffi-callback");
      if (!result.isObject() || !result.getObject(*js).isFunction(*js))
        fail(*js, "Isolated callback source must evaluate to a function");
      function = std::make_unique<fbjsi::Function>(
          result.getObject(*js).getFunction(*js));
    } catch (...) {
      state->close();
      throw;
    }
  }
  ~IsolatedHandler() {
    function.reset();
    state->close();
  }
};
struct Callback : std::enable_shared_from_this<Callback> {
  Callback() { ++liveCallbacks; }
  ~Callback() {
    if (closure)
      ffi_closure_free(closure);
    if (stopImage) {
      stopImage.reset();
      if (auto owner = state.lock()) owner->metadataChanged();
    }
    --liveCallbacks;
  }
  std::weak_ptr<State> state;
  fbjsi::Runtime *ownerJs = nullptr;
  std::shared_ptr<Binding> binding;
  std::unique_ptr<fbjsi::Function> strong;
  std::unique_ptr<fbjsi::WeakObject> weak;
  ffi_closure *closure = nullptr;
  void *address = nullptr;
  std::atomic<size_t> active{0};
  std::atomic<bool> accepting{true};
  std::string mode, source, context;
  Slot exceptional;
  std::mutex isolatedMutex;
  std::unordered_map<std::thread::id, std::unique_ptr<IsolatedHandler>>
      isolated;
  struct Copy {
    size_t argument, lengthArgument;
  };
  std::vector<Copy> copies;
  size_t maxPending = 1024, maxBytes = 16 * 1024 * 1024;
  std::atomic<size_t> pending{0}, pendingBytes{0};
  std::shared_ptr<Binding> stop;
  std::shared_ptr<Image> stopImage;
  void *stopToken = nullptr;
  bool stopped = false;
  void stopProducer() {
    if (!stop || stopped)
      return;
    accepting = false;
    if (auto owner = state.lock()) owner->cancelDeliveries(nullptr, this);
    stopped = true;
    if (stop->arguments.empty())
      reinterpret_cast<void (*)(void)>(stop->address)();
    else
      reinterpret_cast<void (*)(void *)>(stop->address)(stopToken);
    stopImage.reset();
    if (auto owner = state.lock()) owner->metadataChanged();
  }
};
struct SyncDelivery {
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
  bool cancelled = false;
  bool ownerCallError = false;
  Slot result;
  std::string error;
  void finish(std::string failure = {}, bool cancel = false, bool ownerError = false) {
    std::lock_guard<std::mutex> lock(mutex);
    if (done) return;
    error = std::move(failure);
    cancelled = cancel;
    ownerCallError = ownerError;
    done = true;
    ready.notify_one();
  }
};
#ifdef FLAX_FFI_TESTING
std::mutex deliveryTestMutex;
std::condition_variable deliveryTestReady;
bool deliveryTestPaused = false;
std::atomic<uint64_t> deliveryTestHeld{0};
std::atomic<uint64_t> listenerTestBytes{0}, listenerTestReleases{0};
void pauseCompletedDeliveryForTest() {
  std::unique_lock<std::mutex> lock(deliveryTestMutex);
  if (!deliveryTestPaused) return;
  ++deliveryTestHeld;
  deliveryTestReady.wait(lock, [] { return !deliveryTestPaused; });
  --deliveryTestHeld;
}
#endif
struct Message {
  std::weak_ptr<Callback> callback;
  std::vector<Slot> arguments;
  std::string error;
  size_t bytes = 0;
  std::shared_ptr<SyncDelivery> synchronous;
  ~Message() {
#ifdef FLAX_FFI_TESTING
    listenerTestBytes -= bytes;
    if (bytes) ++listenerTestReleases;
#endif
    if (auto owner = callback.lock()) {
      --owner->pending;
      owner->pendingBytes -= bytes;
    }
  }
};
thread_local Callback *executingCallback = nullptr;
void queueError(const std::shared_ptr<State> &state, std::string error, bool synchronous = false) {
  if (asyncError) { *asyncError = std::move(error); return; }
  auto message = std::make_shared<Message>();
  message->error = std::move(error);
  {
    std::lock_guard<std::mutex> lock(state->queueMutex);
    if (synchronous && state->ownerCalls) {
      state->foreignCallbackError = message->error;
      return;
    }
    if (state->messages.size() > 1024)
      return;
    state->messages.push_back(message);
  }
  state->wake();
}
void callbackResult(fbjsi::Runtime &js, State &state, Callback &callback,
                    fbjsi::Function &function, void *result, void **arguments) {
  struct Current {
    Callback *previous = executingCallback;
    explicit Current(Callback *value) { executingCallback = value; }
    ~Current() { executingCallback = previous; }
  } current(&callback);
  std::vector<fbjsi::Value> values;
  for (size_t i = 0; i < callback.binding->arguments.size(); ++i)
    values.push_back(output(js, *callback.binding->arguments[i], arguments[i]));
  auto value = function.call(
      js, static_cast<const fbjsi::Value *>(values.data()), values.size());
  if (value.isObject()) {
    auto then = value.getObject(js).getProperty(js, "then");
    if (then.isObject() && then.getObject(js).isFunction(js))
      fail(js, "Synchronous native callbacks cannot return a Promise");
  }
  if (callback.binding->result->kind != Type::Void) {
    Slot converted(callback.binding->result->size());
    input(js, state, *callback.binding->result, value, converted);
    // A temporary string or borrowed JS buffer cannot outlive the native
    // callback.
    if (callback.binding->result->kind == Type::Pointer &&
        (!converted.string.empty() || converted.source.storage))
      fail(js, "Callback pointer results must use an explicitly retained "
               "bigint address");
    std::memcpy(result, converted.data(), callback.binding->result->size());
  }
}
uint64_t nativeLength(const NativeType &type, const void *value) {
  switch (type.kind) {
  case Type::I8: {
    auto n = load<int8_t>(value);
    if (n < 0)
      throw std::invalid_argument("Negative callback length");
    return n;
  }
  case Type::U8:
    return load<uint8_t>(value);
  case Type::I16: {
    auto n = load<int16_t>(value);
    if (n < 0)
      throw std::invalid_argument("Negative callback length");
    return n;
  }
  case Type::U16:
    return load<uint16_t>(value);
  case Type::I32: {
    auto n = load<int32_t>(value);
    if (n < 0)
      throw std::invalid_argument("Negative callback length");
    return n;
  }
  case Type::U32:
    return load<uint32_t>(value);
  case Type::I64: {
    auto n = load<int64_t>(value);
    if (n < 0)
      throw std::invalid_argument("Negative callback length");
    return n;
  }
  case Type::U64:
    return load<uint64_t>(value);
  default:
    throw std::invalid_argument(
        "Callback copy length must have an integer type");
  }
}
void dispatchCallback(ffi_cif *, void *result, void **arguments,
                      void *data) noexcept {
  auto *raw = static_cast<Callback *>(data);
  ++raw->active;
  struct Exit {
    Callback *callback;
    ~Exit() {
      auto state = callback->state.lock();
      --callback->active;
      if (state && state->retiring) state->wake();
    }
  } exit{raw};
  const auto size = raw->binding->result->kind == Type::Void
                        ? 0
                        : raw->binding->result->size();
  if (size)
    std::memcpy(result, raw->exceptional.data(), size);
  auto state = raw->state.lock();
  if (!state || !raw->accepting || state->closed || state->closing || state->retiring)
    return;
  bool ownerCallError = false;
  try {
    auto callback = raw->shared_from_this();
    if (raw->mode == "threadsafe" && state->owner != std::this_thread::get_id()) {
      auto message = std::make_shared<Message>();
      auto delivery = std::make_shared<SyncDelivery>();
      delivery->result = Slot(raw->binding->result->size());
      message->synchronous = delivery;
      for (size_t i = 0; i < raw->binding->arguments.size(); ++i) {
        message->arguments.emplace_back(raw->binding->arguments[i]->size());
        std::memcpy(message->arguments.back().data(), arguments[i],
                    raw->binding->arguments[i]->size());
      }
      {
        std::lock_guard<std::mutex> lock(state->queueMutex);
        if (!raw->accepting || state->closed || state->closing || state->retiring) return;
        if (state->ownerCalls) {
          ownerCallError = true;
          throw std::runtime_error("Synchronous owner FFI call would deadlock; use callAsync");
        }
        if (raw->pending >= raw->maxPending)
          throw std::runtime_error("FFI callback notification queue is full");
        ++raw->pending;
        message->callback = callback;
        state->messages.push_back(message);
      }
      state->wake();
      std::unique_lock<std::mutex> lock(delivery->mutex);
      delivery->ready.wait(lock, [&] { return delivery->done; });
#ifdef FLAX_FFI_TESTING
      pauseCompletedDeliveryForTest();
#endif
      // A later close cannot revoke a result or exception already delivered by JS.
      if (delivery->cancelled) return;
      // A completed JS failure belongs to its delivery, not an unrelated owner call.
      ownerCallError = delivery->ownerCallError;
      if (!delivery->error.empty()) throw std::runtime_error(delivery->error);
      if (size) std::memcpy(result, delivery->result.data(), size);
      return;
    }
    if (raw->mode == "listener") {
      size_t totalBytes = 0;
      std::vector<size_t> lengths;
      for (auto &copy : raw->copies) {
        const auto length =
            nativeLength(*raw->binding->arguments[copy.lengthArgument],
                         arguments[copy.lengthArgument]);
        if (length > raw->maxBytes - totalBytes)
          throw std::runtime_error(
              "FFI callback copied payload limit exceeded");
        totalBytes += length;
        lengths.push_back(length);
      }
      auto message = std::make_shared<Message>();
      {
        std::lock_guard<std::mutex> lock(state->queueMutex);
        if (raw->pending >= raw->maxPending)
          throw std::runtime_error("FFI callback notification queue is full");
        if (raw->pendingBytes > raw->maxBytes - totalBytes)
          throw std::runtime_error(
              "FFI callback copied payload limit exceeded");
        ++callback->pending;
        callback->pendingBytes += totalBytes;
        message->callback = callback;
        message->bytes = totalBytes;
#ifdef FLAX_FFI_TESTING
        listenerTestBytes += totalBytes;
#endif
      }
      for (size_t i = 0; i < raw->binding->arguments.size(); ++i) {
        message->arguments.emplace_back(raw->binding->arguments[i]->size());
        std::memcpy(message->arguments.back().data(), arguments[i],
                    raw->binding->arguments[i]->size());
      }
      for (size_t i = 0; i < raw->copies.size(); ++i) {
        const auto &copy = raw->copies[i];
        const auto length = lengths[i];
        auto pointer = load<const uint8_t *>(arguments[copy.argument]);
        if (!pointer && length)
          throw std::runtime_error("Null callback copy source");
        auto &slot = message->arguments[copy.argument];
        if (length)
          slot.string.assign(pointer, pointer + length);
        store(slot.data(), slot.string.empty() ? nullptr : slot.string.data());
      }
      {
        std::lock_guard<std::mutex> lock(state->queueMutex);
        if (!raw->accepting || state->closed || state->closing || state->retiring) return;
        state->messages.push_back(message);
      }
      state->wake();
      return;
    }
    if (raw->mode == "isolated") {
      IsolatedHandler *handler;
      {
        std::lock_guard<std::mutex> lock(raw->isolatedMutex);
        auto &entry = raw->isolated[std::this_thread::get_id()];
        if (!entry)
          entry = std::make_unique<IsolatedHandler>(raw->source, raw->context);
        handler = entry.get();
      }
      callbackResult(*handler->js, *handler->state, *raw, *handler->function,
                     result, arguments);
      return;
    }
    if (state->owner != std::this_thread::get_id())
      throw std::runtime_error("Synchronous FFI callback requires its owner "
                               "isolate/thread; use listener or isolated mode");
    std::optional<tonic::DartIsolateScope> isolate;
    if (state->runtime) {
      isolate.emplace(state->runtime->owner);
      Dart_EnterScope();
    }
    struct Scope {
      bool entered;
      ~Scope() {
        if (entered)
          Dart_ExitScope();
      }
    } scope{state->runtime != nullptr};
    auto &js = state->runtime ? *state->runtime->js : *raw->ownerJs;
    auto functionValue = raw->weak->lock(js);
    if (!functionValue.isObject())
      return;
    auto function = functionValue.getObject(js).getFunction(js);
    std::optional<Entry> entry;
    if (state->runtime)
      entry.emplace(state->runtime);
    callbackResult(js, *state, *raw, function, result, arguments);
  } catch (const std::exception &error) {
    if (raw->mode == "sync" && state->owner == std::this_thread::get_id() &&
        state->runtime && state->runtime->depth)
      state->callbackError = error.what();
    else
      queueError(state, error.what(), ownerCallError);
  } catch (...) {
    queueError(state, "Unknown native callback failure");
  }
}
fbjsi::Value callbackOperation(fbjsi::Runtime &js,
                               const std::shared_ptr<State> &state,
                               const std::shared_ptr<Library> &library,
                               const std::string &operation,
                               const fbjsi::Value *args, size_t count) {
  if (operation == "registerCallback") {
    if (!state->runtime)
      fail(js,
           "Independent callback runtimes cannot retain new native callbacks");
    if (count != 3 || !args[2].isObject())
      fail(js, "Expected callback signature, handler and options");
    auto callback = std::make_shared<Callback>();
    callback->state = state;
    callback->ownerJs = &js;
    callback->binding = prepare(js, library, args[0]);
    callback->exceptional = Slot(callback->binding->result->size());
    auto signature = args[0].getObject(js);
    if (!signature.getProperty(js, "fixedArguments").isUndefined())
      fail(js, "Variadic callbacks are not supported by the C ABI");
    auto options = args[2].getObject(js);
    auto mode = options.getProperty(js, "mode");
    callback->mode = mode.isUndefined() ? "sync" : text(js, mode);
    if (callback->mode != "sync" && callback->mode != "listener" &&
        callback->mode != "isolated" && callback->mode != "threadsafe")
      fail(js, "Unknown callback mode");
    if (!state->runtime && callback->mode == "listener")
      fail(js, "Listener callbacks require a scheduled Flax session");
    if (callback->mode == "isolated") {
      callback->source = text(js, args[1]);
      auto context = options.getProperty(js, "context");
      callback->context = context.isUndefined()
                              ? "null"
                              : js.global()
                                    .getPropertyAsObject(js, "JSON")
                                    .getPropertyAsFunction(js, "stringify")
                                    .call(js, context)
                                    .getString(js)
                                    .utf8(js);
      // Validate source before exposing a native address. Runtime instances
      // used by workers are independent; no closure, host object or UI runtime
      // is shared.
      IsolatedHandler validation(callback->source, callback->context);
    } else {
      if (!args[1].isObject() || !args[1].getObject(js).isFunction(js))
        fail(js, "Expected a JavaScript callback", "ERR_INVALID_ARG_TYPE");
      auto function = args[1].getObject(js).getFunction(js);
      callback->weak = std::make_unique<fbjsi::WeakObject>(js, function);
      callback->strong = std::make_unique<fbjsi::Function>(std::move(function));
    }
    if (callback->mode == "listener" &&
        callback->binding->result->kind != Type::Void)
      fail(js, "Listener callbacks must return void");
    auto exceptional = options.getProperty(js, "exceptionalReturn");
    if (!exceptional.isUndefined() &&
        callback->binding->result->kind != Type::Void)
      input(js, *state, *callback->binding->result, exceptional,
            callback->exceptional);
    if (callback->binding->result->kind == Type::Pointer &&
        (!callback->exceptional.string.empty() ||
         callback->exceptional.source.storage))
      fail(js, "Exceptional pointer results require an explicitly retained "
               "bigint address");
    auto stop = options.getProperty(js, "stop");
    if (!stop.isUndefined()) {
      callback->stop = boundFunction(js, *state, stop);
      if (callback->stop->result->kind != Type::Void ||
          callback->stop->arguments.size() > 1 ||
          (!callback->stop->arguments.empty() &&
           callback->stop->arguments[0]->kind != Type::Pointer))
        fail(js, "Callback stop must be a bound void() or void(pointer) native "
                 "function");
      callback->stopImage = callback->stop->library->image;
      callback->stopToken = pointer(js, options.getProperty(js, "stopToken"));
    }
    auto maxPending = options.getProperty(js, "maxPending"),
         maxBytes = options.getProperty(js, "maxBytes");
    if (!maxPending.isUndefined())
      callback->maxPending = checkedSize(js, maxPending);
    if (!maxBytes.isUndefined())
      callback->maxBytes = checkedSize(js, maxBytes);
    if (!callback->maxPending || !callback->maxBytes)
      fail(js, "Callback queue limits must be positive");
    auto copies = options.getProperty(js, "copies");
    if (!copies.isUndefined()) {
      if (callback->mode != "listener" || !copies.isObject() ||
          !copies.getObject(js).isArray(js))
        fail(js, "Copied payloads require listener mode and a copies array");
      auto entries = copies.getObject(js).getArray(js);
      std::unordered_set<size_t> copied;
      for (size_t i = 0; i < entries.size(js); ++i) {
        auto copy = entries.getValueAtIndex(js, i).getObject(js);
        auto arg = checkedSize(js, copy.getProperty(js, "argument"));
        auto len = checkedSize(js, copy.getProperty(js, "lengthArgument"));
        if (arg >= callback->binding->arguments.size() ||
            len >= callback->binding->arguments.size() ||
            callback->binding->arguments[arg]->kind != Type::Pointer ||
            !copied.insert(arg).second)
          fail(js, "Invalid copied callback argument");
        const auto kind = callback->binding->arguments[len]->kind;
        if (kind > Type::U64)
          fail(js, "Callback length argument must be an integer");
        callback->copies.push_back({arg, len});
      }
    }
    callback->closure = static_cast<ffi_closure *>(
        ffi_closure_alloc(sizeof(ffi_closure), &callback->address));
    if (!callback->closure)
      fail(js, "Could not allocate a precompiled callback entry",
           "ERR_RX_MEMORY_NOT_SUPPORTED");
    if (ffi_prep_closure_loc(callback->closure, &callback->binding->cif,
                             dispatchCallback, callback.get(),
                             callback->address) != FFI_OK)
      fail(js, "Could not prepare native callback", "ERR_FFI_CALL_FAILED");
    checkOpen(js, *state, *library);
    state->callbacks.emplace(callback->address, callback);
    return wrapPointer(js, callback->address);
  }
  if (count != 1)
    fail(js, "Expected one callback pointer");
  auto found = state->callbacks.find(pointer(js, args[0], true));
  if (found == state->callbacks.end() ||
      found->second->binding->library != library)
    fail(js, "Callback does not belong to this library");
  auto &callback = *found->second;
  if (operation == "unregisterCallback") {
    if (executingCallback == &callback)
      fail(js, "Cannot unregister an active callback", "ERR_INVALID_STATE");
    if (callback.stop && executingCallback && executingCallback->state.lock() == state)
      fail(js, "Cannot stop a producer from inside a native callback",
           "ERR_INVALID_STATE");
    if (callback.stop) callback.stopProducer();
    else state->cancelDeliveries(nullptr, &callback);
    if (callback.active)
      fail(js, "Cannot unregister an active callback", "ERR_INVALID_STATE");
    state->callbacks.erase(callback.address);
  } else {
    if (callback.mode == "isolated")
      fail(js, "An isolated source handler has no weak JS closure");
    auto function = callback.weak->lock(js);
    if (!function.isObject())
      fail(js, "Callback was garbage collected");
    if (operation == "refCallback")
      callback.strong = std::make_unique<fbjsi::Function>(
          function.getObject(js).getFunction(js));
    else
      callback.strong.reset();
  }
  return {};
}
void State::drain(fbjsi::Runtime &js) {
  completeCalls(js);
  std::string error;
  for (size_t turn = 0; turn < 64; ++turn) {
    std::shared_ptr<Message> message;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      if (messages.empty())
        break;
      message = std::move(messages.front());
      messages.pop_front();
    }
    if (!message->error.empty()) {
      error = message->error;
      break; // Report one error per checkpoint; leave later messages queued.
    }
    auto callback = message->callback.lock();
    if (!callback || !callback->accepting || callback->binding->library->closed ||
        closed || closing || retiring) {
      if (message->synchronous) message->synchronous->finish({}, true);
      continue;
    }
    auto function = callback->weak->lock(js);
    if (!function.isObject()) {
      if (message->synchronous) message->synchronous->finish();
      continue;
    }
    std::vector<void *> arguments;
    for (auto &slot : message->arguments)
      arguments.push_back(slot.data());
    try {
      ++callback->active;
      auto handler = function.getObject(js).getFunction(js);
      callbackResult(js, *this, *callback, handler,
          message->synchronous ? message->synchronous->result.data() : nullptr,
          arguments.data());
      --callback->active;
      if (message->synchronous) message->synchronous->finish();
    } catch (const std::exception &failure) {
      --callback->active;
      if (message->synchronous) message->synchronous->finish(failure.what());
      else {
        error = failure.what();
        break;
      }
    }
  }
  bool remaining;
  {
    std::lock_guard<std::mutex> lock(queueMutex);
    remaining = !messages.empty();
  }
  if (remaining)
    wake();
  if (!error.empty())
    fail(js, error, "ERR_FFI_CALLBACK_FAILED");
}
void State::cancelDeliveries(Library *library, Callback *target) {
  std::lock_guard<std::mutex> lock(queueMutex);
  for (auto it = messages.begin(); it != messages.end();) {
    auto callback = (*it)->callback.lock();
    const bool revoked = closed || closing || retiring || (callback && !callback->accepting);
    const bool selected = (!target || callback.get() == target) &&
        (!library || !callback || callback->binding->library.get() == library);
    const bool discardNotification = callback && (target || library || revoked);
    if (selected && ((*it)->synchronous || discardNotification)) {
      if ((*it)->synchronous) {
        if (revoked) (*it)->synchronous->finish({}, true);
        else (*it)->synchronous->finish("FFI synchronous delivery cancelled to avoid a wait cycle", false, true);
      }
      it = messages.erase(it);
    } else ++it;
  }
}
struct Finalization {
  Finalization() { ++liveFinalizers; }
  ~Finalization() {
    image.reset();
    if (auto owner = state.lock()) owner->metadataChanged();
    --liveFinalizers;
  }
  std::shared_ptr<Image> image;
  std::weak_ptr<State> state;
  std::atomic<bool> done{false};
  void (*cleanup)(void*) = nullptr;
  void* token = nullptr;
  uint64_t id = 0;
  std::unique_ptr<fbjsi::WeakObject> externalWitness;
  void releasePeer(fbjsi::Runtime& js);
  void finish() {
    if (done.exchange(true))
      return;
    cleanup(token);
    image.reset();
  }
  void detach() {
    done = true;
    image.reset();
  }
};
struct FinalizerPeer : fbjsi::NativeState {
  explicit FinalizerPeer(std::shared_ptr<Finalization> record)
      : record(std::move(record)) {
    ++liveFinalizerPeers;
  }
  ~FinalizerPeer() override {
    --liveFinalizerPeers;
    if (record && !record->done) {
      if (auto state = record->state.lock()) {
        {
          std::lock_guard<std::mutex> lock(state->queueMutex);
          if (state->closed) return;
          state->readyFinalizers.push_back(record->id);
        }
        state->wake();
      }
    }
  }
  std::shared_ptr<Finalization> record;
};
struct FinalizerFactory {
  std::shared_ptr<Image> image;
  std::weak_ptr<State> state;
  ~FinalizerFactory() {
    image.reset();
    if (auto owner = state.lock()) owner->metadataChanged();
  }
};
void Finalization::releasePeer(fbjsi::Runtime& js) {
  if (!externalWitness)
    return;
  auto reference = std::move(externalWitness);
  auto witness = reference->lock(js);
  if (witness.isObject()) {
    auto owner = witness.getObject(js);
    if (auto scope = state.lock())
      scope->detachFinalizer->call(js, owner);
    owner.setExternalMemoryPressure(js, 0);
    std::static_pointer_cast<FinalizerPeer>(owner.getNativeState(js))->record.reset();
  }
}
fbjsi::Value makeFinalizer(fbjsi::Runtime &js,
                           const std::shared_ptr<State> &state,
                           const fbjsi::Value &value) {
  if (!state->runtime)
    fail(
        js,
        "Independent callback runtimes require explicit native memory release");
  auto binding = boundFunction(js, *state, value);
  if (binding->result->kind != Type::Void || binding->arguments.size() != 1 ||
      binding->arguments[0]->kind != Type::Pointer)
    fail(js, "NativeFinalizer requires a bound void(pointer) function");
  checkOpen(js, *state, *binding->library);
  auto factory = std::make_shared<FinalizerFactory>();
  factory->image = binding->library->image;
  factory->state = state;
  fbjsi::Object result(js);
  for (const char *name : {"attach", "detach"})
    result.setProperty(
        js, name,
        fbjsi::Function::createFromHostFunction(
            js, fbjsi::PropNameID::forAscii(js, name), 0,
            [state, binding, factory,
             name](fbjsi::Runtime &js, const fbjsi::Value &,
                   const fbjsi::Value *args, size_t count) -> fbjsi::Value {
              std::optional<Entry> entry;
              if (state->runtime)
                entry.emplace(state->runtime);
              if (state->closed || state->retiring)
                fail(js, "FFI installation is closed", "ERR_INVALID_STATE");
              if (std::string(name) == "detach") {
                if (count != 1 || !args[0].isObject() ||
                    !args[0].getObject(js).isArray(js))
                  fail(js, "Expected finalizer attachment ids", "ERR_INVALID_ARG_TYPE");
                auto ids = args[0].getObject(js).getArray(js);
                std::vector<std::shared_ptr<Finalization>> pending;
                for (size_t i = 0; i < ids.size(js); ++i) {
                  auto id = checkedSize(js, ids.getValueAtIndex(js, i));
                  auto found = state->finalizers.find(id);
                  if (found == state->finalizers.end() || found->second->done)
                    fail(js, "Finalizer attachment is not pending", "ERR_INVALID_STATE");
                  if (std::find(pending.begin(), pending.end(), found->second) != pending.end())
                    fail(js, "Duplicate finalizer attachment id");
                  pending.push_back(found->second);
                }
                // Validate the group before consuming any cancellation responsibility.
                for (auto &record : pending) record->detach();
                for (auto &record : pending) record->releasePeer(js);
                for (auto &record : pending) state->finalizers.erase(record->id);
                return {};
              }
              if (count != 3 || !args[0].isObject())
                fail(js, "Expected finalizer target, token and external size");
              auto externalSize = checkedSize(js, args[2]);
              auto record = std::make_shared<Finalization>();
              record->state = state;
              record->image = factory->image;
              record->cleanup =
                  reinterpret_cast<void (*)(void *)>(binding->address);
              record->token = pointer(js, args[1]);
              record->id = state->nextFinalizer++;
              fbjsi::Object owner(js);
              owner.setNativeState(js, std::make_shared<FinalizerPeer>(record));
              try {
                owner.setExternalMemoryPressure(js, externalSize);
                record->externalWitness =
                    std::make_unique<fbjsi::WeakObject>(js, owner);
                state->attachFinalizer->call(js, args[0], owner);
                state->finalizers.emplace(record->id, record);
              } catch (...) {
                // Failed attachment leaves ownership with the caller.
                record->detach();
                owner.setExternalMemoryPressure(js, 0);
                std::static_pointer_cast<FinalizerPeer>(owner.getNativeState(js))
                    ->record.reset();
                record->releasePeer(js);
                throw;
              }
              return static_cast<double>(record->id);
            }));
  return result;
}
fbjsi::Value memoryBind(FlaxRuntime *runtime, fbjsi::Runtime &js,
                        const std::shared_ptr<State> &state,
                        const fbjsi::Value *args, size_t count) {
  if (count != 2)
    fail(js, "Expected function pointer and signature");
  auto binding = prepare(js, state->addressLibrary, args[1]);
  binding->address = pointer(js, args[0]);
  binding->name = "nativeFunction";
  if (!binding->address)
    fail(js, "Cannot bind a null function pointer");
  return callable(runtime, js, state, binding);
}
