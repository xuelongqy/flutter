#include <chrono>

int32_t collectFfiTest(FlaxRuntime *runtime, FlaxError *error, bool sweep) {
  return protect(error, [&] {
    if (runtime) {
      if (runtime->depth || runtime->owner != Dart_CurrentIsolate())
        throw BridgeError(FLAX_STATE_ERROR,
                          "Collection requires the idle owner isolate");
      flaxFfiCollectJs(*runtime->js);
      std::shared_ptr<ffi_dlopen::State> state;
      {
        std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
        auto found = ffi_dlopen::states.find(runtime);
        if (found != ffi_dlopen::states.end())
          state = found->second;
      }
      if (state && sweep)
        state->sweep();
    } else
      Dart_FlaxCollect(false, true);
  });
}
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_collect(FlaxRuntime *runtime, FlaxError *error) {
  return collectFfiTest(runtime, error, true);
}
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_collect_pending(FlaxRuntime *runtime, FlaxError *error) {
  return collectFfiTest(runtime, error, false);
}
// Locate the real Session runtime without exposing a public native handle.
extern "C" __attribute__((visibility("default"))) int32_t
flax_ffi_dlopen_collect_session_pending(FlaxError *error) {
  return protect(error, [&] {
    FlaxRuntime *runtime = nullptr;
    {
      std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
      for (const auto &[candidate, state] : ffi_dlopen::states) {
        if (!candidate || candidate->owner != Dart_CurrentIsolate() || state->closed)
          continue;
        if (runtime)
          throw BridgeError(FLAX_STATE_ERROR, "Expected one active Session runtime");
        runtime = candidate;
      }
    }
    if (!runtime || runtime->depth)
      throw BridgeError(FLAX_STATE_ERROR, "Expected an idle Session runtime");
    flaxFfiCollectJs(*runtime->js);
  });
}
// Test control: exercise the actual existing Dart byte-view bridge, not a
// copied buffer.
extern "C" __attribute__((visibility("default"))) Dart_Handle
flax_ffi_dlopen_read_test_view(FlaxRuntime *runtime, FlaxError *error) {
  FlaxValueId id = 0;
  if (enter(runtime, error, [&] {
        id = keep(runtime, runtime->js->global().getProperty(*runtime->js,
                                                             "ffiSurvivor"));
      }) != FLAX_OK)
    return Dart_Null();
  auto result = readBuffer(runtime, id, 2, error);
  FlaxError ignored{};
  releaseValue(runtime, id, &ignored);
  clearError(&ignored);
  return result;
}
extern "C" __attribute__((visibility("default"))) uint64_t
flax_ffi_dlopen_stat(uint32_t index) {
  using namespace ffi_dlopen;
  switch (index) {
  case 0: {
    std::lock_guard<std::mutex> lock(statesMutex);
    return states.size();
  }
  case 1:
    return liveLibraries;
  case 2:
    return liveBindings;
  case 3:
    return livePointers;
  case 4:
    return calls;
  case 5: {
    std::lock_guard<std::mutex> lock(imagesMutex);
    return images.size();
  }
  case 6:
    return liveMemories;
  case 7:
    return liveViews;
  case 8:
    return liveCallbacks;
  case 9:
    return liveFinalizers;
  case 10:
    return liveFinalizerPeers;
  case 11:
    return liveAsyncCalls;
  case 12:
  case 13:
  case 15: {
    std::lock_guard<std::mutex> statesLock(statesMutex);
    uint64_t pending = 0;
    for (auto &[runtime, state] : states) {
      if (index == 15) { pending += state->closing ? 1 : 0; continue; }
      std::lock_guard<std::mutex> queueLock(state->queueMutex);
      for (auto &message : state->messages)
        pending += index == 13 ? message->bytes : message->synchronous ? 1 : 0;
    }
    return pending;
  }
  case 14:
    return deliveryTestHeld;
  case 16:
    return listenerTestBytes;
  case 17:
    return listenerTestReleases;
  case 18:
    return postedWakeups;
  case 19:
    return largestCleanupBatch;
  default:
    return UINT64_MAX;
  }
}

// Pause only after owner delivery finishes, so retirement can race the native return.
extern "C" __attribute__((visibility("default"))) void
flax_ffi_dlopen_pause_delivery(uint8_t paused) {
  std::lock_guard<std::mutex> lock(ffi_dlopen::deliveryTestMutex);
  ffi_dlopen::deliveryTestPaused = paused != 0;
  ffi_dlopen::deliveryTestReady.notify_all();
}

// Hold a real owner call until every previously completed delivery has returned.
extern "C" __attribute__((visibility("default"))) uint8_t
flax_ffi_dlopen_resume_delivery_wait() {
  std::vector<std::shared_ptr<ffi_dlopen::Callback>> callbacks;
  bool ownerCall = false;
  {
    std::lock_guard<std::mutex> lock(ffi_dlopen::statesMutex);
    for (const auto &[runtime, state] : ffi_dlopen::states) {
      if (state->owner != std::this_thread::get_id()) continue;
      ownerCall = ownerCall || state->ownerCalls != 0;
      for (const auto &[address, callback] : state->callbacks)
        callbacks.push_back(callback);
    }
  }
  if (!ownerCall || ffi_dlopen::deliveryTestHeld == 0) return 0;
  flax_ffi_dlopen_pause_delivery(0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::any_of(callbacks.begin(), callbacks.end(),
                     [](const auto &callback) { return callback->active != 0; })) {
    if (std::chrono::steady_clock::now() >= deadline) return 0;
    std::this_thread::yield();
  }
  return 1;
}

// Test-only entry to the embedder's real, deliberately non-public shutdown
// hook.
extern "C" __attribute__((visibility("default"))) void
flax_ffi_dlopen_shutdown_test() {
  flax_engine_shutdown_current_isolate();
}
