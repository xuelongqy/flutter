#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FLAX_ENGINE_BINDING_VERSION 1

/* Internal, synchronous engine extension. The public native ABI remains 2. */
typedef struct FlaxBindingArgument {
  uint32_t kind;
  double number;
  FlaxValueId id;
  const uint16_t* text;
  size_t length;
} FlaxBindingArgument;

typedef struct FlaxEngineBindingApi {
  uint32_t version;
  size_t struct_size;
  int32_t (*register_members)(FlaxRuntime*,
                              FlaxValueId layout,
                              uint64_t* first,
                              FlaxError*);
  int32_t (*invoke_member)(FlaxRuntime*,
                           FlaxValueId receiver,
                           uint64_t member,
                           const FlaxBindingArgument*,
                           size_t count,
                           int32_t* handled,
                           FlaxValueId* output,
                           FlaxError*);
  int32_t (*call)(FlaxRuntime*,
                  FlaxValueId function,
                  const FlaxBindingArgument* receiver,
                  const FlaxBindingArgument*,
                  size_t count,
                  FlaxValueId* output,
                  FlaxError*);
} FlaxEngineBindingApi;
const FlaxEngineBindingApi* flax_engine_get_binding_api(uint32_t version);
#ifdef __cplusplus
}
#endif
#endif  // FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
