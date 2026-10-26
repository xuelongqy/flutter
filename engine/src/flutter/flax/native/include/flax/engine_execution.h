#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_EXECUTION_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_EXECUTION_H_

#include "engine_binding.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLAX_ENGINE_EXECUTION_VERSION 1

typedef struct FlaxSource {
  const uint8_t* data;
  size_t length;
  const uint8_t* url;
  size_t url_length;
} FlaxSource;

/* Optional, versioned extension. Native ABI 2 and the binding table are
 * unchanged. */
typedef struct FlaxEngineExecutionApi {
  uint32_t version;
  size_t struct_size;
  /* Primitives cross by value. Objects transfer an owned ID; strings transfer
   * info.string_data, freed with buffer_free. Failure leaves no owned outputs.
   */
  int32_t (*call)(FlaxRuntime*,
                  FlaxValueId,
                  const FlaxBindingArgument*,
                  const FlaxBindingArgument*,
                  size_t,
                  FlaxValueId*,
                  FlaxValueInfo*,
                  FlaxError*);
  int32_t (*get_property)(FlaxRuntime*,
                          FlaxValueId,
                          const uint16_t*,
                          size_t,
                          FlaxValueId*,
                          FlaxValueInfo*,
                          FlaxError*);
  /* Describes the borrowed receiver and arguments. Caller frees string_data. */
  int32_t (*inspect_arguments)(FlaxRuntime*,
                               FlaxValueId,
                               const FlaxValueId*,
                               size_t,
                               FlaxValueInfo*,
                               FlaxError*);
  /* Copies inputs before returning. Execute pure module sources, then compile
   * (never execute) entry. started=0 selects ordinary UI-thread loading when
   * another runtime or Dart peer already uses the shared heap. */
  int32_t (*prepare)(FlaxRuntime*,
                     const FlaxSource*,
                     size_t,
                     const FlaxSource* entry,
                     int32_t* started,
                     FlaxError*);
  /* Nonblocking owner-thread polling. Completion restores UI ownership, even
   * after an exception or cancellation. ready=0 waits, 1 adopts, -1 selects UI
   * fallback after another runtime interrupts pure preparation. No Dart
   * callbacks run on the worker. */
  int32_t (*poll)(FlaxRuntime*, int32_t* ready, FlaxError*);
  int32_t (*cancel)(FlaxRuntime*, FlaxError*);
} FlaxEngineExecutionApi;

const FlaxEngineExecutionApi* flax_get_engine_execution_api(uint32_t);

#ifdef __cplusplus
}
#endif
#endif  // FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_EXECUTION_H_
