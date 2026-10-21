// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
#define FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "flutter/assets/asset_manager.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/ref_ptr.h"
#include "flutter/lib/ui/dart_wrapper.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/font_collection.h"

namespace flutter {

struct FlaxFontData;
class ScopedFontAssetProvider;

class FontCollection : public std::enable_shared_from_this<FontCollection> {
 public:
  FontCollection();

  virtual ~FontCollection();

  std::shared_ptr<txt::FontCollection> GetFontCollection() const;

  void SetupDefaultFontManager(uint32_t font_initialization_data);

  // Virtual for testing.
  virtual void RegisterFonts(
      const std::shared_ptr<AssetManager>& asset_manager);

  void RegisterTestFonts();

  static void LoadFontFromList(Dart_Handle font_data_handle,
                               Dart_Handle callback,
                               const std::string& family_name);

  static Dart_Handle ScopedFontStats();

 private:
  friend class FlaxFontCollection;
  friend class ScopedFontAssetProvider;
  std::shared_ptr<FlaxFontData> AcquireFontData(Dart_Handle bytes,
                                                std::string& error);
  std::mutex font_data_mutex_;
  std::unordered_map<uint64_t, std::vector<std::weak_ptr<FlaxFontData>>>
      font_data_;
  uint64_t public_font_revision_ = 0;
  std::atomic_size_t scoped_collections_{0};
  std::atomic_size_t scoped_families_{0};
  std::shared_ptr<txt::FontCollection> collection_;
  sk_sp<txt::DynamicFontManager> dynamic_font_manager_;

  FML_DISALLOW_COPY_AND_ASSIGN(FontCollection);
};

// Private font names and caches; public host fonts remain available as
// fallback.
class FlaxFontCollection : public RefCountedDartWrappable<FlaxFontCollection> {
  DEFINE_WRAPPERTYPEINFO();
  FML_FRIEND_MAKE_REF_COUNTED(FlaxFontCollection);

 public:
  static void Create(Dart_Handle wrapper);
  ~FlaxFontCollection() override;
  bool addFont(Dart_Handle bytes, const std::string& family);
  void dispose();
  std::shared_ptr<txt::FontCollection> GetFontCollection();

 private:
  uint64_t public_font_revision_ = 0;
  explicit FlaxFontCollection(std::shared_ptr<FontCollection> parent);
  std::shared_ptr<FontCollection> parent_;
  std::shared_ptr<txt::FontCollection> collection_;
  ScopedFontAssetProvider* provider_ = nullptr;
};

}  // namespace flutter

#endif  // FLUTTER_LIB_UI_TEXT_FONT_COLLECTION_H_
