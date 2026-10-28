// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/entity_pass_target.h"

#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture.h"

namespace impeller {

static const constexpr RenderTarget::AttachmentConfig kDefaultStencilConfig =
    RenderTarget::AttachmentConfig{
        .storage_mode = StorageMode::kDeviceTransient,
        .load_action = LoadAction::kDontCare,
        .store_action = StoreAction::kDontCare,
    };

static RenderTarget CreateRenderTarget(
    const ContentContext& renderer,
    ISize size,
    const Color& clear_color,
    std::optional<PixelFormat> pixel_format) {
  const std::shared_ptr<Context>& context = renderer.GetContext();

  /// All of the load/store actions are managed by `InlinePassContext` when
  /// `RenderPasses` are created, so we just set them to `kDontCare` here.
  /// What's important is the `StorageMode` of the textures, which cannot be
  /// changed for the lifetime of the textures.

  RenderTarget target;
  if (context->GetCapabilities()->SupportsOffscreenMSAA()) {
    target = renderer.GetRenderTargetCache()->CreateOffscreenMSAA(
        /*context=*/*context,
        /*size=*/size,
        /*mip_count=*/1,
        /*label=*/"EntityPass",
        /*color_attachment_config=*/
        RenderTarget::AttachmentConfigMSAA{
            .storage_mode = StorageMode::kDeviceTransient,
            .resolve_storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kDontCare,
            .store_action = StoreAction::kMultisampleResolve,
            .clear_color = clear_color},
        /*stencil_attachment_config=*/kDefaultStencilConfig, nullptr, nullptr,
        nullptr, pixel_format);
  } else {
    target = renderer.GetRenderTargetCache()->CreateOffscreen(
        *context,  // context
        size,      // size
        /*mip_count=*/1,
        "EntityPass",  // label
        RenderTarget::AttachmentConfig{
            .storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kDontCare,
            .store_action = StoreAction::kDontCare,
            .clear_color = clear_color,
        },  // color_attachment_config
        kDefaultStencilConfig, nullptr, nullptr, pixel_format);
  }

  return target;
}

EntityPassTarget::EntityPassTarget(const RenderTarget& render_target,
                                   bool supports_read_from_resolve,
                                   bool supports_implicit_msaa)
    : target_(render_target),
      supports_read_from_resolve_(supports_read_from_resolve),
      supports_implicit_msaa_(supports_implicit_msaa) {}

EntityPassTarget::EntityPassTarget(const ContentContext& renderer,
                                   ISize size,
                                   Color clear_color)
    : pending_target_(PendingTarget{
          &renderer, size, renderer.GetSubpassPixelFormat(), clear_color}),
      supports_read_from_resolve_(
          renderer.GetDeviceCapabilities().SupportsReadFromResolve()),
      supports_implicit_msaa_(
          renderer.GetDeviceCapabilities().SupportsImplicitResolvingMSAA()) {}

std::shared_ptr<Texture> EntityPassTarget::Flip(
    const ContentContext& renderer) {
  ColorAttachment color0 = target_.GetColorAttachment(0);
  if (!color0.resolve_texture) {
    VALIDATION_LOG << "EntityPassTarget Flip should never be called for a "
                      "non-MSAA target.";
    // ...because there is never a circumstance where doing so would be
    // necessary. Unlike MSAA passes, non-MSAA passes can be trivially loaded
    // with `LoadAction::kLoad`.
    return color0.texture;
  }

  if (supports_read_from_resolve_) {
    // Just return the current resolve texture, which is safe to read in the
    // next render pass that'll resolve to `target_`.
    //
    // Note that this can only be done when MSAA is being used.
    return color0.resolve_texture;
  }

  if (!secondary_color_texture_) {
    // The second texture is allocated lazily to avoid unused allocations.
    TextureDescriptor new_descriptor =
        color0.resolve_texture->GetTextureDescriptor();
    RenderTarget target = renderer.GetRenderTargetCache()->CreateOffscreenMSAA(
        *renderer.GetContext(), new_descriptor.size, 1);
    secondary_color_texture_ = target.GetRenderTargetTexture();

    if (!secondary_color_texture_) {
      return nullptr;
    }
  }

  // If the color0 resolve texture is the same as the texture, then we're
  // running on the GLES backend with implicit resolve.
  if (supports_implicit_msaa_) {
    auto new_secondary = color0.resolve_texture;
    color0.resolve_texture = secondary_color_texture_;
    color0.texture = secondary_color_texture_;
    secondary_color_texture_ = new_secondary;
  } else {
    std::swap(color0.resolve_texture, secondary_color_texture_);
  }

  target_.SetColorAttachment(color0, 0);

  // Return the previous backdrop texture, which is safe to read in the next
  // render pass that attaches `target_`.
  return secondary_color_texture_;
}

RenderTarget& EntityPassTarget::GetRenderTarget() {
  if (pending_target_) {
    const auto pending = pending_target_.value();
    pending_target_.reset();
    target_ = CreateRenderTarget(*pending.renderer, pending.size,
                                 pending.clear_color, pending.format);
  }
  return target_;
}

ISize EntityPassTarget::GetRenderTargetSize() const {
  return pending_target_ ? pending_target_->size
                         : target_.GetRenderTargetSize();
}

PixelFormat EntityPassTarget::GetColorFormat() const {
  return pending_target_
             ? pending_target_->format.value_or(
                   pending_target_->renderer->GetDeviceCapabilities()
                       .GetDefaultColorFormat())
             : target_.GetRenderTargetTexture()->GetTextureDescriptor().format;
}

bool EntityPassTarget::IsColorLossless() const {
  if (pending_target_) {
    return pending_target_->format.has_value() ||
           GetColorFormat() == PixelFormat::kR8G8B8A8UNormInt ||
           !pending_target_->renderer->GetDeviceCapabilities()
                .SupportsOffscreenMSAA() ||
           supports_implicit_msaa_;
  }
  return target_.GetRenderTargetTexture()
             ->GetTextureDescriptor()
             .compression_type == CompressionType::kLossless;
}

Color EntityPassTarget::GetClearColor() const {
  return pending_target_ ? pending_target_->clear_color
                         : target_.GetColorAttachment(0).clear_color;
}

void EntityPassTarget::SetClearColor(Color color) {
  if (pending_target_) {
    pending_target_->clear_color = color;
  } else {
    auto attachment = target_.GetColorAttachment(0);
    attachment.clear_color = color;
    target_.SetColorAttachment(attachment, 0);
  }
}

bool EntityPassTarget::IsValid() const {
  return pending_target_.has_value() || target_.IsValid();
}

void EntityPassTarget::RemoveSecondary() {
  secondary_color_texture_ = nullptr;
}

}  // namespace impeller
