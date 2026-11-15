// V8 keeps its default queues. Worker threads only notify the owning Dart event
// loop.
struct ForegroundState {
  std::atomic<bool> closed{false};
  std::atomic<size_t> ready{0};
  std::mutex mutex;
  std::map<const void*, std::function<void()>> callbacks;

  void notify() {
    std::vector<std::function<void()>> pending;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed)
        return;
      for (auto& entry : callbacks)
        pending.push_back(entry.second);
    }
    for (auto& callback : pending)
      callback();
  }
};

struct ForegroundTicket {
  explicit ForegroundTicket(std::shared_ptr<ForegroundState> value)
      : state(std::move(value)) {}
  std::shared_ptr<ForegroundState> state;
  bool counted = false, done = false;
  bool count() {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (done || counted || state->closed)
      return false;
    counted = true;
    ++state->ready;
    return true;
  }
  void complete() {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (done)
      return;
    done = true;
    if (counted)
      --state->ready;
  }
};

class ForegroundTask final : public v8::Task {
 public:
  ForegroundTask(std::shared_ptr<ForegroundTicket> ticket,
                 std::unique_ptr<v8::Task> task)
      : ticket_(std::move(ticket)), task_(std::move(task)) {}
  ~ForegroundTask() override { ticket_->complete(); }
  void Run() override {
    ticket_->complete();
    if (!ticket_->state->closed)
      task_->Run();
  }

 private:
  std::shared_ptr<ForegroundTicket> ticket_;
  std::unique_ptr<v8::Task> task_;
};

class ForegroundRunner final : public v8::TaskRunner {
 public:
  ForegroundRunner(v8::Platform* base,
                   std::shared_ptr<v8::TaskRunner> runner,
                   std::shared_ptr<ForegroundState> state,
                   v8::TaskPriority priority)
      : base_(base),
        runner_(std::move(runner)),
        state_(std::move(state)),
        priority_(priority) {}
  bool IdleTasksEnabled() override { return runner_->IdleTasksEnabled(); }
  bool NonNestableTasksEnabled() const override {
    return runner_->NonNestableTasksEnabled();
  }
  bool NonNestableDelayedTasksEnabled() const override {
    return runner_->NonNestableDelayedTasksEnabled();
  }

 private:
  void post(std::unique_ptr<v8::Task> task,
            bool non_nestable,
            const v8::SourceLocation& location) {
    if (state_->closed)
      return;
    auto ticket = std::make_shared<ForegroundTicket>(state_);
    ticket->count();
    auto counted = std::make_unique<ForegroundTask>(ticket, std::move(task));
    if (non_nestable)
      runner_->PostNonNestableTask(std::move(counted), location);
    else
      runner_->PostTask(std::move(counted), location);
    state_->notify();
  }

  class DelayedWake final : public v8::Task {
   public:
    explicit DelayedWake(std::shared_ptr<ForegroundTicket> ticket)
        : ticket_(std::move(ticket)) {}
    void Run() override {
      if (ticket_->count())
        ticket_->state->notify();
    }

   private:
    std::shared_ptr<ForegroundTicket> ticket_;
  };

  void delayed(std::unique_ptr<v8::Task> task,
               double delay,
               bool non_nestable,
               const v8::SourceLocation& location) {
    // V8 owns delayed tasks through shutdown; the worker timer owns only a wake
    // ticket.
    auto ticket = std::make_shared<ForegroundTicket>(state_);
    auto counted = std::make_unique<ForegroundTask>(ticket, std::move(task));
    if (non_nestable)
      runner_->PostNonNestableDelayedTask(std::move(counted), delay, location);
    else
      runner_->PostDelayedTask(std::move(counted), delay, location);
    base_->PostDelayedTaskOnWorkerThread(
        priority_, std::make_unique<DelayedWake>(ticket), delay, location);
  }
  void PostTaskImpl(std::unique_ptr<v8::Task> task,
                    const v8::SourceLocation& location) override {
    post(std::move(task), false, location);
  }
  void PostNonNestableTaskImpl(std::unique_ptr<v8::Task> task,
                               const v8::SourceLocation& location) override {
    post(std::move(task), true, location);
  }
  void PostDelayedTaskImpl(std::unique_ptr<v8::Task> task,
                           double delay,
                           const v8::SourceLocation& location) override {
    delayed(std::move(task), delay, false, location);
  }
  void PostNonNestableDelayedTaskImpl(
      std::unique_ptr<v8::Task> task,
      double delay,
      const v8::SourceLocation& location) override {
    delayed(std::move(task), delay, true, location);
  }
  void PostIdleTaskImpl(std::unique_ptr<v8::IdleTask> task,
                        const v8::SourceLocation& location) override {
    runner_->PostIdleTask(std::move(task), location);
  }
  v8::Platform* base_;
  std::shared_ptr<v8::TaskRunner> runner_;
  std::shared_ptr<ForegroundState> state_;
  v8::TaskPriority priority_;
};

