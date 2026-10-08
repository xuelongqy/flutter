#include "engine.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
unsigned flaxV8JitEventCount();
#include "flax_v8.h"
#include "jsi_abi/JsiAbiRuntime.h"
#include "jsi_abi/v8_jsi_config.h"

namespace flax {
std::unique_ptr<facebook::jsi::Runtime> createEngineRuntime() {
  const auto previousJitEvents = flaxV8JitEventCount();
  auto runtime = jsi::abi::makeJsiAbiRuntime(
      v8_create_runtime,
      [](void *, jsi_config config) {
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
} // namespace flax

extern "C" const void *flax_v8_get_api(uint32_t version) {
  return version == FLAX_ABI_VERSION ? flax::runtimeApi() : nullptr;
}

extern "C" __attribute__((visibility("default")))
const FlaxApi *flax_engine_get_api(uint32_t version) {
  return version == FLAX_ABI_VERSION ? flax::runtimeApi() : nullptr;
}
