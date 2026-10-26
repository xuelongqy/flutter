// Pure initialization owns the shared heap exclusively; adoption stays on UI.
struct FlaxStartup {
  struct Source {
    std::string data, url;
  };
  std::shared_ptr<flax::JointHeap> heap;
  std::vector<Source> sources;
  Source entry;
  std::shared_ptr<const fbjsi::PreparedJavaScript> compiled;
  std::thread worker;
  std::atomic<bool> ready{false}, cancelled{false};
  bool fallback = false;
  std::string error;

  void run(fbjsi::Runtime& js) noexcept {
    try {
      for (const auto& source : sources) {
        if (cancelled)
          break;
        js.evaluateJavaScript(
            std::make_shared<fbjsi::StringBuffer>(source.data), source.url);
      }
      if (!cancelled) {
        compiled = js.prepareJavaScript(
            std::make_shared<fbjsi::StringBuffer>(entry.data), entry.url);
      }
    } catch (const std::exception& failure) {
      error = failure.what();
    } catch (...) {
      error = "JavaScript initialization failed";
    }
    ready.store(true, std::memory_order_release);
  }
};

void sharePreparedHeap() {
  FlaxRuntime* runtime = nullptr;
  {
    std::lock_guard<std::mutex> lock(runtimesMutex);
    auto it = runtimes.find(Dart_CurrentIsolate());
    // Preparation starts only with a sole runtime in this UI isolate.
    if (it == runtimes.end() || it->second.size() != 1)
      return;
    runtime = *it->second.begin();
  }
  auto job = runtime->startup;
  if (!job || !job->worker.joinable())
    return;
  if (!job->ready.load(std::memory_order_acquire)) {
    job->fallback = !job->cancelled.load();
    job->cancelled = true;
    job->heap->CancelPreparation();
  }
  job->worker.join();
  job->heap->EndPreparation();
}

void cancelStartup(FlaxRuntime* runtime) {
  auto job = runtime->startup;
  if (!job)
    return;
  job->cancelled = true;
  job->heap->CancelPreparation();
  if (job->worker.joinable()) {
    job->worker.join();
    job->heap->EndPreparation();
  }
  runtime->startup.reset();
}

int32_t prepareStartup(FlaxRuntime* runtime,
                       const FlaxSource* sources,
                       size_t count,
                       const FlaxSource* entry,
                       int32_t* started,
                       FlaxError* error) {
  return enter(
      runtime, error,
      [&] {
        flax::JointHeap::CheckPreparation();
        if (!started || !entry || (count && !sources) || count > 4096)
          throw BridgeError(FLAX_ARGUMENT_ERROR,
                            "Invalid initialization sources");
        *started = 0;
#if !defined(FML_OS_MACOSX)
        return;  // Other targets retain UI-thread startup until their own
                 // acceptance.
#endif
        // Other sessions may call JS synchronously. Never make them wait on a
        // worker.
        {
          std::lock_guard<std::mutex> lock(runtimesMutex);
          if (runtimes[runtime->owner].size() != 1)
            return;
        }
        if (!runtime->cold || !runtime->values.empty() ||
            runtime->preparedEntry)
          return;
#if defined(FLAX_ENABLE_FFI)
        if (flaxFfiExperimentInstalled(runtime))
          return;
#endif
        auto job = std::make_shared<FlaxStartup>();
        auto copy = [](const FlaxSource& source) {
          if (source.length > INT_MAX || source.url_length > INT_MAX)
            throw BridgeError(FLAX_ARGUMENT_ERROR,
                              "Initialization source too large");
          return FlaxStartup::Source{text(source.data, source.length),
                                     text(source.url, source.url_length)};
        };
        for (size_t i = 0; i < count; ++i)
          job->sources.push_back(copy(sources[i]));
        job->entry = copy(*entry);
        job->heap = flax::JointHeap::BeginPreparation();
        if (!job->heap)
          return;
        runtime->cold = false;
        runtime->startup = job;
        try {
          job->worker = std::thread([job, runtime] { job->run(*runtime->js); });
        } catch (...) {
          job->heap->EndPreparation();
          runtime->startup.reset();
          throw;
        }
        *started = 1;
      },
      true);
}

int32_t pollStartup(FlaxRuntime* runtime, int32_t* ready, FlaxError* error) {
  return enter(
      runtime, error,
      [&] {
        if (!ready || !runtime->startup)
          throw BridgeError(FLAX_STATE_ERROR, "No initialization in progress");
        auto job = runtime->startup;
        *ready = job->ready.load(std::memory_order_acquire);
        if (!*ready)
          return;
        if (job->worker.joinable()) {
          job->worker.join();
          job->heap->EndPreparation();
        }
        runtime->startup.reset();
        if (job->fallback) {
          job->compiled.reset();
          clearRuntime(runtime);
          initializeRuntime(runtime);
          *ready = -1;
          return;
        }
        if (job->cancelled)
          throw BridgeError(FLAX_STATE_ERROR,
                            "JavaScript initialization cancelled");
        if (!job->error.empty()) {
          // Pristine inputs have no host side effects. Retry on UI using the
          // same runtime object, rather than calling the application's factory
          // twice.
          clearRuntime(runtime);
          initializeRuntime(runtime);
          throw BridgeError(FLAX_JS_ERROR, job->error.c_str());
        }
        runtime->preparedEntry = std::move(job->compiled);
        runtime->preparedSource = std::move(job->entry.data);
        runtime->preparedUrl = std::move(job->entry.url);
      },
      true);
}

int32_t cancelStartupEntry(FlaxRuntime* runtime, FlaxError* error) {
  return enter(
      runtime, error,
      [&] {
        if (auto job = runtime->startup) {
          job->cancelled = true;
          job->heap->CancelPreparation();
        }
      },
      true);
}
