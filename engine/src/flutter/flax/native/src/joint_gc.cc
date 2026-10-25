#include "joint_gc.h"
#include <cppgc/allocation.h>
#include <stdexcept>
#include <algorithm>
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/trace_event.h"
#include "third_party/tonic/scopes/dart_isolate_scope.h"

namespace flax {
namespace {
std::mutex heaps_mutex;
std::unordered_map<Dart_Isolate, std::weak_ptr<JointHeap>> heaps;
constexpr auto tag = v8::CppHeapPointerTag::kFirstObjectWrappableTag;
}

std::shared_ptr<JointHeap> JointHeap::Current(const v8rt::IsolateConfig& config) {
  auto* state = flutter::UIDartState::Current();
  if (!state || !state->IsRootIsolate() ||
      !state->GetTaskRunners().GetUITaskRunner()->RunsTasksOnCurrentThread()) {
    throw std::runtime_error("Flax requires the Flutter UI isolate");
  }
  std::lock_guard<std::mutex> lock(heaps_mutex);
  auto& entry = heaps[Dart_CurrentIsolate()];
  if (auto heap = entry.lock()) return heap;
  auto heap = std::shared_ptr<JointHeap>(new JointHeap(config));
  entry = heap;
  return heap;
}

JointHeap::JointHeap(const v8rt::IsolateConfig& config)
    : dart_isolate_(Dart_CurrentIsolate()),
      dart_state_(flutter::UIDartState::Current()->GetWeakPtr()),
      runner_(flutter::UIDartState::Current()->GetTaskRunners().GetUITaskRunner()) {
  if (!Dart_FlaxSetCollector(Collect, this)) {
    throw std::runtime_error("Flax does not support multiple UI isolates in one Dart isolate group");
  }
  try {
    isolate_ = v8rt::createIsolate(config);
    if (!isolate_) throw std::runtime_error("V8 isolate creation failed");
    v8::Locker locker(isolate_);
    v8::Isolate::Scope scope(isolate_);
    v8::HandleScope handles(isolate_);
    peers_key_.Reset(isolate_, v8::Private::New(isolate_));
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
    if (it == heaps.end()) return;
    heap = it->second.lock();
    heaps.erase(it);
  }
  if (heap) heap->Shutdown();
}

void JointHeap::NotifyIdleCurrent() {
  std::shared_ptr<JointHeap> heap;
  {
    std::lock_guard<std::mutex> lock(heaps_mutex);
    auto it = heaps.find(Dart_CurrentIsolate());
    if (it != heaps.end()) heap = it->second.lock();
  }
  // Flutter may retire the last Dart owner without calling the JS bridge.
  // Check Dart ownership without claiming that the JS heap changed.
  if (heap && !heap->closed_ && !heap->cells_.empty()) heap->ScheduleIdle(false);
}

void JointHeap::Shutdown() {
  if (closed_.exchange(true)) return;
  if (std::this_thread::get_id() != owner_ || Dart_CurrentIsolate() != dart_isolate_)
    std::abort();
  Dart_FlaxSetCollector(nullptr, nullptr);
  v8::Locker locker(isolate_);
  v8::Isolate::Scope scope(isolate_);
  isolate_->RemoveGCEpilogueCallback(JsCollected, this);
  while (!cells_.empty()) Remove(cells_.begin()->first);
  peers_key_.Reset();
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

JointCell* JointHeap::Bind(const void* session, v8::Local<v8::Object> value,
                     Dart_Handle target, Dart_Handle origin,
                     void (*cleanup)(void*), void* cleanup_data) {
  if (closed_ || std::this_thread::get_id() != owner_)
    throw std::runtime_error("Flax heap is closed or belongs to another thread");
  auto cell = std::make_unique<JointCell>();
  cell->heap = this;
  cell->session = session;
  cell->cleanup = cleanup;
  cell->cleanup_data = cleanup_data;
  auto* raw = cell.get();
  auto weak = [raw](Dart_Handle handle, bool is_origin) {
    if (!handle || Dart_IsNull(handle)) return Dart_WeakPersistentHandle{};
    auto result = Dart_NewWeakPersistentHandle(handle, raw, 0,
        [](void*, void* peer) {
          static_cast<JointCell*>(peer)->heap->Request(false);
        });
    if (!result || !Dart_FlaxRegisterHandle(result, is_origin)) {
      if (result) Dart_DeleteWeakPersistentHandle(result);
      throw std::runtime_error("Dart conditional handle registration failed");
    }
    return result;
  };
  auto ctx = isolate_->GetCurrentContext();
  v8::Local<v8::Value> previous;
  auto key = peers_key_.Get(isolate_);
  if (!value->GetPrivate(ctx, key).ToLocal(&previous)) std::abort();
  JointPeer* peer;
  if (previous->IsObject()) {
    peer = v8::Object::Unwrap<tag, JointPeer>(isolate_, previous.As<v8::Object>());
  } else {
    peer = cppgc::MakeGarbageCollected<JointPeer>(isolate_->GetCppHeap()->GetAllocationHandle());
    auto carrier = v8::FunctionTemplate::New(isolate_)->GetFunction(ctx)
                       .ToLocalChecked()->NewInstance(ctx).ToLocalChecked();
    v8::Object::Wrap<tag>(isolate_, carrier, peer);
    if (!value->SetPrivate(ctx, key, carrier).FromMaybe(false)) std::abort();
  }
  if (!peer) std::abort();
  // Repeated typed reads of the same wrapper must not add permanent metadata.
  // Facade cells own individual ABI value IDs and are deliberately not deduped.
  if (!cleanup) {
    auto same = [](Dart_WeakPersistentHandle saved, Dart_Handle input) {
      if (!input || Dart_IsNull(input)) return !saved;
      return saved && Dart_IdentityEquals(Dart_HandleFromWeakPersistent(saved), input);
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
    if (cell->origin) Dart_DeleteWeakPersistentHandle(cell->origin);
    throw;
  }
  cell->root = peer;
  cell->peer = peer;
  peer->value.Reset(isolate_, value);
  peer->cells.push_back(raw);
  if (cell->origin) origins_.emplace(cell->origin, raw);
  cells_.emplace(raw, std::move(cell));
  ScheduleIdle();
  return raw;
}

void JointHeap::ScheduleIdle(bool js_changed) {
  dirty_ |= js_changed;
  if (idle_pending_) return;
  idle_pending_ = true;
  // Batch newly registered edges. Idle sessions need no caller-managed gc(),
  // and an intervening pressure collection consumes the same dirty batch.
  runner_->PostDelayedTask([weak = weak_from_this()] {
    auto heap = weak.lock();
    if (!heap || heap->closed_) return;
    heap->idle_pending_ = false;
    auto state = heap->dart_state_.lock();
    if (!state || state->IsShuttingDown() || heap->cells_.empty()) return;
    auto* configuration = static_cast<flutter::UIDartState*>(state.get())
                              ->platform_configuration();
    // Match Hermes: a timer between animation frames is not a GC idle window.
    // Pressure collections still request joint tracing without this deferral.
    if (configuration &&
        fml::TimePoint::Now() - configuration->GetLastFrameTime() <
            fml::TimeDelta::FromMilliseconds(100)) {
      TRACE_EVENT0("flutter", "FlaxDeferFrameGC");
      heap->ScheduleIdle(false);
      return;
    }
    TRACE_EVENT0("flutter", "FlaxRequestIdleGC");
    heap->Request(true);
  }, fml::TimeDelta::FromMilliseconds(1000));
}

void JointHeap::Remove(JointCell* cell) {
  if (cell->peer) {
    auto& peers = cell->peer->cells;
    peers.erase(std::remove(peers.begin(), peers.end(), cell), peers.end());
    if (peers.empty()) cell->peer->value.Reset();
  }
  cell->root.Clear();
  cell->peer.Clear();
  if (cell->origin) {
    origins_.erase(cell->origin);
    Dart_DeleteWeakPersistentHandle(cell->origin);
  }
  if (cell->target) Dart_DeleteWeakPersistentHandle(cell->target);
  if (cell->cleanup) cell->cleanup(cell->cleanup_data);
  cells_.erase(cell);
}

void JointHeap::CloseSession(const void* session) {
  for (auto it = cells_.begin(); it != cells_.end();) {
    auto* cell = (it++)->first;
    if (cell->session == session) Remove(cell);
  }
}

void JointHeap::Prune() {
  TRACE_EVENT0("flutter", "FlaxPrune");
  for (auto it = cells_.begin(); it != cells_.end();) {
    auto* cell = (it++)->first;
    if (cell->cpp_dead ||
        (cell->origin && !cell->target &&
         Dart_IsNull(Dart_HandleFromWeakPersistent(cell->origin)))) Remove(cell);
  }
}

void JointHeap::RestoreRoots() {
  TRACE_EVENT0("flutter", "FlaxRestoreRoots");
  for (auto& [cell, owned] : cells_) {
    if (cell->origin && joint_->is_marked(joint_->context, cell->origin)) {
      if (!cell->peer) std::abort();
      cell->root = cell->peer.Get();
    }
  }
}

void JointHeap::Collect(void* peer, const Dart_FlaxGC* gc) {
  auto& heap = *static_cast<JointHeap*>(peer);
  if (heap.closed_ || !heap.isolate_) return;
  if (std::this_thread::get_id() != heap.owner_ || heap.joint_) std::abort();
  v8::Locker locker(heap.isolate_);
  v8::Isolate::Scope scope(heap.isolate_);
  // A Dart allocation may collect while a JS-to-Dart call is active. Preserve
  // that JS job (including WeakRef kept objects), then collect after it returns.
  if (heap.isolate_->InContext()) {
    for (auto& [cell, owned] : heap.cells_) {
      if (cell->origin) gc->mark(gc->context, cell->origin);
      if (cell->target) gc->mark(gc->context, cell->target);
    }
    TRACE_EVENT0("flutter", "FlaxDeferActiveDartGC");
    heap.Request(true);
    return;
  }
  heap.joint_ = gc;
  TRACE_EVENT0("flutter", "FlaxJointGC");
  {
    TRACE_EVENT0("flutter", "FlaxRootPreparation");
    for (auto& [cell, owned] : heap.cells_) cell->root.Clear();
    while (auto origin = gc->next_marked_origin(gc->context)) {
      auto* cell = heap.origins_.at(origin);
      if (!cell->peer) std::abort();
      auto* reachable = cell->peer.Get();
      cell->root = reachable;
      // A Dart root reaches this exact JS value, so its direct Dart targets
      // are reachable without traversing the rest of the JavaScript heap.
      for (auto* edge : reachable->cells) {
        if (edge->target && !gc->is_marked(gc->context, edge->target))
          gc->mark(gc->context, edge->target);
      }
    }
  }
  if (!heap.force_js_collection_ &&
      std::all_of(heap.cells_.begin(), heap.cells_.end(), [&](const auto& entry) {
        auto* cell = entry.first;
        return !cell->target || gc->is_marked(gc->context, cell->target);
      })) {
    TRACE_EVENT0("flutter", "FlaxDartRootsOnly");
    heap.joint_ = nullptr;
    return;
  }
  heap.dirty_ = false;
  {
    TRACE_EVENT0("flutter", "FlaxV8Collection");
    heap.isolate_->ClearKeptObjects();
    // Critical pressure completes a major GC on this locked owner thread.
    // LowMemoryNotification always repeats full GC, even with stable roots.
    heap.isolate_->MemoryPressureNotification(v8::MemoryPressureLevel::kCritical);
    heap.RestoreRoots();
  }
  heap.force_js_collection_ = false;
  heap.joint_ = nullptr;
}

void JointHeap::Trace(JointCell* cell, cppgc::Visitor* visitor) {
  if (!joint_) return;
  if (std::this_thread::get_id() != owner_) std::abort();
  if (cell->target) joint_->mark(joint_->context, cell->target);
  while (auto origin = joint_->next_marked_origin(joint_->context)) {
    auto* reachable = origins_.at(origin);
    if (!reachable->peer) std::abort();
    visitor->Trace(cppgc::Member<JointPeer>(reachable->peer.Get()));
  }
}

void JointHeap::JsCollected(v8::Isolate*, v8::GCType type, v8::GCCallbackFlags, void* peer) {
  auto& heap = *static_cast<JointHeap*>(peer);
  if (heap.joint_) heap.RestoreRoots();
  // Minor/incremental/weak-callback phases do not complete JS heap tracing.
  // Escalating each phase to a full Dart/V8 collection creates a GC feedback loop.
  else if ((type & v8::kGCTypeMarkSweepCompact) && !heap.cells_.empty()) {
    TRACE_EVENT0("flutter", "FlaxRequestV8GC");
    heap.Request(true);
  } else if (!heap.joint_) {
    TRACE_EVENT0("flutter", "FlaxV8IntermediateGC");
  }
}

void JointHeap::RequestFromDart(void* peer) {
  TRACE_EVENT0("flutter", "FlaxRequestBackgroundDartGC");
  static_cast<JointHeap*>(peer)->Request(true);
}

void JointHeap::Request(bool collect) {
  if (closed_) return;
  if (collect) collect_requested_ = true;
  if (queued_.exchange(true)) return;
  // Always post: a GC callback must never reenter Dart or execute application code.
  runner_->PostTask([weak = weak_from_this()] {
    auto heap = weak.lock();
    if (!heap || heap->closed_) return;
    auto state = heap->dart_state_.lock();
    if (!state || state->IsShuttingDown()) return;
    heap->queued_ = false;
    tonic::DartIsolateScope entered(state->isolate());
    Dart_EnterScope();
    if (heap->collect_requested_.exchange(false)) {
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
  for (auto* cell : cells) cell->heap->Trace(cell, visitor);
}
}  // namespace flax
