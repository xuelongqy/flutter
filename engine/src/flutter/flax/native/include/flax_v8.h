#ifndef FLAX_V8_H
#define FLAX_V8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the FlaxApi table for the requested ABI, or NULL if unsupported.
 * The table has static lifetime. Engine-specific C++ types never cross it. */
#if defined(_WIN32)
#if defined(FLAX_BUILDING_BRIDGE)
#define FLAX_ENGINE_EXPORT __declspec(dllexport)
#else
#define FLAX_ENGINE_EXPORT __declspec(dllimport)
#endif
#else
#define FLAX_ENGINE_EXPORT __attribute__((visibility("default")))
#endif
FLAX_ENGINE_EXPORT const void *
flax_v8_get_api(uint32_t version);

#ifdef __cplusplus
}
#endif
#endif
