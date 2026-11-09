#ifndef FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_
#define FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_

#include <cppgc/persistent.h>
#include <cppgc/prefinalizer.h>
#include <dart_flax_api.h>
#include <v8-cppgc.h>
#include <atomic>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
#include "flutter/fml/task_runner.h"
#include "v8_core.h"
namespace tonic {
class DartState;
}

namespace flax {
class JointHeap;
struct JointCell;

struct JointPeer final : v8::Object::Wrappable {
 private:
  CPPGC_USING_PRE_FINALIZER(JointPeer, Detach);
  void Detach();

 public:
  void Trace(cppgc::Visitor* visitor) const;
  std::vector<JointCell*> cells;
  v8::TracedReference<v8::Object> value;
};

struct JointCell {
  JointHeap* heap;
  const void* session;
  Dart_WeakPersistentHandle origin = nullptr;
  Dart_WeakPersistentHandle target = nullptr;
  cppgc::Persistent<JointPeer> root;
  cppgc::WeakPersistent<JointPeer> peer;
  std::atomic<bool> cpp_dead{false};
  bool dart_live = false;
  bool owned_target = false;
  void* cleanup_data = nullptr;
  void (*cleanup)(void*) = nullptr;
};

// One heap per UI isolate. Contexts own sessions; cells never own Dart values.
class JointHeap : public std::enable_shared_from_this<JointHeap> {
 public:
  static std::shared_ptr<JointHeap> Current(const v8rt::IsolateConfig& config);
  static void ShutdownCurrent();
  static void NotifyIdleCurrent();
  static int64_t IncrementalJointExperiment(int32_t op);
  static std::shared_ptr<JointHeap> BeginPreparation();
  static void CheckPreparation();
  void CancelPreparation();
  void EndPreparation();
  ~JointHeap();
  v8::Isolate* isolate() const { return isolate_; }
  JointCell* Bind(const void* session,
                  v8::Local<v8::Object> value,
                  Dart_Handle target,
                  Dart_Handle origin,
                  void (*cleanup)(void*) = nullptr,
                  void* cleanup_data = nullptr);
  bool BindOwned(const void* session,
                 v8::Local<v8::Object> view,
                 Dart_Handle target);
  bool UnlinkOwned(const void* session, v8::Local<v8::Object> view);
  void CloseSession(const void* session);
  void Release(JointCell* cell) { Remove(cell); }
  void Trace(const JointPeer* peer, cppgc::Visitor* visitor);
  void Request(bool collect);
  void ScheduleIdle(bool js_changed = true);
  size_t cell_count() const { return cells_.size(); }
  void TryReserveForegroundGC(bool wakeup_pending);
  void ReleaseForegroundGC();

 private:
  explicit JointHeap(const v8rt::IsolateConfig& config);
  static void Collect(void* peer, Dart_FlaxGCEpoch epoch);
  static void RequestFromDart(void* peer);
  static bool InOutermostForegroundPump(v8::Isolate* isolate);
  static void JsCollectionStarted(v8::Isolate*,
                                  v8::GCType,
                                  v8::GCCallbackFlags,
                                  void*);
  static void JsCollected(v8::Isolate*, v8::GCType, v8::GCCallbackFlags, void*);
  v8::Local<v8::Context> OwnedViewContext(const void* session,
                                          v8::Local<v8::Object> view);
  void Prune();
  void Remove(JointCell* cell);
  void Shutdown();
  void RestoreRoots();
  void GuardAllPeers();
  void ResetConfirmation();
  void ScheduleConfirmation(bool dart_requested = false);
  void ConfirmAtDeadline();
  void ConfirmFromDart();
  bool Checkpoint(const std::vector<Dart_WeakPersistentHandle>& targets,
                  std::vector<JointPeer*>* origins,
                  bool* all_targets_marked = nullptr);
  v8::Isolate* isolate_ = nullptr;
  Dart_Isolate dart_isolate_;
  std::thread::id owner_ = std::this_thread::get_id();
  std::weak_ptr<tonic::DartState> dart_state_;
  fml::RefPtr<fml::TaskRunner> runner_;
  v8::Global<v8::Private> peers_key_;
  v8::Global<v8::FunctionTemplate> peer_template_;
  std::unordered_map<JointCell*, std::unique_ptr<JointCell>> cells_;
  std::unordered_map<Dart_WeakPersistentHandle, JointCell*> origins_;
  Dart_FlaxGCEpoch joint_epoch_ = 0;
  bool collecting_ = false, aborted_ = false;
  bool force_js_collection_ = false;
  std::atomic<bool> queued_{false}, collect_requested_{false}, closed_{false};
  bool idle_pending_ = false, dirty_ = false;
  bool external_gc_pending_ = false;
  bool preparing_ = false;
  bool one_way_gc_ = true;
  bool incremental_start_latched_ = false;
  std::optional<fml::TimePoint> confirmation_due_;
  std::optional<fml::TimePoint> fallback_due_;
  bool confirmation_task_pending_ = false;
  bool deadline_major_armed_ = false;
  bool joined_dart_request_ = false;
  bool confirming_from_dart_ = false;
  int32_t reserved_isolates_ = 0;
  int64_t worker_gate_admitted_ = 0, worker_gate_joined_ = 0;
  int64_t worker_gate_released_ = 0, gate_rejected_ = 0;
  int64_t worker_gate_total_us_ = 0, worker_gate_max_us_ = 0;
  std::atomic<int64_t> confirmation_signals_{0}, confirmation_posts_{0};
  std::atomic<int64_t> incremental_starts_{0}, major_epilogues_{0};
  std::atomic<int64_t> joined_majors_{0}, major_followups_{0};
  std::atomic<int64_t> rejected_joins_{0}, certified_collections_{0};
};
}  // namespace flax
#endif  // FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_
