#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "engine.h"
unsigned flaxV8JitEventCount();
#include "flax_v8.h"
#include "joint_gc.h"
#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/jsi_abi_v8_internal.h"
#include "jsi_abi/v8_jsi_config.h"

namespace flax {
std::unique_ptr<facebook::jsi::Runtime> createEngineRuntime(size_t*) {
  const auto previousJitEvents = flaxV8JitEventCount();
  auto runtime = jsi::abi::makeJsiAbiRuntime(
      v8_create_runtime,
      [](void*, jsi_config config) {
        v8_jsi_config_set_explicit_microtask_policy(config, true);
        v8_jsi_config_enable_multi_thread(config, true);
        v8_jsi_config_enable_jit_tracing(
            config, std::getenv("FLAX_VERIFY_V8_JIT") != nullptr);
        return jsi_no_error;
      },
      nullptr);
#ifndef V8_JITLESS
  if (std::getenv("FLAX_VERIFY_V8_JIT")) {
    runtime->evaluateJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>(
            "function flaxJitProbe(x) { return x + 1; }"
            "%PrepareFunctionForOptimization(flaxJitProbe);"
            "flaxJitProbe(1); flaxJitProbe(2);"
            "%OptimizeFunctionOnNextCall(flaxJitProbe); flaxJitProbe(3);"),
        "flax:jit-verification");
    if (flaxV8JitEventCount() == previousJitEvents) {
      throw std::runtime_error(
          "V8 did not generate machine code for the JIT probe");
    }
  }
#else
  if (std::getenv("FLAX_VERIFY_V8_JIT")) {
    runtime->evaluateJavaScript(
        std::make_shared<facebook::jsi::StringBuffer>(
            "function flaxJitProbe(x) { return x + 1; }"
            "for (let i = 0; i < 10000; i++) flaxJitProbe(i);"),
        "flax:jitless-verification");
    if (flaxV8JitEventCount() != previousJitEvents) {
      throw std::runtime_error("iOS V8 unexpectedly generated machine code");
    }
    std::puts("FLAX_V8_JITLESS: probe executed without generated machine code");
  }
#endif
  return runtime;
}
facebook::jsi::Value prepareEngineValue(facebook::jsi::Runtime&,
                                        facebook::jsi::Value value) {
  return value;
}
JointCell* bindDartPeer(facebook::jsi::Runtime& runtime,
                        const facebook::jsi::Object& object,
                        Dart_Handle target,
                        Dart_Handle origin,
                        bool weaken,
                        void (*cleanup)(void*),
                        void* data) {
  return v8rt_internal::bindDartPeer(jsi::abi::getAbiRuntime(runtime),
                                     jsi::abi::getAbiObject(runtime, object),
                                     target, origin, weaken, cleanup, data);
}
void releaseDartPeer(facebook::jsi::Runtime& runtime, JointCell* cell) {
  v8rt_internal::releaseDartPeer(jsi::abi::getAbiRuntime(runtime), cell);
}
uint64_t jointCellCount(facebook::jsi::Runtime& runtime) {
  return v8rt_internal::jointCellCount(jsi::abi::getAbiRuntime(runtime));
}
void closeEngineRuntime(facebook::jsi::Runtime&) {}
// V8 already schedules idle work when its outermost JSI scope exits.
void notifyEngineWork(facebook::jsi::Runtime&) {}
void shutdownJointHeap() {
  JointHeap::ShutdownCurrent();
}
void notifyIdleJointHeap() {
  JointHeap::NotifyIdleCurrent();
}
}  // namespace flax

extern "C" const void* flax_v8_get_api(uint32_t version) {
  return version == FLAX_ABI_VERSION ? flax::runtimeApi() : nullptr;
}

extern "C" __attribute__((visibility("default"))) const FlaxApi*
flax_engine_get_api(uint32_t version) {
  return version == FLAX_ABI_VERSION ? flax::runtimeApi() : nullptr;
}
