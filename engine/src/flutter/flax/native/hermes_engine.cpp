#include <dart_flax_api.h>
#include <hermes/hermes.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "engine.h"
#include "flutter/fml/time/time_delta.h"
#include "flutter/fml/trace_event.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "third_party/tonic/scopes/dart_isolate_scope.h"

namespace js = facebook::jsi;
namespace hgc = facebook::hermes;
namespace flax {
class HermesHeap;
struct JointCell {
  HermesHeap* heap;
  hgc::HermesRuntime* session;
  uint64_t object_id;
  Dart_WeakPersistentHandle origin = nullptr, target = nullptr;
  js::Value root;
  js::WeakObject value;
  void (*cleanup)(void*) = nullptr;
  void* data = nullptr;
  JointCell(HermesHeap* heap,
            hgc::HermesRuntime& runtime,
            const js::Object& object)
      : heap(heap),
        session(&runtime),
        object_id(runtime.getUniqueID(object)),
        value(runtime, object) {
    // A separate JSI handle must not weaken another caller's temporary root.
    root = value.lock(runtime);
    hgc::makeFlaxConditional(runtime, root.getObject(runtime));
  }
};
namespace {
std::mutex heaps_mutex;
std::unordered_map<Dart_Isolate, std::weak_ptr<HermesHeap>> heaps;
thread_local std::unordered_map<js::Runtime*, std::shared_ptr<HermesHeap>>
    sessions;
}  // namespace

// Sessions own separate Hermes VMs; one UI-isolate coordinator traces all of
// them.
class HermesHeap : public std::enable_shared_from_this<HermesHeap> {
 public:
  static std::shared_ptr<HermesHeap> Current() {
    auto* state = flutter::UIDartState::Current();
    if (!state || !state->IsRootIsolate() ||
        !state->GetTaskRunners().GetUITaskRunner()->RunsTasksOnCurrentThread())
      throw std::runtime_error("Flax requires the Flutter UI isolate");
    std::lock_guard lock(heaps_mutex);
    auto& saved = heaps[Dart_CurrentIsolate()];
    if (auto heap = saved.lock())
      return heap;
    auto heap = std::shared_ptr<HermesHeap>(new HermesHeap());
    saved = heap;
    return heap;
  }
  HermesHeap()
      : isolate_(Dart_CurrentIsolate()),
        state_(flutter::UIDartState::Current()->GetWeakPtr()),
        runner_(flutter::UIDartState::Current()
                    ->GetTaskRunners()
                    .GetUITaskRunner()) {
    if (!Dart_FlaxSetCollector(Collect, this))
      throw std::runtime_error(
          "Flax does not support multiple UI isolates in one Dart isolate "
          "group");
    Dart_FlaxSetCollectionRequest(
        [](void* p) {
          TRACE_EVENT0("flutter", "FlaxHermesRequestDartGC");
          static_cast<HermesHeap*>(p)->Request(true);
        });
  }
  ~HermesHeap() { Shutdown(); }

