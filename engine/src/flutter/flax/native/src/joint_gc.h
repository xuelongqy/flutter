#ifndef FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_
#define FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_

#include <cppgc/persistent.h>
#include <cppgc/prefinalizer.h>
#include <dart_flax_api.h>
#include <v8-cppgc.h>
#include <atomic>
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
  void* cleanup_data = nullptr;
  void (*cleanup)(void*) = nullptr;
};

// One heap per UI isolate. Contexts own sessions; cells never own Dart values.
class JointHeap : public std::enable_shared_from_this<JointHeap> {
 public:
  static std::shared_ptr<JointHeap> Current(const v8rt::IsolateConfig& config);
  static void ShutdownCurrent();
  static void NotifyIdleCurrent();
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
  void CloseSession(const void* session);
  void Release(JointCell* cell) { Remove(cell); }
  void Trace(JointCell* cell, cppgc::Visitor* visitor);
  void Request(bool collect);
  void ScheduleIdle(bool js_changed = true);
  size_t cell_count() const { return cells_.size(); }

 private:
  explicit JointHeap(const v8rt::IsolateConfig& config);
  static void Collect(void* peer, const Dart_FlaxGC* gc);
  static void RequestFromDart(void* peer);
  static void JsCollected(v8::Isolate*, v8::GCType, v8::GCCallbackFlags, void*);
  void Prune();
  void Remove(JointCell* cell);
  void Shutdown();
  void RestoreRoots();
  v8::Isolate* isolate_ = nullptr;
  Dart_Isolate dart_isolate_;
  std::thread::id owner_ = std::this_thread::get_id();
  std::weak_ptr<tonic::DartState> dart_state_;
  fml::RefPtr<fml::TaskRunner> runner_;
  v8::Global<v8::Private> peers_key_;
  std::unordered_map<JointCell*, std::unique_ptr<JointCell>> cells_;
  std::unordered_map<Dart_WeakPersistentHandle, JointCell*> origins_;
  const Dart_FlaxGC* joint_ = nullptr;
  bool force_js_collection_ = false;
  std::atomic<bool> queued_{false}, collect_requested_{false}, closed_{false};
  bool idle_pending_ = false, dirty_ = false;
  bool preparing_ = false;
};
}  // namespace flax
#endif  // FLUTTER_FLAX_NATIVE_SRC_JOINT_GC_H_
