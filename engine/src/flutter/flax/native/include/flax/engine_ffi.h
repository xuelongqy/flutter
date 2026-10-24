#ifndef FLAX_ENGINE_FFI_H
#define FLAX_ENGINE_FFI_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FLAX_ENGINE_FFI_VERSION 1

/* Optional internal extension. All operations run on the idle owner isolate. */
typedef struct FlaxEngineFfiApi {
  uint32_t version;
  size_t struct_size;
  int32_t (*install)(FlaxRuntime*, int64_t wake_port, FlaxError*);
  int32_t (*checkpoint)(FlaxRuntime*, FlaxError*);
  /* Revokes new work, stops owned producers, then waits for active callbacks. */
  int32_t (*retire)(FlaxRuntime*, uint8_t* complete, FlaxError*);
} FlaxEngineFfiApi;
const FlaxEngineFfiApi* flax_engine_get_ffi_api(uint32_t version);
#ifdef __cplusplus
}
#endif
#endif
