// Internal macOS texture/fence protocol shared with the Dawn native package.
#ifndef FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_DAWN_INTEROP_H_
#define FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_DAWN_INTEROP_H_
#import <Metal/Metal.h>
#import <objc/runtime.h>

@protocol FlaxDawnTextureProvider <NSObject>
- (id<MTLTexture>)copyFlaxDawnMetalTexture;
@end

@protocol FlaxDawnFrame <NSObject>
- (id<MTLFence>)prepareOnQueue:(id<MTLCommandQueue>)queue;
- (void)encodeReadComplete:(id<MTLCommandBuffer>)commands;
@end

static inline id<FlaxDawnFrame> FlaxDawnFrameForTexture(id<MTLTexture> texture) {
  return objc_getAssociatedObject(texture, @selector(copyFlaxDawnMetalTexture));
}
#endif  // FLUTTER_FLAX_NATIVE_INCLUDE_FLAX_DAWN_INTEROP_H_
