#ifndef FLAX_CANVAS_H_
#define FLAX_CANVAS_H_
#include <stdint.h>
typedef struct _Dart_Handle* Dart_Handle;
#ifdef __cplusplus
extern "C" {
#endif
/* Private Canvas engine extension; independent of the public runtime ABI. */
#define FLAX_CANVAS_VERSION 5
uint32_t flax_canvas_version(void);
/* Returns an error string on failure, otherwise an operation-specific value.
 * 0 create host; 1 create surface;
 * 2 decode [id,width,height,alpha,FlaxFontCollection?],bytes,Canvas;
 * 3 reset surface; 4 retain id,ImageShader; 5 release image;
 * 6 measure id,text,FlaxFontCollection? (null or 0 selects public fonts);
 * 7 contains bytes,[x,y,evenodd]; 8 dispose surface; 9 dispose host;
 * 10 count live hosts, surfaces and image registrations on this isolate;
 * 11 stroke hit id,bytes,[x,y,explicit]; 12 read Picture,[w,h,x,y,sw,sh];
 * 13 RGBA8 snapshot Picture,[w,h] (no host registration required).
 * 14 encode straight RGBA bytes,[w,h,format,quality] (1 JPEG,2 WebP; quality 0..100);
 * CPU-only encoding also accepts a Dart worker isolate, without a host.
 * 15 reference font metrics/family,FlaxFontCollection?.
 * Calls are synchronous, non-leaf and restricted to their owning UI isolate.
 */
Dart_Handle flax_canvas_dispatch(uint64_t host, uint32_t operation,
                                Dart_Handle a, Dart_Handle b, Dart_Handle c);
#ifdef __cplusplus
}
#endif
#endif
