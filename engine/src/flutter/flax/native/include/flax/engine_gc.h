#ifndef FLAX_ENGINE_GC_H
#define FLAX_ENGINE_GC_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _Dart_Handle* Dart_Handle;
#define FLAX_ENGINE_GC_VERSION 1

/* Internal engine extension. The public native ABI remains 2. */
typedef struct FlaxEngineGcApi {
  uint32_t version;
  size_t struct_size;
  const char* flutter_revision;
  const char* dart_revision;
  int32_t (*bind_value)(FlaxRuntime*, FlaxValueId, Dart_Handle origin, FlaxError*);
  int32_t (*bind_peer)(FlaxRuntime*, FlaxValueId, Dart_Handle target,
                       Dart_Handle origin, FlaxError*);
  int32_t (*register_host)(FlaxRuntime*, const uint16_t*, size_t,
                           FlaxHostCallback, void*, Dart_Handle, FlaxError*);
  uint64_t (*cell_count)(FlaxRuntime*);
} FlaxEngineGcApi;
const FlaxApi* flax_engine_get_api(uint32_t version);
const FlaxEngineGcApi* flax_engine_get_gc_api(uint32_t version);
/* Called by the embedder before tearing down the Dart UI isolate. */
void flax_engine_shutdown_current_isolate(void);
void flax_engine_notify_idle_current_isolate(void);
#ifdef __cplusplus
}
#endif
#endif
