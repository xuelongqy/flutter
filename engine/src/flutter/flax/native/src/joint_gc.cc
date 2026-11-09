#include "joint_gc.h"
#include <cppgc/allocation.h>
#include <cppgc/heap-state.h>
#include <cppgc/testing.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/trace_event.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/tonic/scopes/dart_isolate_scope.h"

namespace flax {
namespace {
std::mutex heaps_mutex;
std::unordered_map<Dart_Isolate, std::weak_ptr<JointHeap>> heaps;
constexpr auto tag = v8::CppHeapPointerTag::kFirstObjectWrappableTag;
constexpr auto kConfirmationDelay = fml::TimeDelta::FromSeconds(1);
constexpr auto kFallbackGrace = fml::TimeDelta::FromSeconds(1);

bool HasRecentFrame(const flutter::UIDartState& state) {
  const auto* configuration = state.platform_configuration();
  return configuration &&
         fml::TimePoint::Now() - configuration->GetLastFrameTime() <
             fml::TimeDelta::FromMilliseconds(100);
}
}  // namespace

std::shared_ptr<JointHeap> JointHeap::Current(
    const v8rt::IsolateConfig& config) {
  CheckPreparation();
  auto* state = flutter::UIDartState::Current();
  if (!state || !state->IsRootIsolate() ||
      !state->GetTaskRunners().GetUITaskRunner()->RunsTasksOnCurrentThread()) {
    throw std::runtime_error("Flax requires the Flutter UI isolate");
  }
  std::lock_guard<std::mutex> lock(heaps_mutex);
  auto& entry = heaps[Dart_CurrentIsolate()];
  if (auto heap = entry.lock())
    return heap;
  auto heap = std::shared_ptr<JointHeap>(new JointHeap(config));
  entry = heap;
  return heap;
}

void JointHeap::CheckPreparation() {
  std::lock_guard<std::mutex> lock(heaps_mutex);
  auto it = heaps.find(Dart_CurrentIsolate());
  auto heap = it == heaps.end() ? nullptr : it->second.lock();
  if (heap && heap->preparing_)
    throw std::runtime_error("Flax heap is preparing JavaScript");
}

std::shared_ptr<JointHeap> JointHeap::BeginPreparation() {
  std::lock_guard<std::mutex> lock(heaps_mutex);
  auto it = heaps.find(Dart_CurrentIsolate());
  auto heap = it == heaps.end() ? nullptr : it->second.lock();
  if (!heap || heap->closed_ || heap->preparing_ || !heap->cells_.empty())
    return nullptr;
  heap->incremental_start_latched_ = false;
  heap->preparing_ = true;
  return heap;
}

void JointHeap::CancelPreparation() {
  if (preparing_)
    isolate_->TerminateExecution();
}

void JointHeap::EndPreparation() {
  v8::Locker locker(isolate_);
  v8::Isolate::Scope scope(isolate_);
  isolate_->CancelTerminateExecution();
  preparing_ = false;
}

JointHeap::JointHeap(const v8rt::IsolateConfig& config)
    : dart_isolate_(Dart_CurrentIsolate()),
      dart_state_(flutter::UIDartState::Current()->GetWeakPtr()),
      runner_(
          flutter::UIDartState::Current()->GetTaskRunners().GetUITaskRunner()) {
  if (!Dart_FlaxSetCollector(Collect, this)) {
    throw std::runtime_error(
        "Flax does not support multiple UI isolates in one Dart isolate group");
  }
  try {
    isolate_ = v8rt::createIsolate(config);
    if (!isolate_)
      throw std::runtime_error("V8 isolate creation failed");
    v8::Locker locker(isolate_);
    v8::Isolate::Scope scope(isolate_);
    v8::HandleScope handles(isolate_);
    peers_key_.Reset(isolate_, v8::Private::New(isolate_));
    isolate_->AddGCPrologueCallback(
        JsCollectionStarted, this,
        static_cast<v8::GCType>(v8::kGCTypeIncrementalMarking |
                                v8::kGCTypeMarkSweepCompact));
    isolate_->AddGCEpilogueCallback(JsCollected, this);
    Dart_FlaxSetCollectionRequest(RequestFromDart);
  } catch (...) {
    Dart_FlaxSetCollector(nullptr, nullptr);
    throw;
  }
}

void JointHeap::ShutdownCurrent() {
  std::shared_ptr<JointHeap> heap;
  {
    std::lock_guard<std::mutex> lock(heaps_mutex);
    auto it = heaps.find(Dart_CurrentIsolate());
    if (it == heaps.end())
      return;
    heap = it->second.lock();
    heaps.erase(it);
  }
  if (heap)
    heap->Shutdown();
}

void JointHeap::NotifyIdleCurrent() {
  std::shared_ptr<JointHeap> heap;
  {
    std::lock_guard<std::mutex> lock(heaps_mutex);
    auto it = heaps.find(Dart_CurrentIsolate());
    if (it != heaps.end())
      heap = it->second.lock();
  }
  // Flutter may retire the last Dart owner without calling the JS bridge.
  // Check Dart ownership without claiming that the JS heap changed.
  if (heap && !heap->closed_ && !heap->cells_.empty())
    heap->ScheduleIdle(false);
}

int64_t JointHeap::IncrementalJointExperiment(int32_t op) {
  std::shared_ptr<JointHeap> heap;
  {
    std::lock_guard<std::mutex> lock(heaps_mutex);
    const auto found = heaps.find(Dart_CurrentIsolate());
    if (found != heaps.end())
      heap = found->second.lock();
  }
  if (!heap || heap->closed_ || std::this_thread::get_id() != heap->owner_)
    return 0;

  switch (op) {
    case 0:
      // Select the staged comparison before binding any peers.
      if (!heap->one_way_gc_ || heap->preparing_ || heap->collecting_ ||
          heap->joint_epoch_ || !heap->cells_.empty())
        return 0;
      heap->incremental_start_latched_ = false;
      heap->one_way_gc_ = false;
      return 1;
    case 1:
      return heap->one_way_gc_;
    case 2:
      return heap->incremental_starts_.load(std::memory_order_relaxed);
    case 3:
      return heap->major_epilogues_.load(std::memory_order_relaxed);
    case 4:
      return heap->joined_majors_.load(std::memory_order_relaxed);
    case 5:
      return heap->major_followups_.load(std::memory_order_relaxed);
    case 6:
      return heap->rejected_joins_.load(std::memory_order_relaxed);
    case 7:
      return heap->certified_collections_.load(std::memory_order_relaxed);
    case 8:
      return heap->worker_gate_admitted_;
    case 9:
      return heap->gate_rejected_;
    case 10:
      return heap->worker_gate_joined_;
    case 11:
      return heap->worker_gate_total_us_;
    case 12:
      return heap->worker_gate_max_us_;
    case 13:
      return heap->worker_gate_released_;
    case 14:
      if (!heap->one_way_gc_ || heap->collecting_ || heap->joint_epoch_)
        return 0;
      Dart_FlaxCollect(false, false);
      return 1;
    case 15:
      return heap->confirmation_signals_.load(std::memory_order_relaxed);
    case 16:
      return heap->confirmation_posts_.load(std::memory_order_relaxed);
    default:
      return 0;
  }
}

extern "C" __attribute__((visibility("default"))) int64_t
flax_experiment_incremental_joint(int32_t op) {
  return JointHeap::IncrementalJointExperiment(op);
}

extern "C" int32_t flax_engine_defer_idle_gc_current_isolate() {
  std::lock_guard<std::mutex> lock(heaps_mutex);
  const auto found = heaps.find(Dart_CurrentIsolate());
  if (found == heaps.end())
    return false;
  const auto heap = found->second.lock();
  if (!heap || !heap->cell_count())
    return false;
  const bool recent_frame = HasRecentFrame(*flutter::UIDartState::Current());
  if (recent_frame) {
    TRACE_EVENT0("flutter", "FlaxDeferDartIdleGC");
  }
  return recent_frame;
}

void JointHeap::Shutdown() {
  if (closed_.exchange(true))
    return;
  if (std::this_thread::get_id() != owner_ ||
      Dart_CurrentIsolate() != dart_isolate_)
    std::abort();
  incremental_start_latched_ = false;
  if (joint_epoch_ || !Dart_FlaxSetCollector(nullptr, nullptr))
    std::abort();
  v8::Locker locker(isolate_);
  v8::Isolate::Scope scope(isolate_);
  isolate_->RemoveGCPrologueCallback(JsCollectionStarted, this);
  isolate_->RemoveGCEpilogueCallback(JsCollected, this);
  while (!cells_.empty())
    Remove(cells_.begin()->first);
  peers_key_.Reset();
  peer_template_.Reset();
}

JointHeap::~JointHeap() {
  Shutdown();
  {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope scope(isolate_);
    delete v8rt::IsolateData::fromIsolate(isolate_);
  }
  v8rt::V8PlatformHolder::notifyIsolateShutdown(isolate_);
  isolate_->Dispose();
}

JointCell* JointHeap::Bind(const void* session,
                           v8::Local<v8::Object> value,
                           Dart_Handle target,
                           Dart_Handle origin,
                           void (*cleanup)(void*),
                           void* cleanup_data) {
  if (closed_ || std::this_thread::get_id() != owner_)
    throw std::runtime_error(
        "Flax heap is closed or belongs to another thread");
  auto cell = std::make_unique<JointCell>();
  cell->heap = this;
  cell->session = session;
  cell->cleanup = cleanup;
  cell->cleanup_data = cleanup_data;
  auto* raw = cell.get();
  auto weak = [raw](Dart_Handle handle, bool is_origin) {
    if (!handle || Dart_IsNull(handle))
      return Dart_WeakPersistentHandle{};
    auto result =
        Dart_NewWeakPersistentHandle(handle, raw, 0, [](void*, void* peer) {
          static_cast<JointCell*>(peer)->heap->Request(false);
        });
    if (!result || !Dart_FlaxRegisterHandle(result, is_origin)) {
      if (result)
        Dart_DeleteWeakPersistentHandle(result);
      throw std::runtime_error("Dart conditional handle registration failed");
    }
    return result;
  };
  auto ctx = isolate_->GetCurrentContext();
  v8::Local<v8::Value> previous;
  auto key = peers_key_.Get(isolate_);
  if (!value->GetPrivate(ctx, key).ToLocal(&previous))
    std::abort();
  JointPeer* peer;
  if (previous->IsObject()) {
    peer =
        v8::Object::Unwrap<tag, JointPeer>(isolate_, previous.As<v8::Object>());
  } else {
    peer = cppgc::MakeGarbageCollected<JointPeer>(
        isolate_->GetCppHeap()->GetAllocationHandle());
    // Reuse the type template; each Context still gets its own constructor.
    if (peer_template_.IsEmpty())
      peer_template_.Reset(isolate_, v8::FunctionTemplate::New(isolate_));
    auto carrier = peer_template_.Get(isolate_)
                       ->GetFunction(ctx)
                       .ToLocalChecked()
                       ->NewInstance(ctx)
                       .ToLocalChecked();
    v8::Object::Wrap<tag>(isolate_, carrier, peer);
    if (!value->SetPrivate(ctx, key, carrier).FromMaybe(false))
      std::abort();
  }
  if (!peer)
    std::abort();
  // Repeated typed reads of the same wrapper must not add permanent metadata.
  // Facade cells own individual ABI value IDs and are deliberately not deduped.
  if (!cleanup) {
    auto same = [](Dart_WeakPersistentHandle saved, Dart_Handle input) {
      if (!input || Dart_IsNull(input))
        return !saved;
      return saved &&
             Dart_IdentityEquals(Dart_HandleFromWeakPersistent(saved), input);
    };
    for (auto* existing : peer->cells) {
      if (!existing->cleanup && existing->session == session &&
          same(existing->target, target) && same(existing->origin, origin))
        return existing;
    }
  }
  cell->origin = weak(origin, true);
  try {
    cell->target = weak(target, false);
  } catch (...) {
    if (cell->origin)
      Dart_DeleteWeakPersistentHandle(cell->origin);
    throw;
  }
  cell->root = peer;
  cell->peer = peer;
  peer->value.Reset(isolate_, value);
  peer->cells.push_back(raw);
  if (cell->origin)
    origins_.emplace(cell->origin, raw);
  cells_.emplace(raw, std::move(cell));
  ScheduleIdle();
  return raw;
}

v8::Local<v8::Context> JointHeap::OwnedViewContext(const void* session,
                                                   v8::Local<v8::Object> view) {
  if (!session || std::this_thread::get_id() != owner_ || closed_ ||
      Dart_CurrentIsolate() != dart_isolate_ || collecting_ || joint_epoch_ ||
      v8::Isolate::TryGetCurrent() != isolate_ || view.IsEmpty() ||
      view->IsProxy() || view->IsFunction()) {
    return {};
  }
  auto ctx = isolate_->GetCurrentContext();
  v8::Local<v8::Context> creation;
  if (ctx.IsEmpty() || ctx != isolate_->GetEnteredOrMicrotaskContext() ||
      ctx->GetNumberOfEmbedderDataFields() <=
          v8rt::ContextEmbedderIndex::kContextTag ||
      ctx->GetAlignedPointerFromEmbedderData(
          v8rt::ContextEmbedderIndex::kContextTag,
          v8::kEmbedderDataTypeTagDefault) != v8rt::RuntimeContextTagPtr ||
      ctx->GetAlignedPointerFromEmbedderData(
          v8rt::ContextEmbedderIndex::kRuntime,
          v8::kEmbedderDataTypeTagDefault) != session ||
      !view->GetCreationContext(isolate_).ToLocal(&creation) ||
      creation != ctx) {
    return {};
  }
  return ctx;
}

bool JointHeap::BindOwned(const void* session,
                          v8::Local<v8::Object> view,
                          Dart_Handle target) {
  auto ctx = OwnedViewContext(session, view);
  v8::Local<v8::Value> previous;
  if (ctx.IsEmpty() ||
      !view->GetPrivate(ctx, peers_key_.Get(isolate_)).ToLocal(&previous) ||
      !previous->IsUndefined()) {
    return false;
  }
  Bind(session, view, target, nullptr)->owned_target = true;
  return true;
}

bool JointHeap::UnlinkOwned(const void* session, v8::Local<v8::Object> view) {
  auto ctx = OwnedViewContext(session, view);
  v8::Local<v8::Value> carrier;
  if (ctx.IsEmpty() ||
      !view->GetPrivate(ctx, peers_key_.Get(isolate_)).ToLocal(&carrier) ||
      !carrier->IsObject()) {
    return false;
  }
  auto* peer =
      v8::Object::Unwrap<tag, JointPeer>(isolate_, carrier.As<v8::Object>());
  if (!peer || peer->value.Get(isolate_) != view)
    return false;
  for (auto* cell : peer->cells) {
    if (cell->owned_target && cell->session == session && cell->target &&
        !cell->origin && !cell->cleanup && !cell->cleanup_data) {
      Remove(cell);
      return true;
    }
  }
  return false;
}

void JointHeap::ScheduleIdle(bool js_changed) {
  // Pure preparation has no Dart peers and must not mutate UI-owned idle state.
  if (std::this_thread::get_id() != owner_ || closed_ || cells_.empty())
    return;
  if (one_way_gc_) {
    ScheduleConfirmation();
    return;
  }
  dirty_ |= js_changed;
  if (idle_pending_)
    return;
  idle_pending_ = true;
  // Batch newly registered edges. Idle sessions need no caller-managed gc(),
  // and an intervening pressure collection consumes the same dirty batch.
  runner_->PostDelayedTask(
      [weak = weak_from_this()] {
        auto heap = weak.lock();
        if (!heap || heap->closed_)
          return;
        heap->idle_pending_ = false;
        auto state = heap->dart_state_.lock();
        if (!state || state->IsShuttingDown() || heap->cells_.empty())
          return;
        // Pressure requests can still collect between animation frames.
        if (HasRecentFrame(static_cast<const flutter::UIDartState&>(*state))) {
          TRACE_EVENT0("flutter", "FlaxDeferFrameGC");
          heap->ScheduleIdle(false);
          return;
        }
        TRACE_EVENT0("flutter", "FlaxRequestIdleGC");
        heap->Request(true);
      },
      fml::TimeDelta::FromMilliseconds(1000));
}

void JointHeap::Remove(JointCell* cell) {
  if (cell->peer) {
    auto& peers = cell->peer->cells;
    peers.erase(std::remove(peers.begin(), peers.end(), cell), peers.end());
    if (peers.empty())
      cell->peer->value.Reset();
  }
  cell->root.Clear();
  cell->peer.Clear();
  if (cell->origin) {
    origins_.erase(cell->origin);
    Dart_DeleteWeakPersistentHandle(cell->origin);
  }
  if (cell->target)
    Dart_DeleteWeakPersistentHandle(cell->target);
  if (cell->cleanup)
    cell->cleanup(cell->cleanup_data);
  cells_.erase(cell);
  if (cells_.empty() && one_way_gc_) {
    ResetConfirmation();
    collect_requested_ = false;
  }
}

void JointHeap::CloseSession(const void* session) {
  incremental_start_latched_ = false;
  for (auto it = cells_.begin(); it != cells_.end();) {
    auto* cell = (it++)->first;
    if (cell->session == session)
      Remove(cell);
  }
}

void JointHeap::Prune() {
  TRACE_EVENT0("flutter", "FlaxPrune");
  for (auto it = cells_.begin(); it != cells_.end();) {
    auto* cell = (it++)->first;
    if (cell->cpp_dead ||
        (cell->origin && !cell->target &&
         Dart_IsNull(Dart_HandleFromWeakPersistent(cell->origin))))
      Remove(cell);
  }
}

void JointHeap::RestoreRoots() {
  TRACE_EVENT0("flutter", "FlaxRestoreRoots");
  for (auto& [cell, owned] : cells_) {
    if (cell->dart_live) {
      if (!cell->peer)
        std::abort();
      cell->root = cell->peer.Get();
    }
  }
}

void JointHeap::GuardAllPeers() {
  for (auto& [cell, owned] : cells_) {
    if (cell->peer)
      cell->root = cell->peer.Get();
  }
}

bool JointHeap::Checkpoint(
    const std::vector<Dart_WeakPersistentHandle>& targets,
    std::vector<JointPeer*>* origins,
    bool* all_targets_marked) {
  std::vector<Dart_WeakPersistentHandle> reached;
  bool active;
  {
    TRACE_EVENT0("flutter", "FlaxDartFeedback");
    active = Dart_FlaxGCCheckpoint(
        joint_epoch_, targets.data(), targets.size(),
        [](void* peer, Dart_WeakPersistentHandle origin) {
          static_cast<std::vector<Dart_WeakPersistentHandle>*>(peer)->push_back(
              origin);
        },
        &reached, all_targets_marked);
  }
  // The checkpoint has left its Dart safepoint. Only now touch CppGC roots or
  // use the current CppGC visitor; neither collector's visitor is retained.
  if (!active) {
    aborted_ = true;
    dirty_ = true;
    GuardAllPeers();
    return false;
  }
  for (auto origin : reached) {
    auto* cell = origins_.at(origin);
    if (!cell->peer)
      std::abort();
    cell->dart_live = true;
    origins->push_back(cell->peer.Get());
  }
  return true;
}

void JointHeap::Collect(void* peer, Dart_FlaxGCEpoch epoch) {
  auto& heap = *static_cast<JointHeap*>(peer);
  if (heap.closed_ || !heap.isolate_ || heap.cells_.empty())
    return;
  if (std::this_thread::get_id() != heap.owner_ || heap.collecting_ ||
      heap.joint_epoch_)
    std::abort();
  if (heap.one_way_gc_ && !heap.confirming_from_dart_) {
    std::vector<Dart_WeakPersistentHandle> guards;
    guards.reserve(heap.cells_.size());
    for (const auto& [cell, owned] : heap.cells_) {
      if (cell->target)
        guards.push_back(cell->target);
    }
    // JS can still reach these Dart targets. Ordinary Dart tracing from the
    // targets also preserves any origins they reach.
    Dart_FlaxGCCheckpoint(epoch, guards.data(), guards.size(), nullptr, nullptr,
                          nullptr);
    heap.Request(true);
    return;
  }
  heap.external_gc_pending_ = false;
  heap.collecting_ = true;
  heap.aborted_ = false;
  heap.joint_epoch_ = epoch;
  v8::Locker locker(heap.isolate_);
  v8::Isolate::Scope scope(heap.isolate_);
  TRACE_EVENT0("flutter", "FlaxStagedJointGC");
  for (auto& [cell, owned] : heap.cells_)
    cell->dart_live = false;
  const auto& cpp_heap = heap.isolate_->GetCppHeap()->GetHeapHandle();
  const bool in_atomic_pause =
      cppgc::subtle::HeapState::IsInAtomicPause(cpp_heap);
  const bool sweeping_on_owner =
      cppgc::subtle::HeapState::IsSweepingOnOwningThread(cpp_heap);
  if (heap.isolate_->InContext() || in_atomic_pause || sweeping_on_owner) {
    std::vector<Dart_WeakPersistentHandle> guards;
    for (auto& [cell, owned] : heap.cells_) {
      if (cell->origin)
        guards.push_back(cell->origin);
      if (cell->target)
        guards.push_back(cell->target);
    }
    std::vector<JointPeer*> ignored;
    heap.Checkpoint(guards, &ignored);
    heap.GuardAllPeers();
    if (sweeping_on_owner) {
      TRACE_EVENT0("flutter", "FlaxDeferOwnerV8Sweep");
    } else if (in_atomic_pause) {
      TRACE_EVENT0("flutter", "FlaxDeferAtomicV8GC");
    } else {
      TRACE_EVENT0("flutter", "FlaxDeferActiveDartGC");
    }
    heap.Request(true);
  } else {
    bool all_targets_marked = false;
    bool active = true;
    bool roots_ready = false;
    std::vector<JointPeer*> origins;
    // Direct edges can alternate through many ordinary Dart objects. Query
    // all targets only at closure boundaries, rather than once per edge.
    while (active && !roots_ready) {
      const bool checking_closure = origins.empty();
      std::vector<Dart_WeakPersistentHandle> targets;
      for (auto* reachable : origins) {
        for (auto* edge : reachable->cells) {
          if (edge->target)
            targets.push_back(edge->target);
        }
      }
      origins.clear();
      active = heap.Checkpoint(
          targets, &origins, checking_closure ? &all_targets_marked : nullptr);
      roots_ready = checking_closure && origins.empty();
    }
    if (active && (heap.force_js_collection_ || !all_targets_marked)) {
      // Existing marking may contain guard roots. Pure post-weak sweeping is
      // joined by the fresh collection without another guarded major.
      if (cppgc::subtle::HeapState::IsMarking(cpp_heap)) {
        heap.GuardAllPeers();
        heap.joint_epoch_ = 0;
        {
          TRACE_EVENT0("flutter", "FlaxV8ConservativeCompletion");
          heap.isolate_->LowMemoryNotification();
        }
        heap.joint_epoch_ = epoch;
      } else {
        TRACE_EVENT0("flutter", "FlaxV8CompletionSkipped");
      }
      origins.clear();
      active = heap.Checkpoint({}, &origins);
      if (active) {
        for (auto& [cell, owned] : heap.cells_)
          cell->root.Clear();
        heap.RestoreRoots();
        heap.dirty_ = false;
        {
          heap.certified_collections_.fetch_add(1, std::memory_order_relaxed);
          TRACE_EVENT0("flutter", "FlaxV8CertifiedCollection");
          heap.isolate_->ClearKeptObjects();
          heap.isolate_->MemoryPressureNotification(
              v8::MemoryPressureLevel::kCritical);
        }
        if (heap.aborted_)
          heap.GuardAllPeers();
        else
          heap.RestoreRoots();
      }
    } else if (active) {
      for (auto& [cell, owned] : heap.cells_)
        cell->root.Clear();
      heap.RestoreRoots();
      TRACE_EVENT0("flutter", "FlaxDartRootsOnly");
    }
  }
  if (heap.aborted_)
    heap.Request(true);
  else if (heap.confirming_from_dart_)
    heap.GuardAllPeers();  // Resume one-way collection with surviving peers
                           // guarded.
  heap.force_js_collection_ = false;
  heap.joint_epoch_ = 0;
  heap.collecting_ = false;
}

void JointHeap::Trace(const JointPeer* peer, cppgc::Visitor* visitor) {
  if (!joint_epoch_ || aborted_)
    return;
  if (std::this_thread::get_id() != owner_)
    std::abort();
  std::vector<Dart_WeakPersistentHandle> targets;
  for (auto* cell : peer->cells) {
    if (cell->target)
      targets.push_back(cell->target);
  }
  if (targets.empty())
    return;
  std::vector<JointPeer*> origins;
  if (!Checkpoint(targets, &origins)) {
    // An intervening Dart collection retired the token. Feed every surviving
    // peer to this still-valid visitor before unified marking may terminate.
    for (auto& [edge, owned] : cells_) {
      if (edge->peer)
        visitor->Trace(cppgc::Member<JointPeer>(edge->peer.Get()));
    }
    return;
  }
  for (auto* reachable : origins)
    visitor->Trace(cppgc::Member<JointPeer>(reachable));
}

void JointHeap::TryReserveForegroundGC(bool wakeup_pending) {
  if (std::this_thread::get_id() != owner_ || !one_way_gc_ ||
      !confirmation_due_ || (!incremental_start_latched_ && !wakeup_pending) ||
      reserved_isolates_ || preparing_ || closed_ || collecting_ ||
      joint_epoch_ || cells_.empty() ||
      Dart_CurrentIsolate() != dart_isolate_ ||
      v8::Locker::IsLocked(isolate_) ||
      (!collect_requested_ &&
       HasRecentFrame(*flutter::UIDartState::Current()))) {
    return;
  }
  reserved_isolates_ = Dart_FlaxTryReserveEmbedderGC(this);
  if (reserved_isolates_ == 0) {
    ++gate_rejected_;
  } else if (reserved_isolates_ == 2) {
    ++worker_gate_admitted_;
  }
}

void JointHeap::ReleaseForegroundGC() {
  const int32_t isolates = std::exchange(reserved_isolates_, 0);
  if (isolates == 0)
    return;
  const int64_t elapsed = Dart_FlaxReleaseEmbedderGCReservation();
  if (isolates == 2) {
    ++worker_gate_released_;
    worker_gate_total_us_ += elapsed;
    worker_gate_max_us_ = std::max(worker_gate_max_us_, elapsed);
  }
}

void JointHeap::ResetConfirmation() {
  confirmation_due_.reset();
  fallback_due_.reset();
}

void JointHeap::ScheduleConfirmation(bool dart_requested) {
  if (!confirmation_due_)
    confirmation_due_ = fml::TimePoint::Now() + kConfirmationDelay;
  if (dart_requested && !fallback_due_)
    fallback_due_ = fml::TimePoint::Now() + kConfirmationDelay + kFallbackGrace;
  if (confirmation_task_pending_)
    return;
  confirmation_task_pending_ = true;
  runner_->PostTaskForTime(
      [weak = weak_from_this()] {
        auto heap = weak.lock();
        if (!heap || heap->closed_)
          return;
        heap->confirmation_task_pending_ = false;
        if (!heap->confirmation_due_)
          return;
        const auto now = fml::TimePoint::Now();
        if (now >= *heap->confirmation_due_) {
          heap->ConfirmAtDeadline();
          if (!heap->confirmation_due_)
            return;
          if (*heap->confirmation_due_ <= now) {
            heap->confirmation_due_ =
                fml::TimePoint::Now() + fml::TimeDelta::FromMilliseconds(100);
          }
        }
        heap->ScheduleConfirmation();
      },
      *confirmation_due_);
}

void JointHeap::ConfirmAtDeadline() {
  auto state = dart_state_.lock();
  if (!state || state->IsShuttingDown() || cells_.empty()) {
    ResetConfirmation();
    return;
  }
  // Keep pending idle work across frames without frequent retries.
  if (!collect_requested_ &&
      HasRecentFrame(static_cast<const flutter::UIDartState&>(*state))) {
    TRACE_EVENT0("flutter", "FlaxDeferFrameGC");
    confirmation_due_ = fml::TimePoint::Now() + kConfirmationDelay;
    return;
  }
  if (std::this_thread::get_id() != owner_ || preparing_ || collecting_ ||
      joint_epoch_ || reserved_isolates_ || Dart_CurrentIsolate() ||
      v8::Isolate::TryGetCurrent() || v8::Locker::IsLocked(isolate_))
    return;

  const auto now = fml::TimePoint::Now();
  if (!fallback_due_)
    fallback_due_ = now + kFallbackGrace;
  const bool fallback_ready = now >= *fallback_due_;

  tonic::DartState::Scope entered(state);
  TryReserveForegroundGC(true);
  if (!reserved_isolates_ && !fallback_ready)
    return;

  using HeapState = cppgc::subtle::HeapState;
  bool v8_ready;
  {
    v8::Locker locker(isolate_);
    v8::Isolate::Scope scope(isolate_);
    auto& cpp_heap = isolate_->GetCppHeap()->GetHeapHandle();
    v8_ready = !isolate_->InContext() &&
               !HeapState::IsInAtomicPause(cpp_heap) &&
               !HeapState::IsSweepingOnOwningThread(cpp_heap);
    if (v8_ready && reserved_isolates_) {
      if (HeapState::IsMarking(cpp_heap)) {
        TRACE_EVENT0("flutter", "FlaxV8ConservativeCompletion");
        isolate_->LowMemoryNotification();
      }
      if (!HeapState::IsMarking(cpp_heap)) {
        // This UI task starts outside Dart and JS jobs, with no V8 handles.
        cppgc::testing::OverrideEmbedderStackStateScope stack_state(
            cpp_heap, cppgc::EmbedderStackState::kNoHeapPointers);
        isolate_->ClearKeptObjects();
        deadline_major_armed_ = true;
        isolate_->MemoryPressureNotification(
            v8::MemoryPressureLevel::kCritical);
        deadline_major_armed_ = false;
        if (joint_epoch_)
          std::abort();
      }
    }
  }
  ReleaseForegroundGC();
  if (v8_ready && fallback_ready && confirmation_due_)
    ConfirmFromDart();
}

void JointHeap::ConfirmFromDart() {
  TRACE_EVENT0("flutter", "FlaxDartConfirmationFallback");
  // A request during the staged pass belongs to the next batch.
  ResetConfirmation();
  collect_requested_.exchange(false);
  confirming_from_dart_ = true;
  force_js_collection_ = true;
  Dart_FlaxCollect(false, false);
  confirming_from_dart_ = false;
  // Retry if the VM took a path that skipped the owner collector.
  if (std::exchange(force_js_collection_, false))
    Request(true);
  Request(false);
}

void JointHeap::JsCollectionStarted(v8::Isolate* isolate,
                                    v8::GCType type,
                                    v8::GCCallbackFlags flags,
                                    void* peer) {
  auto& heap = *static_cast<JointHeap*>(peer);
  const bool on_owner = std::this_thread::get_id() == heap.owner_;
  if (type == v8::kGCTypeIncrementalMarking) {
    if (on_owner && (heap.collecting_ || heap.joint_epoch_)) {
      heap.incremental_start_latched_ = false;
      return;
    }
    heap.incremental_starts_.fetch_add(1, std::memory_order_relaxed);
    TRACE_EVENT0("flutter", "FlaxIncrementalV8Start");
    if (on_owner) {
      heap.incremental_start_latched_ =
          heap.one_way_gc_ && !heap.preparing_ && heap.confirmation_due_ &&
          !heap.closed_ && Dart_CurrentIsolate() == heap.dart_isolate_;
    }
    return;
  }

  // Each early start is eligible only for the next actual major.
  if (!on_owner)
    return;
  const bool deadline = std::exchange(heap.deadline_major_armed_, false);
  const bool early_start =
      std::exchange(heap.incremental_start_latched_, false);
  if ((!deadline && !early_start) || !heap.confirmation_due_ ||
      heap.preparing_ || heap.closed_ || heap.collecting_ ||
      heap.joint_epoch_ || heap.cells_.empty() ||
      Dart_CurrentIsolate() != heap.dart_isolate_) {
    if (deadline)
      heap.ReleaseForegroundGC();
    return;
  }

  using HeapState = cppgc::subtle::HeapState;
  const auto& cpp_heap = isolate->GetCppHeap()->GetHeapHandle();
  if (deadline) {
    if (isolate->InContext() || HeapState::IsMarking(cpp_heap) ||
        HeapState::IsInAtomicPause(cpp_heap) ||
        HeapState::IsSweepingOnOwningThread(cpp_heap) ||
        !(flags & v8::kGCCallbackFlagCollectAllAvailableGarbage)) {
      heap.ReleaseForegroundGC();
      return;
    }
    TRACE_EVENT0("flutter", "FlaxDeadlineJointBegin");
  } else {
    if (!InOutermostForegroundPump(isolate) ||
        (flags & (v8::kGCCallbackFlagForced | v8::kGCCallbackFlagLastResort |
                  v8::kGCCallbackFlagCollectAllAvailableGarbage)) ||
        !HeapState::IsMarking(cpp_heap) ||
        HeapState::IsInAtomicPause(cpp_heap) || HeapState::IsSweeping(cpp_heap))
      return;
    TRACE_EVENT0("flutter", "FlaxIncrementalJointBegin");
  }

  if (heap.reserved_isolates_) {
    heap.joint_epoch_ = Dart_FlaxTryBeginEmbedderGC(&heap);
  }
  if (!heap.joint_epoch_) {
    if (!deadline)
      heap.rejected_joins_.fetch_add(1, std::memory_order_relaxed);
    heap.ReleaseForegroundGC();
    return;
  }
  heap.aborted_ = false;
  heap.joined_dart_request_ = heap.collect_requested_.exchange(false);
  for (auto& [cell, owned] : heap.cells_)
    cell->dart_live = false;

  std::vector<JointPeer*> origins;
  if (heap.Checkpoint({}, &origins)) {
    // Atomic CppGC has not visited its strong persistent roots yet.
    for (auto& [cell, owned] : heap.cells_)
      cell->root.Clear();
    heap.RestoreRoots();
  }
}

void JointHeap::JsCollected(v8::Isolate* isolate,
                            v8::GCType type,
                            v8::GCCallbackFlags flags,
                            void* peer) {
  auto& heap = *static_cast<JointHeap*>(peer);
  const bool major = type & v8::kGCTypeMarkSweepCompact;
  if (major) {
    heap.major_epilogues_.fetch_add(1, std::memory_order_relaxed);
    if (std::this_thread::get_id() == heap.owner_) {
      heap.incremental_start_latched_ = false;
      if (!heap.collecting_ && heap.joint_epoch_) {
        const bool natural = InOutermostForegroundPump(isolate);
        const auto epoch = std::exchange(heap.joint_epoch_, 0);
        const bool committed = Dart_FlaxFinishEmbedderGC(epoch, !heap.aborted_);
        // The Worker gate must keep the epoch valid through both weak
        // decisions.
        if (!committed && !heap.aborted_)
          std::abort();
        heap.GuardAllPeers();
        const bool dart_requested =
            std::exchange(heap.joined_dart_request_, false);
        if (committed) {
          heap.ResetConfirmation();
          if (natural) {
            TRACE_EVENT0("flutter", "FlaxIncrementalJointCommit");
            heap.joined_majors_.fetch_add(1, std::memory_order_relaxed);
            if (heap.reserved_isolates_ == 2)
              ++heap.worker_gate_joined_;
          } else {
            TRACE_EVENT0("flutter", "FlaxDeadlineJointCommit");
            heap.certified_collections_.fetch_add(1, std::memory_order_relaxed);
          }
        } else if (natural) {
          heap.rejected_joins_.fetch_add(1, std::memory_order_relaxed);
        }
        heap.ReleaseForegroundGC();
        if (committed) {
          heap.Request(false);  // Prune without consuming queued Dart work.
        } else if (dart_requested) {
          heap.Request(true);
        }
      }
    }
  }
  if (heap.collecting_) {
    if (heap.joint_epoch_ && !heap.aborted_)
      heap.RestoreRoots();
    return;
  }
  if (heap.one_way_gc_)
    return;
  // Minor/incremental/weak-callback phases do not complete JS heap tracing.
  // Escalating each phase to a full Dart/V8 collection creates a GC feedback
  // loop.
  if (major && !heap.cells_.empty()) {
    heap.major_followups_.fetch_add(1, std::memory_order_relaxed);
    TRACE_EVENT0("flutter", "FlaxRequestV8GC");
    const bool external = flags & v8::kGCCallbackFlagCollectAllExternalMemory;
    const bool urgent =
        flags & (v8::kGCCallbackFlagForced | v8::kGCCallbackFlagLastResort |
                 v8::kGCCallbackFlagCollectAllAvailableGarbage);
    if (!external || urgent) {
      heap.external_gc_pending_ = false;
      heap.Request(true);
    } else if (!heap.external_gc_pending_) {
      // Transfers repeatedly charge existing BackingStores as external memory.
      // Merge their Dart follow-ups for 8 ms; V8 GC and Dart pressure stay
      // immediate.
      heap.external_gc_pending_ = true;
      heap.runner_->PostDelayedTask(
          [weak = heap.weak_from_this()] {
            auto heap = weak.lock();
            if (!heap || !heap->external_gc_pending_)
              return;
            heap->external_gc_pending_ = false;
            if (!heap->closed_ && !heap->cells_.empty())
              heap->Request(true);
          },
          fml::TimeDelta::FromMilliseconds(8));
    }
  } else {
    TRACE_EVENT0("flutter", "FlaxV8IntermediateGC");
  }
}

void JointHeap::RequestFromDart(void* peer) {
  TRACE_EVENT0("flutter", "FlaxRequestBackgroundDartGC");
  static_cast<JointHeap*>(peer)->Request(true);
}

void JointHeap::Request(bool collect) {
  if (closed_)
    return;
  if (collect) {
    confirmation_signals_.fetch_add(1, std::memory_order_relaxed);
    if (collect_requested_.exchange(true))
      return;
  }
  if (queued_.exchange(true))
    return;
  if (collect)
    confirmation_posts_.fetch_add(1, std::memory_order_relaxed);
  // Always post: a GC callback must never reenter Dart or execute application
  // code.
  runner_->PostTask([weak = weak_from_this()] {
    auto heap = weak.lock();
    if (!heap || heap->closed_)
      return;
    auto state = heap->dart_state_.lock();
    if (!state || state->IsShuttingDown())
      return;
    heap->queued_ = false;
    tonic::DartIsolateScope entered(state->isolate());
    Dart_EnterScope();
    if (heap->one_way_gc_) {
      if (heap->cells_.empty()) {
        heap->collect_requested_ = false;
      } else if (heap->collect_requested_) {
        heap->ScheduleConfirmation(/*dart_requested=*/true);
      }
    } else if (heap->collect_requested_.exchange(false)) {
      TRACE_EVENT0("flutter", "FlaxCollect");
      // JS changes need an idle collection even when all Dart targets survive.
      // Unmarked Dart targets always trigger joint tracing independently.
      heap->force_js_collection_ = heap->dirty_;
      Dart_FlaxCollect(false, false);
      heap->force_js_collection_ = false;
    }
    heap->Prune();
    Dart_ExitScope();
  });
}

void JointPeer::Detach() {
  // Weak persistents clear before deferred sweeping. Detach while metadata is
  // still alive, so later cell removal cannot leave dangling destructor inputs.
  for (auto* cell : cells) {
    cell->cpp_dead = true;
    cell->heap->Request(false);
  }
  cells.clear();
}
void JointPeer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(value);
  if (!cells.empty())
    cells.front()->heap->Trace(this, visitor);
}
}  // namespace flax
