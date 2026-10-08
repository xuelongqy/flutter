#ifndef FLAX_ENGINE_H
#define FLAX_ENGINE_H

#include <dart_api.h>
#include <jsi/jsi.h>
#include <memory>
#include "flax/runtime.h"

namespace flax {
struct JointCell;
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
uint64_t jointCellCount(facebook::jsi::Runtime&);
void closeEngineRuntime(facebook::jsi::Runtime&);
void notifyEngineWork(facebook::jsi::Runtime&);
void shutdownJointHeap();
void notifyIdleJointHeap();
const FlaxApi* runtimeApi();
}  // namespace flax
#endif
