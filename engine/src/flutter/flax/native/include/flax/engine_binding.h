#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
#include "engine_gc.h"
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
#define FLAX_ENGINE_BINDING_VERSION 3

/* These indices match FlaxBufferType. All views share the same byte lease. */
enum FlaxBufferKind {
  FLAX_ARRAY_BUFFER = 0,
  FLAX_INT8_ARRAY = 1,
  FLAX_UINT8_ARRAY = 2,
  FLAX_UINT8_CLAMPED_ARRAY = 3,
  FLAX_INT16_ARRAY = 4,
  FLAX_UINT16_ARRAY = 5,
  FLAX_INT32_ARRAY = 6,
  FLAX_UINT32_ARRAY = 7,
  FLAX_BIGINT64_ARRAY = 8,
  FLAX_BIGUINT64_ARRAY = 9,
  FLAX_FLOAT32_ARRAY = 10,
  FLAX_FLOAT64_ARRAY = 11,
  FLAX_DATA_VIEW = 12
};

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
  /* Shares fixed, writable bytes. Neither operation copies the payload. */
  int32_t (*share_buffer)(FlaxRuntime*,
                         Dart_Handle,
                         uint32_t kind,
                         FlaxValueId* output,
                         FlaxError*);
  Dart_Handle (*read_buffer)(FlaxRuntime*, FlaxValueId, uint32_t kind, FlaxError*);
} FlaxEngineBindingApi;
const FlaxEngineBindingApi* flax_engine_get_binding_api(uint32_t version);
#ifdef __cplusplus
}
#endif
#endif  // FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_BINDING_H_
