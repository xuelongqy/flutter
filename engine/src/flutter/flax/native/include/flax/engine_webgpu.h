#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_WEBGPU_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_WEBGPU_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Optional macOS mapped-buffer and Metal producer/consumer fence profile.
// Independent of native ABI 2, UI protocol 24 and Worker profile 2.
#define FLAX_ENGINE_WEBGPU_INTEROP_VERSION 1
uint32_t flax_webgpu_interop_version(void);
// Query the current root UI isolate; no process-wide supported-state cache.
uint32_t flax_webgpu_renderer_supported(void);
#ifdef __cplusplus
}
#endif
#endif
