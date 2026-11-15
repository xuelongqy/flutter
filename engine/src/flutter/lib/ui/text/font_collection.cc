// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/text/font_collection.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string_view>

#include "flutter/lib/ui/text/asset_manager_font_provider.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/runtime/test_font_data.h"
#include "impeller/typographer/typographer_context.h"
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontMgr.h"
#include "third_party/skia/include/core/SkGraphics.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_library_natives.h"
#include "third_party/tonic/logging/dart_invoke.h"
#include "third_party/tonic/typed_data/typed_list.h"
#include "txt/asset_font_manager.h"
#include "txt/platform.h"
#include "txt/test_font_manager.h"

#if FML_OS_MACOSX || FML_OS_IOS
#include "txt/platform_mac.h"
#endif

namespace flutter {

struct FlaxFontData {
  uint64_t key;
  sk_sp<SkData> bytes;
  sk_sp<SkTypeface> typeface;
};

// Keeping sources in the provider also retains them for live Paragraphs.
class ScopedFontAssetProvider : public txt::TypefaceFontAssetProvider {
 public:
  explicit ScopedFontAssetProvider(std::shared_ptr<FontCollection> parent)
      : parent_(std::move(parent)) {
    parent_->scoped_collections_++;
  }

  ~ScopedFontAssetProvider() override {
    parent_->scoped_families_ -= families_.size();
    parent_->scoped_collections_--;
    bool retired_source = false;
    // Drop expired keys as owners retire, even if nobody requests diagnostics.
    {
      std::lock_guard<std::mutex> lock(parent_->font_data_mutex_);
      for (auto& [family, faces] : families_) {
        for (auto& source : faces) {
          const auto key = source->key;
          source.reset();
          auto found = parent_->font_data_.find(key);
          if (found != parent_->font_data_.end()) {
            retired_source |=
                std::erase_if(found->second, [](const auto& entry) {
                  return entry.expired();
                }) > 0;
            if (found->second.empty())
              parent_->font_data_.erase(found);
          }
        }
      }
    }
    if (retired_source) {
      impeller::TypographerContext::InvalidateFontCaches();
    }
  }

  bool Add(const std::string& family, std::shared_ptr<FlaxFontData> source) {
    const auto key = CanonicalFamilyName(family);
    auto& faces = families_[key];
    if (std::find(faces.begin(), faces.end(), source) != faces.end()) {
      return false;
    }
    if (faces.empty()) {
      parent_->scoped_families_++;
    }
    RegisterTypeface(source->typeface, family);
    faces.push_back(std::move(source));
    return true;
  }

  sk_sp<SkFontStyleSet> MatchFamily(const std::string& family) override {
    auto match = txt::TypefaceFontAssetProvider::MatchFamily(family);
    return match ? match
                 : parent_->dynamic_font_manager_->font_provider().MatchFamily(
                       family);
  }

