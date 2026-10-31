#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_IMAGES_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_ENGINE_IMAGES_H_
#include <stdint.h>
typedef struct _Dart_Handle* Dart_Handle;
#ifdef __cplusplus
extern "C" {
#endif
/* Optional shared image codec extension; independent of native ABI 2.
 * Dispatch returns an error string or the operation's value.
 * 0 encode RGBA bytes,[width,height,format,quality]; 1 decode encoded bytes,
 * [convert,premultiply,orient]; 2 transform RGBA bytes,bitmap options;
 * 3 create ui.Image from straight RGBA bytes,[width,height];
 * 4 read RGBA from Picture,[width,height,x,y,width,height].
 * Codec operations 0..2 are CPU-only and may run in a Dart worker isolate.
 * Operations 3..4 require the owning Flutter UI isolate. All calls are
 * non-leaf.
 */
#define FLAX_ENGINE_IMAGES_VERSION 1
uint32_t flax_image_version(void);
Dart_Handle flax_image_dispatch(uint32_t operation,
                                Dart_Handle a,
                                Dart_Handle b,
                                Dart_Handle c);
#ifdef __cplusplus
}
#endif
#endif