  void Add(hgc::HermesRuntime& runtime, size_t* active) {
    runtimes_.emplace(&runtime, Session{active, nullptr});
    hgc::setFlaxGC(runtime, &runtime, [](void* p, const hgc::FlaxGC& gc) {
      auto* runtime = static_cast<hgc::HermesRuntime*>(p);
      sessions.at(runtime)->Trace(*runtime, gc);
    });
  }
  void Close(hgc::HermesRuntime& runtime) {
    for (auto it = cells_.begin(); it != cells_.end();) {
      auto* cell = (it++)->first;
      if (cell->session == &runtime)
        Remove(cell);
    }
    hgc::setFlaxGC(runtime, nullptr, nullptr);
    runtimes_.erase(&runtime);
    peers_.erase(&runtime);
  }
  JointCell* Bind(hgc::HermesRuntime& runtime,
                  const js::Object& object,
                  Dart_Handle target,
                  Dart_Handle origin,
                  bool weaken,
                  void (*cleanup)(void*),
                  void* data) {
    if (closed_ || isolate_ != Dart_CurrentIsolate())
      throw std::runtime_error(
          "Flax heap is closed or belongs to another isolate");
    const auto id = runtime.getUniqueID(object);
    auto same = [](Dart_WeakPersistentHandle saved, Dart_Handle input) {
      return (!input || Dart_IsNull(input))
                 ? !saved
                 : saved && Dart_IdentityEquals(
                                Dart_HandleFromWeakPersistent(saved), input);
    };
    if (!cleanup) {
      for (auto* cell : peers_[&runtime][id])
        if (!cell->cleanup && same(cell->target, target) &&
            same(cell->origin, origin))
          return cell;
    }
    auto owned = std::make_unique<JointCell>(this, runtime, object);
    auto* cell = owned.get();
    auto weak = [cell](Dart_Handle handle, bool origin) {
      if (!handle || Dart_IsNull(handle))
        return Dart_WeakPersistentHandle{};
      auto result =
          Dart_NewWeakPersistentHandle(handle, cell, 0, [](void*, void* p) {
            static_cast<JointCell*>(p)->heap->Request(false);
          });
      if (!result || !Dart_FlaxRegisterHandle(result, origin)) {
        if (result)
          Dart_DeleteWeakPersistentHandle(result);
        throw std::runtime_error("Dart conditional handle registration failed");
      }
      return result;
    };
    cell->origin = weak(origin, true);
    try {
      cell->target = weak(target, false);
    } catch (...) {
      if (cell->origin)
        Dart_DeleteWeakPersistentHandle(cell->origin);
      throw;
    }
    cell->cleanup = cleanup;
    cell->data = data;
    peers_[&runtime][id].insert(cell);
    cells_.emplace(cell, std::move(owned));
    cells_empty_ = false;
    if (weaken)
      hgc::makeFlaxConditional(runtime, object);
    ScheduleIdle();
    return cell;
  }
  void Remove(JointCell* cell) {
    // Remove the stable object index while its runtime still exists.
    auto& by_id = peers_[cell->session];
    auto it = by_id.find(cell->object_id);
    if (it != by_id.end()) {
      it->second.erase(cell);
      if (it->second.empty())
        by_id.erase(it);
    }
    if (cell->origin)
      Dart_DeleteWeakPersistentHandle(cell->origin);
    if (cell->target)
      Dart_DeleteWeakPersistentHandle(cell->target);
    if (cell->cleanup)
      cell->cleanup(cell->data);
    cells_.erase(cell);
    cells_empty_ = cells_.empty();
  }
  void Prune() {
    TRACE_EVENT0("flutter", "FlaxHermesPrune");
    for (auto it = cells_.begin(); it != cells_.end();) {
      auto* cell = (it++)->first;
      if (cell->value.lock(*cell->session).isUndefined() ||
          (cell->origin && !cell->target &&
           Dart_IsNull(Dart_HandleFromWeakPersistent(cell->origin))))
        Remove(cell);
    }
  }
  void Shutdown() {
    if (closed_.exchange(true))
      return;
    if (Dart_CurrentIsolate() != isolate_)
      std::abort();
    Dart_FlaxSetCollector(nullptr, nullptr);
    while (!cells_.empty())
      Remove(cells_.begin()->first);
  }
  size_t count() const { return cells_.size(); }
  void ScheduleIdle() {
    if (idle_pending_ || closed_)
      return;
    idle_pending_ = true;
    runner_->PostDelayedTask(
        [weak = weak_from_this()] {
          if (auto heap = weak.lock(); heap && !heap->closed_) {
            heap->idle_pending_ = false;
            auto state = heap->state_.lock();
            if (!state || state->IsShuttingDown() || heap->cells_.empty())
              return;
            auto* configuration =
                static_cast<flutter::UIDartState*>(state.get())
                    ->platform_configuration();
            // A timer firing does not make the interval between animation
            // frames idle. Pressure collections still bypass this deferral.
            if (configuration &&
                fml::TimePoint::Now() - configuration->GetLastFrameTime() <
                    fml::TimeDelta::FromMilliseconds(100)) {
              TRACE_EVENT0("flutter", "FlaxHermesDeferFrameGC");
              heap->ScheduleIdle();
              return;
            }
            TRACE_EVENT0("flutter", "FlaxHermesRequestIdleGC");
            heap->Request(true);
          }
        },
        fml::TimeDelta::FromSeconds(1));
  }
  void Request(bool collect) {
    if (closed_)
      return;
    if (collect)
      collect_requested_ = true;
    if (queued_.exchange(true))
      return;
    runner_->PostTask([weak = weak_from_this()] {
      auto heap = weak.lock();
      if (!heap || heap->closed_)
        return;
      auto state = heap->state_.lock();
      if (!state || state->IsShuttingDown())
        return;
      heap->queued_ = false;
      tonic::DartIsolateScope entered(state->isolate());
      Dart_EnterScope();
      if (heap->collect_requested_.exchange(false)) {
        TRACE_EVENT0("flutter", "FlaxHermesCollect");
        Dart_FlaxCollect(false, false);
      }
      heap->Prune();
      Dart_ExitScope();
    });
  }
  void JsCollected(const ::hermes::vm::GCAnalyticsEvent& event) {
    // A young collection is not a completed tracing of the JS heap. Old-gen
    // statistics can arrive after the joint collection that produced them.
    if (event.collectionType == "old" && event.cause != "Flax joint GC" &&
        !in_joint_ && !cells_empty_) {
      TRACE_EVENT0("flutter", "FlaxHermesRequestOldGC");
      Request(true);
    }
  }

