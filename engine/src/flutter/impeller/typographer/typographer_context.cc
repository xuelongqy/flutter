// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/typographer/typographer_context.h"

#include <atomic>
#include <utility>

namespace impeller {

namespace {
std::atomic_size_t font_cache_generation{0};
}

void TypographerContext::InvalidateFontCaches() {
  font_cache_generation.fetch_add(1, std::memory_order_relaxed);
}

size_t TypographerContext::GetFontCacheGeneration() {
  return font_cache_generation.load(std::memory_order_relaxed);
}

TypographerContext::TypographerContext() {
  is_valid_ = true;
}

TypographerContext::~TypographerContext() = default;

bool TypographerContext::IsValid() const {
  return is_valid_;
}

}  // namespace impeller
