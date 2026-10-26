#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_MODULES_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_MODULES_H_

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLAX_ENGINE_MODULES_VERSION 1
#define FLAX_MODULE_JAVASCRIPT 1
#define FLAX_MODULE_JSON 2
#define FLAX_MODULE_WASM 4
#define FLAX_MODULE_SOURCE_IMPORTS 8

/* Optional native module controller. Calls use the ordinary owner-isolate
 * stack. The importer is a JS function returning a Promise for a dynamic
 * import. install returns one owned controller value, released with the base
 * ABI. */
typedef struct FlaxEngineModulesApi {
  uint32_t version;
  size_t struct_size;
  uint32_t capabilities;
  int32_t (*install)(FlaxRuntime* runtime,
                     FlaxValueId importer,
                     FlaxValueId* controller,
                     FlaxError* error);
  /* Thread-safe VM notifications only; JS still enters from the owner isolate.
   */
  int32_t (*set_wakeup_port)(FlaxRuntime* runtime,
                             int64_t port,
                             FlaxError* error);
} FlaxEngineModulesApi;

const FlaxEngineModulesApi* flax_engine_get_modules_api(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif  // FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_MODULES_H_
