// Native workers never enter JSI or Dart; completion and destruction stay on the owner.
struct AsyncCall {
  AsyncCall() { ++liveAsyncCalls; }
  uint64_t id = 0;
  std::shared_ptr<Binding> binding;
  std::shared_ptr<Binding> discard;
  std::vector<Slot> arguments;
  Slot result;
  std::unique_ptr<fbjsi::Function> complete;
  std::unique_ptr<fbjsi::Value> roots;
  std::thread worker;
  std::atomic<bool> done{false}, cancelled{false};
  std::string error;
  int nativeError = 0;
  bool captureErrno = false;
  bool executed = false;
  void discardResult() {
    if (discard && executed)
      reinterpret_cast<void (*)(void *)>(discard->address)(load<void *>(result.data()));
  }
  ~AsyncCall() {
    if (worker.joinable()) worker.join();
    --liveAsyncCalls;
  }
};

void State::completeCalls(fbjsi::Runtime &js) {
  std::string completionError;
  // Remove before resolving: Promise hooks may reenter the FFI installation.
  std::vector<std::shared_ptr<AsyncCall>> completed;
  for (auto it = asyncCalls.begin(); it != asyncCalls.end();) {
    if (!(*it)->done) { ++it; continue; }
    auto call = *it;
    call->worker.join();
    --call->binding->library->active;
    completed.push_back(call);
    it = asyncCalls.erase(it);
  }
  for (auto &call : completed) {
    auto error = call->error;
    if (closing && !call->executed) error = "FFI session is closing";
    else if (call->cancelled) error = "FFI call was aborted";
    fbjsi::Value value;
    try {
      if (call->executed) value = output(js, *call->binding->result, call->result.data());
    } catch (const std::exception &failure) { error = failure.what(); }
    if (call->captureErrno) {
      fbjsi::Object captured(js);
      captured.setProperty(js, "value", std::move(value));
      captured.setProperty(js, "errno", call->nativeError);
      value = std::move(captured);
    }
    try {
      call->complete->call(js, fbjsi::String::createFromUtf8(js, error), value, call->executed);
    } catch (const std::exception &failure) {
      if (completionError.empty()) completionError = failure.what();
    }
    if (call->discard) --call->discard->library->active;
  }
  if (!completionError.empty()) {
    wake();
    fail(js, completionError, "ERR_FFI_CALL_FAILED");
  }
}

void installCalls(fbjsi::Runtime &js, const std::shared_ptr<State> &state) {
  js.global().setProperty(js, "__flaxFfiInvoke",
      fbjsi::Function::createFromHostFunction(js,
          fbjsi::PropNameID::forAscii(js, "ffiInvoke"), 0,
          [state](fbjsi::Runtime &js, const fbjsi::Value &,
                  const fbjsi::Value *args, size_t count) -> fbjsi::Value {
    std::optional<Entry> entry;
    if (state->runtime) entry.emplace(state->runtime);
    if (count == 1) {
      auto id = checkedSize(js, args[0]);
      for (auto &call : state->asyncCalls)
        if (call->id == id) call->cancelled = true;
      return {};
    }
    if ((count != 3 && count != 6) || !args[1].isObject() ||
        !args[1].getObject(js).isArray(js) || !args[2].isBool())
      fail(js, "Expected bound function, arguments and error capture flag");
    auto binding = boundFunction(js, *state, args[0]);
    auto array = args[1].getObject(js).getArray(js);
    std::vector<fbjsi::Value> values;
    for (size_t i = 0; i < array.size(js); ++i)
      values.push_back(array.getValueAtIndex(js, i));
    if (count == 3)
      return invoke(state->runtime, js, *state, *binding, values.data(),
                    values.size(), args[2].getBool());
    if (state->closing) fail(js, "FFI session is closing", "ERR_INVALID_STATE");
    if (!state->runtime || state->port == ILLEGAL_PORT)
      fail(js, "Background calls require an active Session checkpoint", "ERR_INVALID_STATE");
    if (state->asyncCalls.size() >= 32)
      fail(js, "Too many pending background FFI calls", "ERR_OUT_OF_RANGE");
    if (values.size() != binding->arguments.size())
      fail(js, "Invalid FFI argument count");
    auto call = std::make_shared<AsyncCall>();
    call->binding = binding;
    call->result = Slot(binding->result->size());
    call->captureErrno = args[2].getBool();
    call->complete = std::make_unique<fbjsi::Function>(args[3].getObject(js).getFunction(js));
    call->roots = std::make_unique<fbjsi::Value>(js, args[4]);
    if (!args[5].isUndefined()) {
      call->discard = boundFunction(js, *state, args[5]);
      if (binding->result->kind != Type::Pointer ||
          call->discard->result->kind != Type::Void ||
          call->discard->arguments.size() != 1 ||
          call->discard->arguments[0]->kind != Type::Pointer)
        fail(js, "onDiscard requires a pointer result and bound void(pointer) cleanup");
    }
    call->arguments.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      call->arguments.emplace_back(binding->arguments[i]->size());
      input(js, *state, *binding->arguments[i], values[i], call->arguments.back());
    }
    checkOpen(js, *state, *binding->library);
    if (call->discard) checkOpen(js, *state, *call->discard->library);
    // Argument getters may reenter and submit work or start Session retirement.
    if (state->closing) fail(js, "FFI session is closing", "ERR_INVALID_STATE");
    if (state->asyncCalls.size() >= 32)
      fail(js, "Too many pending background FFI calls", "ERR_OUT_OF_RANGE");
    call->id = state->nextCall++;
    state->asyncCalls.push_back(call);
    ++binding->library->active;
    if (call->discard) ++call->discard->library->active;
    try {
      call->worker = std::thread([state, raw = call.get()] {
        asyncError = &raw->error;
        try {
          if (!raw->cancelled && !state->closing) {
            std::vector<void *> addresses;
            for (auto &slot : raw->arguments) addresses.push_back(slot.data());
            if (raw->captureErrno) errno = 0;
            ++calls;
            ffi_call(&raw->binding->cif, FFI_FN(raw->binding->address),
                     raw->result.data(), addresses.data());
            raw->nativeError = errno;
            raw->executed = true;
          }
        } catch (const std::exception &error) { raw->error = error.what(); }
        catch (...) { raw->error = "Unknown background native failure"; }
        asyncError = nullptr;
        raw->done = true;
        state->wake();
      });
    } catch (...) {
      --binding->library->active;
      if (call->discard) --call->discard->library->active;
      state->asyncCalls.pop_back();
      throw;
    }
    return static_cast<double>(call->id);
  }));
}
