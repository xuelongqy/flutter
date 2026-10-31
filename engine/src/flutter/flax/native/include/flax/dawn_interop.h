// Internal macOS texture/fence protocol shared with the Dawn native package.
#pragma once
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
