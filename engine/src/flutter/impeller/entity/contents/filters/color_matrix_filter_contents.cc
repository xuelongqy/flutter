// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/color_matrix_filter_contents.h"

#include <cmath>
#include <optional>

#include "impeller/core/formats.h"

#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/contents.h"
#include "impeller/entity/contents/filters/color_filter_contents.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/vector.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {

ColorMatrixFilterContents::ColorMatrixFilterContents() = default;

ColorMatrixFilterContents::~ColorMatrixFilterContents() = default;

void ColorMatrixFilterContents::SetMatrix(const ColorMatrix& matrix) {
  matrix_ = matrix;
}

ColorMatrixContents::ColorMatrixContents(Snapshot input,
                                         ColorMatrix matrix,
                                         Scalar input_alpha,
                                         Rect coverage)
    : input_(std::move(input)),
      matrix_(matrix),
      input_alpha_(input_alpha),
      coverage_(coverage) {}

bool ColorMatrixContents::CanRenderCanvasScreen(const Entity& entity,
                                                ISize output_size) const {
  if (entity.GetTransform() != Matrix() ||
      !input_.transform.IsTranslationOnly() || !input_.texture ||
      input_.texture->GetMipCount() != 1 ||
      input_.texture->GetTextureDescriptor().format !=
          PixelFormat::kR8G8B8A8UNormInt ||
      input_alpha_ != 1) {
    return false;
  }
  const auto offset = input_.transform.vec[3];
  if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || offset.z != 0 ||
      offset.x != std::round(offset.x) || offset.y != std::round(offset.y)) {
    return false;
  }
  for (float value : matrix_.array) {
    if (!std::isfinite(value) || std::abs(value) > 16)
      return false;
  }
  if (matrix_.array[4] != 0 || matrix_.array[9] != 0 ||
      matrix_.array[14] != 0 || matrix_.array[15] != 0 ||
      matrix_.array[16] != 0 || matrix_.array[17] != 0 ||
      matrix_.array[18] != 1 || matrix_.array[19] != 0) {
    return false;
  }
  // Integer edges have no partial MSAA coverage. Keep the original viewport;
  // the screen shader supplies clear texels outside the input, including clips.
  return !output_size.IsEmpty();
}

std::optional<Rect> ColorMatrixContents::GetCoverage(
    const Entity& entity) const {
  return coverage_.TransformBounds(entity.GetTransform());
}

template <typename FS>
bool ColorMatrixContents::RenderMatrix(const ContentContext& renderer,
                                       const Entity& entity,
                                       RenderPass& pass,
                                       std::optional<ISize> screen_size) const {
  using VS = ColorMatrixColorFilterPipeline::VertexShader;

  auto options = OptionsFromPassAndEntity(pass, entity);
  options.primitive_type = PrimitiveType::kTriangleStrip;
  if (screen_size) {
    options.blend_mode = BlendMode::kSrc;
    pass.SetPipeline(renderer.GetCanvasColorMatrixScreenPipeline(options));
    pass.SetCommandLabel("Canvas Quantized Color Matrix Screen");
  } else {
    pass.SetPipeline(renderer.GetColorMatrixColorFilterPipeline(options));

    pass.SetCommandLabel("Color Matrix Filter");
  }
  auto size = input_.texture->GetSize();

  std::array<VS::PerVertexData, 4> vertices = {
      VS::PerVertexData{Point(0, 0), Point(0, 0)},
      VS::PerVertexData{Point(1, 0), Point(1, 0)},
      VS::PerVertexData{Point(0, 1), Point(0, 1)},
      VS::PerVertexData{Point(1, 1), Point(1, 1)},
  };
  if (screen_size) {
    const auto offset = input_.transform.vec[3];
    for (auto& vertex : vertices) {
      vertex.texture_coords = Point(
          (vertex.position.x * screen_size->width - offset.x) / size.width,
          (vertex.position.y * screen_size->height - offset.y) / size.height);
    }
  }
  auto& data = renderer.GetTransientsDataBuffer();
  pass.SetVertexBuffer(CreateVertexBuffer(vertices, data));

  VS::FrameInfo frame_info;
  const auto transform =
      screen_size ? Matrix::MakeScale(Vector2(Size(*screen_size)))
                  : input_.transform * Matrix::MakeScale(Vector2(size));
  frame_info.mvp = Entity::GetShaderTransform(
      entity.GetShaderClipDepth(), pass, entity.GetTransform() * transform);

  typename FS::FragInfo frag_info;
  const float* matrix = matrix_.array;
  frag_info.color_v = Vector4(matrix[4], matrix[9], matrix[14], matrix[19]);
  frag_info.color_m =
      Matrix(matrix[0], matrix[5], matrix[10], matrix[15], matrix[1], matrix[6],
             matrix[11], matrix[16], matrix[2], matrix[7], matrix[12],
             matrix[17], matrix[3], matrix[8], matrix[13], matrix[18]);
  frag_info.input_alpha = input_alpha_;
  frag_info.output_alpha = 1;

  auto sampler = renderer.GetContext()->GetSamplerLibrary()->GetSampler({});
  FS::BindInputTexture(pass, input_.texture, sampler);
  FS::BindFragInfo(pass, data.EmplaceUniform(frag_info));
  VS::BindFrameInfo(pass, data.EmplaceUniform(frame_info));

  return pass.Draw().ok();
}
bool ColorMatrixContents::Render(const ContentContext& renderer,
                                 const Entity& entity,
                                 RenderPass& pass) const {
  return RenderMatrix<ColorMatrixColorFilterPipeline::FragmentShader>(
      renderer, entity, pass, std::nullopt);
}

bool ColorMatrixContents::RenderCanvasScreen(const ContentContext& renderer,
                                             const Entity& entity,
                                             RenderPass& pass,
                                             ISize output_size) const {
  return RenderMatrix<CanvasColorMatrixScreenPipeline::FragmentShader>(
      renderer, entity, pass, output_size);
}

std::optional<Entity> ColorMatrixFilterContents::RenderFilter(
    const FilterInput::Vector& inputs,
    const ContentContext& renderer,
    const Entity& entity,
    const Matrix& effect_transform,
    const Rect& coverage,
    const std::optional<Rect>& coverage_hint) const {
  if (inputs.empty())
    return std::nullopt;
  auto input = inputs[0]->GetSnapshot("ColorMatrix", renderer, entity);
  if (!input)
    return std::nullopt;

  auto contents = std::make_shared<ColorMatrixContents>(
      input.value(), matrix_,
      GetAbsorbOpacity() == ColorFilterContents::AbsorbOpacity::kYes
          ? input->opacity
          : 1.0f,
      coverage);

  Entity result;
  result.SetContents(std::move(contents));
  result.SetBlendMode(entity.GetBlendMode());
  return result;
}

}  // namespace impeller