 private:
  std::shared_ptr<FontCollection> parent_;
  std::unordered_map<std::string, std::vector<std::shared_ptr<FlaxFontData>>>
      families_;
};

std::shared_ptr<FlaxFontData> FontCollection::AcquireFontData(
    Dart_Handle bytes,
    std::string& error) {
  tonic::Uint8List input(bytes);
  const auto length = static_cast<size_t>(input.num_elements());
  const auto key = std::hash<std::string_view>{}(
      std::string_view(reinterpret_cast<const char*>(input.data()), length));
  std::lock_guard<std::mutex> lock(font_data_mutex_);
  auto& bucket = font_data_[key];
  for (const auto& entry : bucket) {
    if (auto source = entry.lock()) {
      // The checksum selects a bucket; exact bytes establish identity.
      if (source->bytes->size() == length &&
          std::memcmp(source->bytes->data(), input.data(), length) == 0) {
        return source;
      }
    }
  }
  auto data = SkData::MakeWithCopy(input.data(), input.num_elements());
  input.Release();
  auto typeface = txt::GetDefaultFontManager()->makeFromData(data);
  if (!typeface) {
    if (bucket.empty())
      font_data_.erase(key);
    error = "Invalid module font data";
    return nullptr;
  }
  auto source = std::make_shared<FlaxFontData>();
  source->key = key;
  source->bytes = std::move(data);
  source->typeface = std::move(typeface);
  bucket.push_back(source);
  return source;
}

IMPLEMENT_WRAPPERTYPEINFO(ui, FlaxFontCollection);

void FlaxFontCollection::Create(Dart_Handle wrapper) {
  UIDartState::ThrowIfUIOperationsProhibited();
  auto parent = UIDartState::Current()
                    ->platform_configuration()
                    ->client()
                    ->GetFontCollection()
                    .shared_from_this();
  auto result = fml::MakeRefCounted<FlaxFontCollection>(std::move(parent));
  result->AssociateWithDartWrapper(wrapper);
}

FlaxFontCollection::FlaxFontCollection(std::shared_ptr<FontCollection> parent)
    : parent_(std::move(parent)) {
  public_font_revision_ = parent_->public_font_revision_;
  auto provider = std::make_unique<ScopedFontAssetProvider>(parent_);
  provider_ = provider.get();
  collection_ = parent_->collection_->CreateScopedCollection(
      sk_make_sp<txt::AssetFontManager>(std::move(provider)));
}

FlaxFontCollection::~FlaxFontCollection() = default;

bool FlaxFontCollection::addFont(Dart_Handle bytes, const std::string& family) {
  UIDartState::ThrowIfUIOperationsProhibited();
  if (family.find('\0') != std::string::npos) {
    Dart_ThrowException(tonic::ToDart("Invalid font family"));
    return false;
  }
  std::string error;
  auto source = parent_->AcquireFontData(bytes, error);
  if (!source) {
    Dart_ThrowException(tonic::ToDart(error));
    return false;
  }
  SkString intrinsic_family;
  if (family.empty())
    source->typeface->getFamilyName(&intrinsic_family);
  const auto added = provider_->Add(
      family.empty() ? intrinsic_family.c_str() : family, std::move(source));
  if (added) {
    collection_->ClearFontFamilyCache();
  }
  return added;
}

std::shared_ptr<txt::FontCollection> FlaxFontCollection::GetFontCollection() {
  if (public_font_revision_ != parent_->public_font_revision_) {
    collection_->RefreshPublicFonts(*parent_->collection_);
    public_font_revision_ = parent_->public_font_revision_;
  }
  return collection_;
}

void FlaxFontCollection::dispose() {
  collection_.reset();
  provider_ = nullptr;
  parent_.reset();
  ClearDartWrapper();
}

Dart_Handle FontCollection::ScopedFontStats() {
  UIDartState::ThrowIfUIOperationsProhibited();
  auto& fonts = UIDartState::Current()
                    ->platform_configuration()
                    ->client()
                    ->GetFontCollection();
  std::lock_guard<std::mutex> lock(fonts.font_data_mutex_);
  int64_t sources = 0;
  int64_t bytes = 0;
  for (auto it = fonts.font_data_.begin(); it != fonts.font_data_.end();) {
    for (const auto& entry : it->second) {
      if (auto source = entry.lock()) {
        sources++;
        bytes += source->bytes->size();
      }
    }
    std::erase_if(it->second,
                  [](const auto& entry) { return entry.expired(); });
    if (it->second.empty())
      it = fonts.font_data_.erase(it);
    else
      ++it;
  }

  return tonic::ToDart(std::vector<int64_t>{
      static_cast<int64_t>(fonts.scoped_collections_.load()),
      static_cast<int64_t>(fonts.scoped_families_.load()), sources, bytes});
}

FontCollection::FontCollection()
    : collection_(std::make_shared<txt::FontCollection>()) {
  dynamic_font_manager_ = sk_make_sp<txt::DynamicFontManager>();
  collection_->SetDynamicFontManager(dynamic_font_manager_);
}

FontCollection::~FontCollection() {
  collection_.reset();
  SkGraphics::PurgeFontCache();
}

std::shared_ptr<txt::FontCollection> FontCollection::GetFontCollection() const {
  return collection_;
}

void FontCollection::SetupDefaultFontManager(
    uint32_t font_initialization_data) {
  collection_->SetupDefaultFontManager(font_initialization_data);
  public_font_revision_++;
}

// Font manifest yaml format:
//
// flutter:
//   fonts:
//    - family: Raleway
//      fonts:
//        - asset: fonts/Raleway-Regular.ttf
//        - asset: fonts/Raleway-Italic.ttf
//          style: italic
//    - family: RobotoMono
//      fonts:
//        - asset: fonts/RobotoMono-Regular.ttf
//        - asset: fonts/RobotoMono-Bold.ttf
//          weight: 700
//
// Structure described in https://docs.flutter.dev/cookbook/design/fonts
void FontCollection::RegisterFonts(
    const std::shared_ptr<AssetManager>& asset_manager) {
  public_font_revision_++;
#if FML_OS_MACOSX || FML_OS_IOS
  RegisterSystemFonts(*dynamic_font_manager_);
#endif
  std::unique_ptr<fml::Mapping> manifest_mapping =
      asset_manager->GetAsMapping("FontManifest.json");
  if (manifest_mapping == nullptr) {
    FML_DLOG(WARNING) << "Could not find the font manifest in the asset store.";
    return;
  }

  rapidjson::Document document;
  static_assert(sizeof(decltype(document)::Ch) == sizeof(uint8_t), "");
  document.Parse(reinterpret_cast<const decltype(document)::Ch*>(
                     manifest_mapping->GetMapping()),
                 manifest_mapping->GetSize());

  if (document.HasParseError()) {
    FML_DLOG(WARNING) << "Error parsing the font manifest in the asset store.";
    return;
  }

  if (!document.IsArray()) {
    return;
  }

  auto font_provider =
      std::make_unique<AssetManagerFontProvider>(asset_manager);

  for (const auto& family : document.GetArray()) {
    auto family_name = family.FindMember("family");
    if (family_name == family.MemberEnd() || !family_name->value.IsString()) {
      continue;
    }

    auto family_fonts = family.FindMember("fonts");
    if (family_fonts == family.MemberEnd() || !family_fonts->value.IsArray()) {
      continue;
    }

    for (const auto& family_font : family_fonts->value.GetArray()) {
      if (!family_font.IsObject()) {
        continue;
      }

      auto font_asset = family_font.FindMember("asset");
      if (font_asset == family_font.MemberEnd() ||
          !font_asset->value.IsString()) {
        continue;
      }

      // TODO(chinmaygarde): Handle weights and styles.
      font_provider->RegisterAsset(family_name->value.GetString(),
                                   font_asset->value.GetString());
    }
  }

  collection_->SetAssetFontManager(
      sk_make_sp<txt::AssetFontManager>(std::move(font_provider)));
}

void FontCollection::RegisterTestFonts() {
  public_font_revision_++;
  std::vector<sk_sp<SkTypeface>> test_typefaces = GetTestFontData();
  std::unique_ptr<txt::TypefaceFontAssetProvider> font_provider =
      std::make_unique<txt::TypefaceFontAssetProvider>();

  size_t index = 0;
  std::vector<std::string> names = GetTestFontFamilyNames();
  for (sk_sp<SkTypeface> typeface : test_typefaces) {
    if (typeface) {
      font_provider->RegisterTypeface(std::move(typeface), names[index]);
    }
    index++;
  }

  collection_->SetTestFontManager(
      sk_make_sp<txt::TestFontManager>(std::move(font_provider), names));

  collection_->DisableFontFallback();
}

void FontCollection::LoadFontFromList(Dart_Handle font_data_handle,
                                      Dart_Handle callback,
                                      const std::string& family_name) {
  UIDartState::ThrowIfUIOperationsProhibited();
  tonic::Uint8List font_data(font_data_handle);
  FontCollection& font_collection = UIDartState::Current()
                                        ->platform_configuration()
                                        ->client()
                                        ->GetFontCollection();

  std::unique_ptr<SkStreamAsset> font_stream = std::make_unique<SkMemoryStream>(
      font_data.data(), font_data.num_elements(), true);
  sk_sp<SkFontMgr> font_mgr = txt::GetDefaultFontManager();
  sk_sp<SkTypeface> typeface = font_mgr->makeFromStream(std::move(font_stream));
  txt::TypefaceFontAssetProvider& font_provider =
      font_collection.dynamic_font_manager_->font_provider();
  if (family_name.empty()) {
    font_provider.RegisterTypeface(typeface);
  } else {
    font_provider.RegisterTypeface(typeface, family_name);
  }
  font_collection.public_font_revision_++;
  font_collection.collection_->ClearFontFamilyCache();

  font_data.Release();
  tonic::DartInvoke(callback, {tonic::ToDart(0)});
}

}  // namespace flutter