 private:
  struct Session {
    size_t* active;
    const hgc::FlaxGC* visitor;
  };
  void Trace(hgc::HermesRuntime& runtime, const hgc::FlaxGC& js) {
    if (!joint_)
      return;
    if (!js.complete) {
      for (auto& [cell, _] : cells_)
        if (cell->session == &runtime && cell->origin &&
            joint_->is_marked(joint_->context, cell->origin))
          js.mark(js.context, cell->value);
      return;
    }
    auto& session = runtimes_.at(&runtime);
    session.visitor = &js;
    // Suspend the outer mark phase until every session has reached this phase.
    for (auto& [other, _] : runtimes_)
      if (!started_.contains(other)) {
        started_.insert(other);
        hgc::collectFlaxGC(*other);
      }
    bool changed;
    do {
      changed = false;
      for (auto& [cell, _] : cells_) {
        auto* visitor = runtimes_.at(cell->session).visitor;
        if (!visitor)
          continue;
        if (cell->origin && joint_->is_marked(joint_->context, cell->origin) &&
            !visitor->isMarked(visitor->context, cell->value)) {
          visitor->mark(visitor->context, cell->value);
          if (!visitor->isMarked(visitor->context, cell->value))
            std::abort();
          changed = true;
        }
        if (cell->target && visitor->isMarked(visitor->context, cell->value) &&
            !joint_->is_marked(joint_->context, cell->target)) {
          joint_->mark(joint_->context, cell->target);
          changed = true;
        }
      }
      for (auto& [_, state] : runtimes_)
        if (state.visitor)
          state.visitor->drain(state.visitor->context);
    } while (changed);
    session.visitor = nullptr;
  }
  static void Collect(void* p, const Dart_FlaxGC* gc) {
    auto& heap = *static_cast<HermesHeap*>(p);
    if (heap.closed_ || heap.cells_.empty() || heap.runtimes_.empty())
      return;
    if (heap.joint_)
      std::abort();
    for (auto& [_, session] : heap.runtimes_)
      if (*session.active) {
        TRACE_EVENT0("flutter", "FlaxHermesDeferActiveGC");
        for (auto& [cell, _] : heap.cells_) {
          if (cell->origin)
            gc->mark(gc->context, cell->origin);
          if (cell->target)
            gc->mark(gc->context, cell->target);
        }
        heap.Request(true);
        return;
      }
    TRACE_EVENT0("flutter", "FlaxHermesJointGC");
    heap.in_joint_ = true;
    for (auto& [runtime, _] : heap.runtimes_)
      hgc::prepareFlaxGC(*runtime);
    heap.joint_ = gc;
    auto* first = heap.runtimes_.begin()->first;
    heap.started_.insert(first);
    hgc::collectFlaxGC(*first);
    heap.started_.clear();
    heap.joint_ = nullptr;
    heap.in_joint_ = false;
    heap.cells_empty_ = heap.cells_.empty();
  }
  Dart_Isolate isolate_;
  std::weak_ptr<tonic::DartState> state_;
  fml::RefPtr<fml::TaskRunner> runner_;
  std::map<hgc::HermesRuntime*, Session> runtimes_;
  std::unordered_map<JointCell*, std::unique_ptr<JointCell>> cells_;
  std::unordered_map<
      hgc::HermesRuntime*,
      std::unordered_map<uint64_t, std::unordered_set<JointCell*>>>
      peers_;
  std::unordered_set<hgc::HermesRuntime*> started_;
  const Dart_FlaxGC* joint_ = nullptr;
  std::atomic<bool> queued_{false}, collect_requested_{false}, closed_{false};
  std::atomic<bool> in_joint_{false}, cells_empty_{true};
  bool idle_pending_ = false;
};

std::unique_ptr<js::Runtime> createEngineRuntime(size_t* active) {
  auto heap = HermesHeap::Current();
  auto gc =
      ::hermes::vm::GCConfig::Builder()
          .withCallback([](::hermes::vm::GCEventKind kind, const char* info) {
            TRACE_EVENT_INSTANT1(
                "flutter",
                kind == ::hermes::vm::GCEventKind::CollectionEnd
                    ? "FlaxHermesGCEnd"
                    : "FlaxHermesGCStart",
                "kind", info);
          })
          .withAnalyticsCallback(
              [weak = std::weak_ptr<HermesHeap>(heap)](
                  const ::hermes::vm::GCAnalyticsEvent& event) {
                if (auto heap = weak.lock())
                  heap->JsCollected(event);
              })
          .build();
  auto config = ::hermes::vm::RuntimeConfig::Builder()
                    .withES6BlockScoping(true)
                    .withMicrotaskQueue(true)
                    .withGCConfig(gc)
                    .build();
  auto runtime = hgc::makeHermesRuntime(config);
  sessions.emplace(runtime.get(), heap);
  try {
    heap->Add(*runtime, active);
  } catch (...) {
    sessions.erase(runtime.get());
    throw;
  }
  if (std::getenv("FLAX_VERIFY_HERMES"))
    std::puts("FLAX_HERMES: " FLAX_HERMES_REVISION
              " embedded interpreter, joint GC");
  return runtime;
}
js::Value prepareEngineValue(js::Runtime& runtime, js::Value value) {
  // Hermes shares ordinary JSI clones. ABI IDs require independent GC
  // ownership.
  return value.isObject()
             ? js::WeakObject(runtime, value.getObject(runtime)).lock(runtime)
             : std::move(value);
}
JointCell* bindDartPeer(js::Runtime& runtime,
                        const js::Object& object,
                        Dart_Handle target,
                        Dart_Handle origin,
                        bool weaken,
                        void (*cleanup)(void*),
                        void* data) {
  return sessions.at(&runtime)->Bind(static_cast<hgc::HermesRuntime&>(runtime),
                                     object, target, origin, weaken, cleanup,
                                     data);
}
void releaseDartPeer(js::Runtime& runtime, JointCell* cell) {
  sessions.at(&runtime)->Remove(cell);
}
uint64_t jointCellCount(js::Runtime& runtime) {
  return sessions.at(&runtime)->count();
}
void closeEngineRuntime(js::Runtime& runtime) {
  auto it = sessions.find(&runtime);
  if (it == sessions.end())
    return;
  it->second->Close(static_cast<hgc::HermesRuntime&>(runtime));
  sessions.erase(it);
}
void notifyEngineWork(js::Runtime& runtime) {
  auto& heap = sessions.at(&runtime);
  if (heap->count())
    heap->ScheduleIdle();
}
void shutdownJointHeap() {
  std::shared_ptr<HermesHeap> heap;
  {
    std::lock_guard lock(heaps_mutex);
    auto it = heaps.find(Dart_CurrentIsolate());
    if (it != heaps.end()) {
      heap = it->second.lock();
      heaps.erase(it);
    }
  }
  if (heap)
    heap->Shutdown();
}
void notifyIdleJointHeap() {
  std::lock_guard lock(heaps_mutex);
  auto it = heaps.find(Dart_CurrentIsolate());
  if (it != heaps.end())
    if (auto heap = it->second.lock(); heap && heap->count())
      heap->ScheduleIdle();
}
}  // namespace flax
extern "C" __attribute__((visibility("default"))) const FlaxApi*
flax_engine_get_api(uint32_t version) {
  return version == FLAX_ABI_VERSION ? flax::runtimeApi() : nullptr;
}

namespace flax {
Uint8View readUint8Array(facebook::jsi::Runtime& runtime,
          const facebook::jsi::Object& object) {
  if (!object.isUint8Array(runtime))
    throw std::invalid_argument("Expected an actual Uint8Array");
  auto array = object.getUint8Array(runtime);
  auto buffer = array.buffer(runtime);
  if (buffer.detached(runtime))
    throw std::invalid_argument("Expected an attached ArrayBuffer");
  auto storage = buffer.tryGetMutableBuffer(runtime);
  if (!storage)
    throw std::invalid_argument("Hermes did not retain the backing storage");
  auto offset = array.byteOffset(runtime), length = array.byteLength(runtime);
  if (offset > storage->size() || length > storage->size() - offset)
    throw std::invalid_argument("Uint8Array range is out of bounds");
  return {std::move(storage), offset, length};
}

}  // namespace flax
