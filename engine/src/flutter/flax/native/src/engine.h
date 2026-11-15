#ifndef FLUTTER_FLAX_NATIVE_SRC_ENGINE_H_
#define FLUTTER_FLAX_NATIVE_SRC_ENGINE_H_

#include <dart_api.h>
#include <jsi/jsi.h>
#include <memory>
#include "flax/runtime.h"

namespace flax {
struct JointCell;
struct Uint8View {
  std::shared_ptr<facebook::jsi::MutableBuffer> storage;
  size_t offset;
  size_t length;
};
Uint8View readUint8Array(facebook::jsi::Runtime&, const facebook::jsi::Object&);
std::unique_ptr<facebook::jsi::Runtime> createDetachedRuntime();
void installWorkerBridge(facebook::jsi::Runtime&, int64_t port);
void attachWorker(facebook::jsi::Runtime&, uint64_t channel, int64_t port);
void interruptWorker(uint64_t channel);
uint64_t workerChannelCount();
void retireWorkerRuntime(facebook::jsi::Runtime&);
void setRuntimeWakeupPort(facebook::jsi::Runtime&, int64_t port);
void installModuleController(facebook::jsi::Runtime&,
                             const facebook::jsi::Object& importer,
                             const facebook::jsi::Object& controller);
// Implemented once by the selected engine adapter, not by the shared runtime.
std::unique_ptr<facebook::jsi::Runtime> createEngineRuntime(
    size_t* active_depth);
facebook::jsi::Value prepareEngineValue(facebook::jsi::Runtime&,
                                        facebook::jsi::Value value);
JointCell* bindDartPeer(facebook::jsi::Runtime&,
                        const facebook::jsi::Object&,
                        Dart_Handle target,
                        Dart_Handle origin,
                        bool weaken,
                        void (*cleanup)(void*) = nullptr,
                        void* data = nullptr);
void releaseDartPeer(facebook::jsi::Runtime&, JointCell*);
bool bindOwnedDartPeer(facebook::jsi::Runtime&,
                       const facebook::jsi::Object&,
                       Dart_Handle target);
bool unlinkOwnedDartPeer(facebook::jsi::Runtime&,
                         const facebook::jsi::Object&);
uint64_t jointCellCount(facebook::jsi::Runtime&);
void closeEngineRuntime(facebook::jsi::Runtime&);
void notifyEngineWork(facebook::jsi::Runtime&);
void shutdownJointHeap();
void notifyIdleJointHeap();
const FlaxApi* runtimeApi();
}  // namespace flax
#endif  // FLUTTER_FLAX_NATIVE_SRC_ENGINE_H_
