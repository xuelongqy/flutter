#version 450

// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/blending.glsl>
#include <impeller/color.glsl>
#include <impeller/types.glsl>

layout(input_attachment_index = 0) uniform subpassInputMS uSub;

uniform FragInfo {
  mat4 color_m;
  f16vec4 color_v;
  float16_t input_alpha;
  float16_t output_alpha;
}
frag_info;

// Subpass-input shaders require a 32-bit sampled image in SPIR-V.
uniform sampler2D input_texture;

in highp vec2 v_texture_coords;
out vec4 frag_color;

float QuantizeUnorm8(float value) {
#ifdef IMPELLER_TARGET_METAL_DESKTOP
  // Match attachment rounding without rounding value * 255 at a float32 tie.
  if (value < 1.0 / 512.0)
    return 0.0;
  if (value >= 1.0)
    return 1.0;
  uint bits = floatBitsToUint(value);
  uint product = ((bits & 0x7fffffu) | 0x800000u) * 255u;
  uint shift = 150u - ((bits >> 23u) & 255u);
  uint lower = shift == 32u ? 0u : product >> shift;
  uint halfway = 1u << (shift - 1u);
  uint remainder = shift == 32u ? product : product & ((1u << shift) - 1u);
  uint result = lower + uint(remainder > halfway ||
                             (remainder == halfway && (lower & 1u) != 0u));
  if (result == 0u)
    return 0.0;
  if (result == 255u)
    return 1.0;
  // Reconstruct the texture sample; a rounded 1/255 reciprocal can differ.
  return float(result * 0x01010101u + 1u) * 2.3283064365386963e-10;
#else
  float scaled = value * 255.0;
  float lower = floor(scaled);
  float fraction = scaled - lower;
  float increment =
      fraction > 0.5 ? 1.0 : (fraction == 0.5 ? mod(lower, 2.0) : 0.0);
  return (lower + increment) / 255.0;
#endif
}

void main() {
  // Keep the original attachment's transparent border and screen rounding
  // there.
  f16vec4 color = f16vec4(0.0hf);
  if (all(greaterThanEqual(v_texture_coords, vec2(0))) &&
      all(lessThan(v_texture_coords, vec2(1)))) {
    color = f16vec4(texture(input_texture, v_texture_coords)) *
            frag_info.input_alpha;
  }
  color = IPHalfUnpremultiply(color);
  color = clamp(f16mat4(frag_info.color_m) * color + frag_info.color_v,
                float16_t(0), float16_t(1));
  color = IPHalfPremultiply(color) * frag_info.output_alpha;

  // Preserve the removed RGBA8 attachment's store and subsequent half sampling.
  highp vec4 unorm =
      vec4(QuantizeUnorm8(float(color.r)), QuantizeUnorm8(float(color.g)),
           QuantizeUnorm8(float(color.b)), QuantizeUnorm8(float(color.a)));
  vec4 destination = (subpassLoad(uSub, 0) + subpassLoad(uSub, 1) +
                      subpassLoad(uSub, 2) + subpassLoad(uSub, 3)) /
                     vec4(4);
  f16vec4 dst = IPHalfUnpremultiply(f16vec4(destination));
  dst.a *= frag_info.output_alpha;
  f16vec4 src = IPHalfUnpremultiply(f16vec4(unorm));
  src.a *= frag_info.output_alpha;
  frag_color = IPApplyBlendedColor(dst, src, IPBlendScreen(dst.rgb, src.rgb));
}
