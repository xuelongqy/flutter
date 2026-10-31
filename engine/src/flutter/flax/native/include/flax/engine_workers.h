#ifndef FLAX_ENGINE_WORKERS_H_
#define FLAX_ENGINE_WORKERS_H_
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FLAX_ENGINE_WORKERS_VERSION 2
/* Detached runtimes have explicit references and no Dart/Flutter object peers.
 * interrupt is thread safe; all other entries use the owning Dart isolate. */
typedef struct FlaxEngineWorkersApi {
  uint32_t version;
  size_t struct_size;
  int32_t (*create)(FlaxRuntime**, FlaxError*);
  int32_t (*install)(FlaxRuntime*, int64_t port, FlaxError*);
  int32_t (*attach)(FlaxRuntime*, uint64_t channel, int64_t port, FlaxError*);
  void (*interrupt)(uint64_t channel);
  int32_t (*retire)(FlaxRuntime*, FlaxError*);
  void (*counts)(uint64_t* channels, uint64_t* runtimes);
} FlaxEngineWorkersApi;
const FlaxEngineWorkersApi* flax_engine_get_workers_api(uint32_t version);
#ifdef __cplusplus
}
#endif
#endif