class ForegroundPlatform final : public v8::Platform {
 public:
  explicit ForegroundPlatform(v8::Platform* base) : base_(base) {}
  std::shared_ptr<ForegroundState> state(v8::Isolate* isolate) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& value = states_[isolate];
    if (!value)
      value = std::make_shared<ForegroundState>();
    return value;
  }
  std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
      v8::Isolate* isolate,
      v8::TaskPriority priority) override {
    auto shared_state = state(isolate);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = std::make_pair(isolate, priority);
    auto& runner = runners_[key];
    if (!runner)
      runner = std::make_shared<ForegroundRunner>(
          base_, base_->GetForegroundTaskRunner(isolate, priority),
          shared_state, priority);
    return runner;
  }
  void close(v8::Isolate* isolate) {
    std::lock_guard<std::mutex> map_lock(mutex_);
    auto found = states_.find(isolate);
    if (found != states_.end()) {
      auto shared_state = found->second;
      std::lock_guard<std::mutex> lock(shared_state->mutex);
      shared_state->closed = true;
      shared_state->callbacks.clear();
      states_.erase(found);
    }
    for (auto it = runners_.begin(); it != runners_.end();) {
      if (it->first.first == isolate)
        it = runners_.erase(it);
      else
        ++it;
    }
  }
  v8::PageAllocator* GetPageAllocator() override {
    return base_->GetPageAllocator();
  }
  v8::ThreadIsolatedAllocator* GetThreadIsolatedAllocator() override {
    return base_->GetThreadIsolatedAllocator();
  }
  size_t GetZeroSegmentSize() override { return base_->GetZeroSegmentSize(); }
  void OnCriticalMemoryPressure() override {
    base_->OnCriticalMemoryPressure();
  }
  int NumberOfWorkerThreads() override {
    return base_->NumberOfWorkerThreads();
  }
  bool IdleTasksEnabled(v8::Isolate* isolate) override {
    return base_->IdleTasksEnabled(isolate);
  }
  std::unique_ptr<v8::ScopedBoostablePriority> CreateBoostablePriorityScope()
      override {
    return base_->CreateBoostablePriorityScope();
  }
  std::unique_ptr<v8::ScopedBlockingCall> CreateBlockingScope(
      v8::BlockingType type) override {
    return base_->CreateBlockingScope(type);
  }
  double MonotonicallyIncreasingTime() override {
    return base_->MonotonicallyIncreasingTime();
  }
  double CurrentClockTimeMillis() override {
    return base_->CurrentClockTimeMillis();
  }
  int64_t CurrentClockTimeMilliseconds() override {
    return base_->CurrentClockTimeMilliseconds();
  }
  double CurrentClockTimeMillisecondsHighResolution() override {
    return base_->CurrentClockTimeMillisecondsHighResolution();
  }
  StackTracePrinter GetStackTracePrinter() override {
    return base_->GetStackTracePrinter();
  }
  v8::TracingController* GetTracingController() override {
    return base_->GetTracingController();
  }
  void DumpWithoutCrashing() override { base_->DumpWithoutCrashing(); }
  v8::HighAllocationThroughputObserver* GetHighAllocationThroughputObserver()
      override {
    return base_->GetHighAllocationThroughputObserver();
  }

 private:
  std::unique_ptr<v8::JobHandle> CreateJobImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::JobTask> task,
      const v8::SourceLocation& location) override {
    return base_->CreateJob(priority, std::move(task), location);
  }
  void PostTaskOnWorkerThreadImpl(v8::TaskPriority priority,
                                  std::unique_ptr<v8::Task> task,
                                  const v8::SourceLocation& location) override {
    base_->PostTaskOnWorkerThread(priority, std::move(task), location);
  }
  void PostDelayedTaskOnWorkerThreadImpl(
      v8::TaskPriority priority,
      std::unique_ptr<v8::Task> task,
      double delay,
      const v8::SourceLocation& location) override {
    base_->PostDelayedTaskOnWorkerThread(priority, std::move(task), delay,
                                         location);
  }
  v8::Platform* base_;
  std::mutex mutex_;
  std::map<v8::Isolate*, std::shared_ptr<ForegroundState>> states_;
  std::map<std::pair<v8::Isolate*, v8::TaskPriority>,
           std::shared_ptr<ForegroundRunner>>
      runners_;
};

std::unique_ptr<v8::Platform> createForegroundPlatform(v8::Platform* base) {
  return std::make_unique<ForegroundPlatform>(base);
}

void V8PlatformHolder::setForegroundWakeup(v8::Isolate* isolate,
                                           const void* owner,
                                           std::function<void()> callback) {
  auto* platform = static_cast<ForegroundPlatform*>(platform_.get());
  auto state = platform->state(isolate);
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (callback)
      state->callbacks[owner] = std::move(callback);
    else
      state->callbacks.erase(owner);
  }
  if (state->ready)
    state->notify();
}

void V8PlatformHolder::notifyPendingForegroundTasks(v8::Isolate* isolate) {
  auto state =
      static_cast<ForegroundPlatform*>(platform_.get())->state(isolate);
  if (state->ready)
    state->notify();
}

void V8PlatformHolder::closeForegroundTasks(v8::Isolate* isolate) {
  static_cast<ForegroundPlatform*>(platform_.get())->close(isolate);
}
