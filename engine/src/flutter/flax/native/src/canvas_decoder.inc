// Canvas protocol v1. Each commit owns its output and publishes state
// atomically.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include "../include/flax/canvas.h"
#include "flutter/display_list/effects/color_sources/dl_conical_gradient_color_source.h"
#include "flutter/display_list/effects/color_sources/dl_image_color_source.h"
#include "flutter/display_list/effects/color_sources/dl_linear_gradient_color_source.h"
#include "flutter/display_list/effects/color_sources/dl_sweep_gradient_color_source.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/geometry/dl_geometry_conversions.h"
#include "flutter/display_list/image/dl_image_skia.h"
#include "flutter/display_list/skia/dl_sk_conversions.h"
#include "flutter/fml/synchronization/waitable_event.h"
#include "flutter/lib/ui/painting/display_list_image_gpu.h"
#if !SLIMPELLER
#include "flutter/lib/ui/painting/display_list_deferred_image_gpu_skia.h"
#endif
#include "flutter/lib/ui/painting/image_shader.h"
#include "flutter/lib/ui/painting/picture.h"
#include "flutter/lib/ui/text/font_collection.h"
#include "flutter/lib/ui/window/platform_configuration.h"
#include "flutter/txt/src/skia/paragraph_skia.h"
#include "flutter/txt/src/txt/paragraph_builder.h"
#include "third_party/harfbuzz/src/hb-ot.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "third_party/skia/include/codec/SkEncodedOrigin.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/codec/SkPixmapUtils.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "third_party/skia/include/core/SkPathMeasure.h"
#include "third_party/skia/include/core/SkPathUtils.h"
#include "third_party/skia/include/core/SkStream.h"
#include "third_party/skia/include/encode/SkJpegEncoder.h"
#include "third_party/skia/include/encode/SkWebpEncoder.h"
#include "third_party/skia/include/utils/SkParsePath.h"
#include "third_party/skia/modules/skparagraph/include/ParagraphBuilder.h"
#include "third_party/skia/modules/skunicode/include/SkUnicode_icu.h"
#include "third_party/skia/src/core/SkUTF.h"
#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "flutter/lib/ui/painting/display_list_deferred_image_gpu_impeller.h"
#include "flutter/lib/ui/painting/image_encoding_impeller.h"
#include "flutter/lib/ui/painting/pixel_deferred_image_gpu_impeller.h"
#endif

namespace flax::canvas {
using namespace flutter;
constexpr double pi = 3.14159265358979323846;

struct Bytes {
  Dart_Handle handle;
  const uint8_t* data = nullptr;
  intptr_t size = 0;
  explicit Bytes(Dart_Handle value) : handle(value) {
    if (!Dart_IsTypedData(value) ||
        (Dart_GetTypeOfTypedData(value) != Dart_TypedData_kUint8 &&
         Dart_GetTypeOfExternalTypedData(value) != Dart_TypedData_kUint8))
      throw std::runtime_error("Expected canvas Uint8List");
    Dart_TypedData_Type type;
    void* address = nullptr;
    const auto error = Dart_TypedDataAcquireData(value, &type, &address, &size);
    if (Dart_IsError(error))
      throw std::runtime_error("Cannot acquire canvas bytes");
    data = static_cast<const uint8_t*>(address);
  }
  ~Bytes() { Dart_TypedDataReleaseData(handle); }
};

struct Reader {
  const uint8_t* data;
  size_t size;
  size_t offset = 0;
  void check(size_t count) const {
    if (count > size - offset)
      throw std::runtime_error("Truncated canvas command");
  }
  uint16_t u16() {
    check(2);
    const uint16_t n = data[offset] | (uint16_t(data[offset + 1]) << 8);
    offset += 2;
    return n;
  }
  uint32_t u32() {
    const uint32_t lo = u16(), hi = u16();
    return lo | (hi << 16);
  }
  double f64() {
    const uint64_t lo = u32(), hi = u32();
    const uint64_t bits = lo | (hi << 32);
    double n;
    std::memcpy(&n, &bits, sizeof(n));
    if (!std::isfinite(n) || std::abs(n) > std::numeric_limits<float>::max())
      throw std::runtime_error("Invalid canvas number");
    return n;
  }
  std::string text() {
    const auto count = u32();
    check(count);
    const char* start = reinterpret_cast<const char*>(data + offset);
    if (SkUTF::CountUTF8(start, count) < 0)
      throw std::runtime_error("Invalid canvas UTF-8");
    offset += count;
    return std::string(start, count);
  }
  SkPoint point() {
    const float x = f64(), y = f64();
    return {x, y};
  }
  SkRect rect(bool sorted = true) {
    const float x = f64(), y = f64(), w = f64(), h = f64();
    const auto rect = SkRect::MakeXYWH(x, y, w, h);
    return sorted ? rect.makeSorted() : rect;
  }
  SkMatrix matrix() {
    const float a = f64(), b = f64(), c = f64(), d = f64(), e = f64(),
                f = f64();
    return SkMatrix::MakeAll(a, c, e, b, d, f, 0, 0, 1);
  }
  DlColor color() {
    auto channel = [&]() { return float(std::clamp(f64(), 0., 1.)); };
    const auto r = channel(), g = channel(), b = channel(), a = channel();
    return DlColor::RGBA(r, g, b, a);
  }
  void done() const {
    if (offset != size)
      throw std::runtime_error("Invalid canvas payload length");
  }
};

double sweep(double start, double end, bool ccw) {
  if (start == end)
    return 0;
  auto n = std::fmod(ccw ? start - end : end - start, 2 * pi);
  if (n <= 0)
    n += 2 * pi;
  return ccw ? -n : n;
}

// Skia parses atomically; SVG retains complete segments before the first error.
SkPath svg_path(const std::string& text) {
  size_t at = 0, end = 0;
  char command = 0;
  const auto whitespace = [&]() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' ||
                                text[at] == '\n' || text[at] == '\r'))
      ++at;
  };
  const auto digit = [&](size_t i) {
    return i < text.size() && text[i] >= '0' && text[i] <= '9';
  };
  while (at < text.size()) {
    whitespace();
    if (at == text.size())
      break;
    const bool explicit_command =
        std::isalpha(static_cast<unsigned char>(text[at]));
    if (explicit_command)
      command = text[at++];
    const char op = std::toupper(static_cast<unsigned char>(command));
    if (end == 0 && op != 'M')
      break;
    if (op == 'Z') {
      if (!explicit_command)
        break;
      end = at;
      continue;
    }
    const int count = op == 'H' || op == 'V'                ? 1
                      : op == 'M' || op == 'L' || op == 'T' ? 2
                      : op == 'Q' || op == 'S'              ? 4
                      : op == 'C'                           ? 6
                      : op == 'A'                           ? 7
                                                            : 0;
    if (!count)
      break;
    bool valid = true;
    for (int i = 0; i < count; ++i) {
      whitespace();
      if ((i > 0 || !explicit_command) && at < text.size() && text[at] == ',') {
        ++at;
        whitespace();
      }
      if (op == 'A' && (i == 3 || i == 4)) {
        if (at == text.size() || (text[at] != '0' && text[at] != '1')) {
          valid = false;
          break;
        }
        ++at;
        continue;
      }
      const size_t start = at;
      if (at < text.size() && (text[at] == '+' || text[at] == '-'))
        ++at;
      bool digits = false;
      while (digit(at)) {
        digits = true;
        ++at;
      }
      if (at < text.size() && text[at] == '.') {
        ++at;
        while (digit(at)) {
          digits = true;
          ++at;
        }
      }
      if (!digits) {
        valid = false;
        break;
      }
      if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
        ++at;
        if (at < text.size() && (text[at] == '+' || text[at] == '-'))
          ++at;
        if (!digit(at)) {
          valid = false;
          break;
        }
        while (digit(at))
          ++at;
      }
      const double number =
          std::strtod(text.substr(start, at - start).c_str(), nullptr);
      if (!std::isfinite(number) ||
          std::abs(number) > std::numeric_limits<float>::max()) {
        valid = false;
        break;
      }
    }
    if (!valid)
      break;
    end = at;
    if (command == 'M')
      command = 'L';
    if (command == 'm')
      command = 'l';
  }
  const auto parsed = SkParsePath::FromSVGString(text.substr(0, end).c_str());
  return parsed.value_or(SkPath());
}

std::optional<SkPoint> path_end(const SkPath& path) {
  SkPath::RawIter iter(path);
  std::optional<SkPoint> point;
  SkPoint start = {0, 0};
  while (auto record = iter.next()) {
    if (record->fVerb == SkPathVerb::kMove)
      start = record->fPoints[0];
    point =
        record->fVerb == SkPathVerb::kClose ? start : record->fPoints.back();
  }
  return point;
}

SkPath path(Reader& r, unsigned depth = 0) {
  if (depth > 64)
    throw std::runtime_error("Canvas path nesting limit exceeded");
  const auto count = r.u32();
  if (count > (r.size - r.offset) / 2)
    throw std::runtime_error("Invalid path count");
  SkPathBuilder p;
  for (uint32_t i = 0; i < count; i++) {
    switch (r.u16()) {
      case 1:
        p.moveTo(r.point());
        break;
      case 2: {
        const auto point = r.point();
        if (p.getLastPt())
          p.lineTo(point);
        else
          p.moveTo(point);
        break;
      }
      case 3:
        p.close();
        break;
      case 4: {
        const auto rect = r.rect(false);
        if (rect.width() != 0 || rect.height() != 0)
          p.addRect(rect.makeSorted(), (rect.width() < 0) != (rect.height() < 0)
                                           ? SkPathDirection::kCCW
                                           : SkPathDirection::kCW);
        p.moveTo(rect.fLeft, rect.fTop);
        break;
      }
      case 5: {
        const auto a = r.point(), b = r.point(), c = r.point();
        if (!p.getLastPt())
          p.moveTo(a);
        p.cubicTo(a, b, c);
        break;
      }
      case 6: {
        const auto a = r.point(), b = r.point();
        if (!p.getLastPt())
          p.moveTo(a);
        p.quadTo(a, b);
        break;
      }
      case 7:
      case 8: {
        // Both arc encodings end with start, end, counterclockwise.
        const auto opcode = uint16_t(r.data[r.offset - 2]);
        const auto center = r.point();
        const double rx = r.f64(), ry = opcode == 7 ? rx : r.f64();
        const double rotation = opcode == 7 ? 0 : r.f64();
        const double start = r.f64(), end = r.f64();
        const double delta = sweep(start, end, r.f64() != 0);
        if (rotation == 0) {
          const auto oval = SkRect::MakeLTRB(center.x() - rx, center.y() - ry,
                                             center.x() + rx, center.y() + ry);
          if (std::abs(delta) >= 2 * pi - 1e-12) {
            p.arcTo(oval, start * 180 / pi, delta * 90 / pi, false);
            p.arcTo(oval, (start + delta / 2) * 180 / pi, delta * 90 / pi,
                    false);
          } else
            p.arcTo(oval, start * 180 / pi, delta * 180 / pi, false);
        } else {
          auto point = [&](double angle) {
            const auto x = rx * std::cos(angle), y = ry * std::sin(angle);
            return SkPoint::Make(
                center.x() + x * std::cos(rotation) - y * std::sin(rotation),
                center.y() + x * std::sin(rotation) + y * std::cos(rotation));
          };
          if (p.getLastPt())
            p.lineTo(point(start));
          else
            p.moveTo(point(start));
          auto arc = [&](double from, double to) {
            p.arcTo({float(std::abs(rx)), float(std::abs(ry))},
                    rotation * 180 / pi,
                    std::abs(to - from) > pi
                        ? SkPathBuilder::ArcSize::kLarge_ArcSize
                        : SkPathBuilder::ArcSize::kSmall_ArcSize,
                    to > from ? SkPathDirection::kCW : SkPathDirection::kCCW,
                    point(to));
          };
          if (std::abs(delta) >= 2 * pi - 1e-12) {
            arc(start, start + delta / 2);
            arc(start + delta / 2, start + delta);
          } else if (delta != 0)
            arc(start, start + delta);
        }
        break;
      }
      case 9: {
        const auto p2 = r.point();
        const float radius = r.f64();
        const auto p1 = r.point();
        if (radius < 0)
          throw std::runtime_error("Negative arc radius");
        if (!p.getLastPt()) {
          p.moveTo(p1);
          break;
        }
        auto current = *p.getLastPt();
        const auto shape = p.snapshot();
        if (shape.isLastContourClosed()) {
          SkPathMeasure metric(shape, false);
          do {
            (void)metric.getPosTan(metric.getLength(), &current, nullptr);
          } while (metric.nextContour());
        }
        const double x1 = current.x() - p1.x(), y1 = current.y() - p1.y();
        const double x2 = p2.x() - p1.x(), y2 = p2.y() - p1.y();
        const double len1 = std::hypot(x1, y1), len2 = std::hypot(x2, y2);
        if (radius == 0 || len1 == 0 || len2 == 0) {
          p.lineTo(p1);
          break;
        }
        const double ax = x1 / len1, ay = y1 / len1, bx = x2 / len2,
                     by = y2 / len2;
        const double cross = ax * by - ay * bx;
        if (std::abs(cross) < 1e-12) {
          p.lineTo(p1);
          break;
        }
        const double distance =
            radius /
            std::tan(std::acos(std::clamp(ax * bx + ay * by, -1., 1.)) / 2);
        p.lineTo(
            {float(p1.x() + ax * distance), float(p1.y() + ay * distance)});
        // Preserve the original SVG-arc subdivision, including its raster
        // edges.
        p.arcTo({radius, radius}, 0, SkPathBuilder::ArcSize::kSmall_ArcSize,
                cross < 0 ? SkPathDirection::kCW : SkPathDirection::kCCW,
                {float(p1.x() + bx * distance), float(p1.y() + by * distance)});
        break;
      }
      case 10: {
        auto bounds = r.rect(false);
        const SkPoint start = {bounds.fLeft, bounds.fTop};
        const bool flip_x = bounds.width() < 0, flip_y = bounds.height() < 0;
        SkVector radii[4];
        for (auto& radius : radii)
          radius = r.point();
        // Canvas prunes zero-length closed contours; Impeller would stroke a
        // dot.
        if (bounds.width() == 0 && bounds.height() == 0) {
          p.moveTo(start);
          break;
        }
        if (flip_x) {
          std::swap(radii[0], radii[1]);
          std::swap(radii[3], radii[2]);
        }
        if (flip_y) {
          std::swap(radii[0], radii[3]);
          std::swap(radii[1], radii[2]);
        }
        bounds.sort();
        SkRRect rr;
        rr.setRectRadii(bounds, radii);
        p.addRRect(
            rr, flip_x != flip_y ? SkPathDirection::kCCW : SkPathDirection::kCW,
            flip_y ? (flip_x ? 4 : 5) : (flip_x ? 1 : 0));
        p.moveTo(start);
        break;
      }
      case 11: {
        const auto svg = svg_path(r.text());
        p.addPath(svg);
        if (const auto end = path_end(svg))
          p.moveTo(*end);
        break;
      }
      case 12:
      case 13: {
        const bool append = r.data[r.offset - 2] == 12;
        const bool extend =
            r.data[r.offset - 2] == 13 && p.getLastPt().has_value();
        const auto matrix = r.matrix();
        const auto child = path(r, depth + 1);
        p.addPath(
            child, matrix,
            extend ? SkPath::kExtend_AddPathMode : SkPath::kAppend_AddPathMode);
        if (append) {
          if (const auto end = path_end(child))
            p.moveTo(matrix.mapPoint(*end));
        }
        break;
      }
      default:
        throw std::runtime_error("Unknown canvas path opcode");
    }
  }
  return p.detach();
}

struct FilterEffect {
  std::shared_ptr<DlImageFilter> filter;
  DlColor color = DlColor::kTransparent();
  double x = 0, y = 0, sigma = 0;
  bool drop_shadow = false;
};

void append_filter(std::vector<FilterEffect>& effects, FilterEffect effect) {
  if (!effect.drop_shadow && !effect.filter)
    return;
  const auto* blur = effect.filter ? effect.filter->asBlur() : nullptr;
  const auto* previous = effects.empty() || !effects.back().filter
                             ? nullptr
                             : effects.back().filter->asBlur();
  if (blur && previous) {
    // Gaussian kernels compose by variance; one pass avoids approximation
    // drift.
    const auto sigma =
        std::min<double>(std::hypot(blur->sigma_x(), previous->sigma_x()),
                         std::numeric_limits<float>::max());
    effects.back().filter =
        DlImageFilter::MakeBlur(sigma, sigma, DlTileMode::kDecal);
  } else {
    effects.push_back(std::move(effect));
  }
}

struct DrawState {
  SkMatrix transform = SkMatrix::I();
  std::vector<SkPath> clips;
  DlColor fill = DlColor::kBlack(), stroke = DlColor::kBlack();
  std::shared_ptr<DlColorSource> fill_shader, stroke_shader;
  double alpha = 1, width = 1, miter = 10, dash_offset = 0;
  DlBlendMode blend = DlBlendMode::kSrcOver;
  DlStrokeCap cap = DlStrokeCap::kButt;
  DlStrokeJoin join = DlStrokeJoin::kMiter;
  std::vector<float> dash;
  DlColor shadow = DlColor::kTransparent();
  double blur = 0, shadow_x = 0, shadow_y = 0;
  bool smoothing = true;
  unsigned quality = 0;
  std::string font = "10px sans-serif", align = "start",
              baseline = "alphabetic", direction = "inherit";
  double letter = 0, word = 0;
  std::string letter_css, word_css;
  std::string lang = "inherit", kerning = "auto", stretch = "normal",
              caps = "normal", rendering = "auto";
  std::vector<FilterEffect> filters;

  DlImageSampling sampling() const {
    if (!smoothing)
      return DlImageSampling::kNearestNeighbor;
    return quality == 2   ? DlImageSampling::kCubic
           : quality == 1 ? DlImageSampling::kMipmapLinear
                          : DlImageSampling::kLinear;
  }
  DlPaint paint(bool filling, bool image = false) const {
    DlPaint p;
    auto shader = image ? nullptr : filling ? fill_shader : stroke_shader;
    const auto color = image || shader ? DlColor::kWhite()
                       : filling       ? fill
                                       : stroke;
    p.setAntiAlias(true)
        .setColor(color.withAlphaF(color.getAlphaF() * alpha))
        .setDrawStyle(filling ? DlDrawStyle::kFill : DlDrawStyle::kStroke)
        .setStrokeWidth(width)
        .setStrokeMiter(miter)
        .setStrokeCap(cap)
        .setStrokeJoin(join);
    if (!image) {
      if (shader && shader->asImage())
        shader = shader->asImage()->WithSampling(sampling());
      p.setColorSource(shader);
    }
    return p;
  }
};

struct Surface {
  DrawState state;
  std::vector<DrawState> stack;
  double width = 0, height = 0;
  bool alpha = true;
};
struct Host {
  Dart_Isolate owner = Dart_CurrentIsolate();
  uint64_t next = 1;
  std::unordered_map<uint64_t, Surface> surfaces;
  std::unordered_map<uint64_t, sk_sp<DlImage>> images;
  std::shared_ptr<txt::FontCollection> fonts;
};
std::mutex hosts_mutex;
std::unordered_map<uint64_t, std::shared_ptr<Host>> hosts;
uint64_t next_host = 1;

DlBlendMode blend(const std::string& name, DlBlendMode previous) {
  static const std::unordered_map<std::string, DlBlendMode> modes = {
      {"source-over", DlBlendMode::kSrcOver},
      {"clear", DlBlendMode::kClear},
      {"source-in", DlBlendMode::kSrcIn},
      {"source-out", DlBlendMode::kSrcOut},
      {"source-atop", DlBlendMode::kSrcATop},
      {"destination-over", DlBlendMode::kDstOver},
      {"destination-in", DlBlendMode::kDstIn},
      {"destination-out", DlBlendMode::kDstOut},
      {"destination-atop", DlBlendMode::kDstATop},
      {"xor", DlBlendMode::kXor},
      {"copy", DlBlendMode::kSrc},
      {"lighter", DlBlendMode::kPlus},
      {"multiply", DlBlendMode::kMultiply},
      {"screen", DlBlendMode::kScreen},
      {"overlay", DlBlendMode::kOverlay},
      {"darken", DlBlendMode::kDarken},
      {"lighten", DlBlendMode::kLighten},
      {"color-dodge", DlBlendMode::kColorDodge},
      {"color-burn", DlBlendMode::kColorBurn},
      {"hard-light", DlBlendMode::kHardLight},
      {"soft-light", DlBlendMode::kSoftLight},
      {"difference", DlBlendMode::kDifference},
      {"exclusion", DlBlendMode::kExclusion},
      {"hue", DlBlendMode::kHue},
      {"saturation", DlBlendMode::kSaturation},
      {"color", DlBlendMode::kColor},
      {"luminosity", DlBlendMode::kLuminosity}};
  const auto found = modes.find(name);
  return found == modes.end() ? previous : found->second;
}

// Legacy string payloads remain readable; new encoders send validated filter
// steps.
bool filter(DrawState& s, const std::string& input) {
  static const std::regex token(
      R"((blur|brightness|contrast|grayscale|invert|opacity|saturate|sepia)\(\s*([-\d.]+)\s*(px|%)?\s*\)|hue-rotate\(\s*([-\d.]+)\s*deg\s*\))",
      std::regex::icase);
  if (input.empty() || input == "none") {
    s.filters.clear();
    return true;
  }
  std::vector<FilterEffect> effects;
  size_t consumed = 0;
  const auto whitespace = [](const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
  };
  for (auto i = std::sregex_iterator(input.begin(), input.end(), token);
       i != std::sregex_iterator(); ++i) {
    const auto& m = *i;
    if (!whitespace(input.substr(consumed, m.position() - consumed)))
      return false;
    consumed = m.position() + m.length();
    std::string name = m[1];
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    const bool hue = m[4].matched;
    const double value = std::stod(hue ? m[4].str() : m[2].str());
    if (!std::isfinite(value))
      return false;
    const double a = m[3] == "%" ? value / 100 : value;
    if (name == "blur") {
      if (m[3] == "%")
        return false;
      append_filter(
          effects, {DlImageFilter::MakeBlur(value, value, DlTileMode::kDecal)});
      continue;
    }
    std::array<float, 20> n{};
    n[0] = n[6] = n[12] = n[18] = 1;
    if (name == "brightness")
      n[0] = n[6] = n[12] = a;
    else if (name == "contrast") {
      n[0] = n[6] = n[12] = a;
      n[4] = n[9] = n[14] = .5 * (1 - a);
    } else if (name == "opacity")
      n[18] = a;
    else if (name == "invert") {
      n[0] = n[6] = n[12] = 1 - 2 * a;
      n[4] = n[9] = n[14] = a;
    } else if (name == "grayscale" || name == "saturate") {
      const double t = name == "grayscale" ? 1 - a : a;
      const double r = name == "grayscale" ? .2126 : .213;
      const double g = name == "grayscale" ? .7152 : .715;
      const double b = name == "grayscale" ? .0722 : .072;
      for (int row = 0; row < 3; row++) {
        n[row * 5] = r * (1 - t);
        n[row * 5 + 1] = g * (1 - t);
        n[row * 5 + 2] = b * (1 - t);
        n[row * 5 + row] += t;
      }
    } else if (name == "sepia") {
      const float sepia[9] = {.393, .769, .189, .349, .686,
                              .168, .272, .534, .131};
      for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
          n[row * 5 + col] =
              sepia[row * 3 + col] * a + (row == col ? 1 - a : 0);
    } else if (hue) {
      const double c = std::cos(value * pi / 180),
                   t = std::sin(value * pi / 180);
      n = {float(.213 + c * .787 - t * .213),
           float(.715 - c * .715 - t * .715),
           float(.072 - c * .072 + t * .928),
           0,
           0,
           float(.213 - c * .213 + t * .143),
           float(.715 + c * .285 + t * .140),
           float(.072 - c * .072 - t * .283),
           0,
           0,
           float(.213 - c * .213 - t * .787),
           float(.715 - c * .715 + t * .715),
           float(.072 + c * .928 + t * .072),
           0,
           0,
           0,
           0,
           0,
           1,
           0};
    }
    append_filter(
        effects,
        {DlImageFilter::MakeColorFilter(DlColorFilter::MakeMatrix(n.data()))});
  }
  if (consumed == 0 || !whitespace(input.substr(consumed)))
    return false;
  s.filters = std::move(effects);
  return true;
}

DlPaint text_transformed_paint(DlPaint paint, const SkMatrix& transform) {
  // Keep gradient/pattern coordinates fixed while transforming the glyphs.
  SkMatrix inverted;
  if (!transform.invert(&inverted))
    return paint;
  const auto inverse = ToDlMatrix(inverted);
  const auto* source = paint.getColorSourcePtr();
  if (const auto* g = source->asLinearGradient()) {
    const auto matrix = inverse * g->matrix();
    paint.setColorSource(DlColorSource::MakeLinear(
        g->start_point(), g->end_point(), g->stop_count(), g->colors(),
        g->stops(), g->tile_mode(), &matrix));
  } else if (const auto* g = source->asConicalGradient()) {
    const auto matrix = inverse * g->matrix();
    paint.setColorSource(DlColorSource::MakeConical(
        g->start_center(), g->start_radius(), g->end_center(), g->end_radius(),
        g->stop_count(), g->colors(), g->stops(), g->tile_mode(), &matrix));
  } else if (const auto* g = source->asSweepGradient()) {
    const auto matrix = inverse * g->matrix();
    paint.setColorSource(DlColorSource::MakeSweep(
        g->center(), g->start(), g->end(), g->stop_count(), g->colors(),
        g->stops(), g->tile_mode(), &matrix));
  } else if (const auto* g = source->asImage()) {
    const auto matrix = inverse * g->matrix();
    paint.setColorSource(DlColorSource::MakeImage(
        g->image(), g->horizontal_tile_mode(), g->vertical_tile_mode(),
        g->sampling(), &matrix));
  }
  return paint;
}

struct TextBounds {
  SkRect ink = SkRect::MakeEmpty();
  double ascent = 0, descent = 0, em_ascent = 0, em_descent = 0;
  double hanging = 0, ideographic = 0;
  double skew = 0;
};

void font_metrics(const SkFont& font, TextBounds& bounds) {
  SkFontMetrics fm;
  font.getMetrics(&fm);
  bounds.ascent = -fm.fAscent;
  bounds.descent = fm.fDescent;
  const auto height = bounds.ascent + bounds.descent;
  bounds.em_ascent =
      height > 0 ? font.getSize() * bounds.ascent / height : font.getSize();
  bounds.em_descent = font.getSize() - bounds.em_ascent;
  bounds.hanging = bounds.ascent * .8;
  bounds.ideographic = -bounds.descent;
  auto typeface = font.refTypeface();
  if (!typeface)
    return;
  auto face = hb_face_create_for_tables(
      [](hb_face_t*, hb_tag_t tag, void* data) {
        auto table = static_cast<SkTypeface*>(data)->copyTableData(tag);
        if (!table)
          return hb_blob_get_empty();
        auto* owner = table.release();
        return hb_blob_create(
            static_cast<const char*>(owner->data()), owner->size(),
            HB_MEMORY_MODE_READONLY, owner,
            [](void* data) { static_cast<SkData*>(data)->unref(); });
      },
      typeface.get(), nullptr);
  auto hbfont = hb_font_create(face);
  hb_ot_font_set_funcs(hbfont);
  const auto upem = hb_face_get_upem(face);
  hb_font_set_scale(hbfont, upem, upem);
  const auto baseline = [&](hb_ot_layout_baseline_tag_t tag, double fallback) {
    hb_position_t value;
    return hb_ot_layout_get_baseline(hbfont, tag, HB_DIRECTION_LTR,
                                     HB_TAG('D', 'F', 'L', 'T'), HB_TAG_NONE,
                                     &value) ||
                   hb_ot_layout_get_baseline(hbfont, tag, HB_DIRECTION_LTR,
                                             HB_TAG('l', 'a', 't', 'n'),
                                             HB_TAG_NONE, &value)
               ? double(value) * font.getSize() / upem
               : fallback;
  };
  bounds.hanging = baseline(HB_OT_LAYOUT_BASELINE_TAG_HANGING, bounds.hanging);
  bounds.ideographic = baseline(
      HB_OT_LAYOUT_BASELINE_TAG_IDEO_EMBOX_BOTTOM_OR_LEFT, bounds.ideographic);
  hb_font_destroy(hbfont);
  hb_face_destroy(face);
}

double spacing(const std::string& css, double fallback, const SkFont& font) {
  if (css.empty())
    return fallback;
  static const std::regex relative(R"(([+-]?[0-9.eE+-]+)(em|ex|ch|ic))");
  std::smatch match;
  if (!std::regex_match(css, match, relative))
    return fallback;
  const double value = std::stod(match[1]);
  const auto checked = [](double pixels) {
    if (!std::isfinite(pixels) ||
        std::abs(pixels) > std::numeric_limits<float>::max())
      throw std::runtime_error("Canvas spacing overflow");
    return pixels;
  };
  const auto unit = match[2].str();
  if (unit == "em")
    return checked(value * font.getSize());
  SkFontMetrics fm;
  font.getMetrics(&fm);
  if (unit == "ex")
    return checked(value *
                   (fm.fXHeight > 0 ? fm.fXHeight : font.getSize() / 2));
  const bool zero = unit == "ch";
  if (font.unicharToGlyph(zero ? '0' : 0x6c34) == 0)
    return checked(value * font.getSize() * (zero ? .5 : 1));
  const char* glyph = zero ? "0" : "水";
  return checked(value * font.measureText(glyph, std::strlen(glyph),
                                          SkTextEncoding::kUTF8));
}

std::unique_ptr<txt::Paragraph> paragraph(
    const DrawState& s,
    const std::shared_ptr<txt::FontCollection>& fonts,
    std::string text,
    const DlPaint& paint,
    TextBounds* bounds = nullptr,
    bool ink_bounds = false) {
  namespace skt = skia::textlayout;
  skt::TextStyle style;
  std::smatch font;
  static const std::regex shorthand(
      R"((.*?)\b([0-9]*\.?[0-9]+(?:[eE][+-]?[0-9]+)?)px(?:/[^\s]+)?\s+(.+))");
  if (!std::regex_match(s.font, font, shorthand))
    throw std::runtime_error("Invalid canvas font");
  const double size = std::stod(font[2]);
  if (!std::isfinite(size) || size < 0 ||
      size > std::numeric_limits<float>::max())
    throw std::runtime_error("Invalid canvas font size");
  double weight = 400;
  std::optional<double> oblique;
  auto slant = SkFontStyle::kUpright_Slant;
  const std::vector<std::string> stretches = {
      "ultra-condensed", "extra-condensed", "condensed",
      "semi-condensed",  "normal",          "semi-expanded",
      "expanded",        "extra-expanded",  "ultra-expanded"};
  auto stretch = s.stretch;
  std::string caps = s.caps;
  std::istringstream prefix(font[1]);
  std::string token;
  while (prefix >> token) {
    if (token == "italic")
      slant = SkFontStyle::kItalic_Slant;
    else if (token == "oblique")
      oblique = 14;
    else if (oblique && token.size() > 3 &&
             token.substr(token.size() - 3) == "deg") {
      *oblique = std::stod(token);
      if (!std::isfinite(*oblique) || std::abs(*oblique) > 90)
        throw std::runtime_error("Invalid canvas oblique angle");
    } else if (token == "bold" || token == "bolder")
      weight = 700;
    else if (token == "lighter")
      weight = 100;
    else if (std::regex_match(token, std::regex("[0-9]+(?:\\.[0-9]+)?"))) {
      weight = std::stod(token);
      if (weight < 1 || weight > 1000)
        throw std::runtime_error("Invalid canvas font weight");
    } else if (token == "small-caps" && caps == "normal")
      caps = token;
    else if (std::find(stretches.begin(), stretches.end(), token) !=
                 stretches.end() &&
             stretch == "normal")
      stretch = token;
  }
  const auto width = std::find(stretches.begin(), stretches.end(), stretch);
  if (width == stretches.end())
    throw std::runtime_error("Invalid canvas font stretch");
  style.setFontStyle(SkFontStyle(int(std::round(weight)),
                                 1 + int(width - stretches.begin()), slant));
  std::vector<SkString> families;
  std::string family;
  char quote = 0;
  bool escaped = false;
  bool quoted_family = false;
  const auto add_family = [&]() {
    const auto first = quoted_family ? 0 : family.find_first_not_of(" \t");
    if (first != std::string::npos) {
      const auto length = quoted_family
                              ? family.size()
                              : family.find_last_not_of(" \t") - first + 1;
      families.emplace_back(family.substr(first, length).c_str());
    }
    family.clear();
    quoted_family = false;
  };
  const auto family_text = font[3].str();
  for (size_t i = 0; i < family_text.size(); ++i) {
    const auto c = family_text[i];
    if (quote) {
      if (escaped) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
          size_t end = i;
          while (end < family_text.size() && end < i + 6 &&
                 std::isxdigit(static_cast<unsigned char>(family_text[end])))
            ++end;
          const auto code =
              std::stoul(family_text.substr(i, end - i), nullptr, 16);
          char bytes[4];
          const auto count = SkUTF::ToUTF8(code, bytes);
          if (count > 0)
            family.append(bytes, count);
          i = end - 1;
          if (end < family_text.size() && family_text[end] == ' ')
            ++i;
        } else
          family += c;
        escaped = false;
      } else if (c == '\\')
        escaped = true;
      else if (c == quote)
        quote = 0;
      else
        family += c;
    } else if (c == '\'' || c == '"') {
      quote = c;
      family.clear();
      quoted_family = true;
    } else if (c == ',')
      add_family();
    else if (!quoted_family)
      family += c;
  }
  add_family();
  auto collection = fonts->CreateSktFontCollection();
  SkFontArguments::VariationPosition::Coordinate axes[] = {
      {SkSetFourByteTag('w', 'g', 'h', 't'), float(weight)},
      {SkSetFourByteTag('s', 'l', 'n', 't'), float(-oblique.value_or(0))}};
  style.setFontArguments(
      SkFontArguments().setVariationDesignPosition({axes, oblique ? 2 : 1}));
  const auto faces = collection->findTypefaces(families, style.getFontStyle(),
                                               style.getFontArguments());
  auto face = faces.empty() ? collection->defaultFallback() : faces.front();
  double skew = 0;
  if (oblique) {
    std::vector<SkFontParameters::Variation::Axis> variation(
        face ? std::max(0, face->getVariationDesignParameters({})) : 0);
    if (face)
      face->getVariationDesignParameters(variation);
    const bool variable_slant =
        std::any_of(variation.begin(), variation.end(), [](const auto& axis) {
          return axis.tag == SkSetFourByteTag('s', 'l', 'n', 't');
        });
    if (!variable_slant)
      skew = -std::tan(*oblique * pi / 180);
  }
  SkFont primary(face, size);
  if (bounds) {
    font_metrics(primary, *bounds);
    bounds->skew = skew;
  }
  style.setFontFamilies(families);
  style.setFontSize(size);
  style.setLetterSpacing(spacing(s.letter_css, s.letter, primary));
  style.setWordSpacing(spacing(s.word_css, s.word, primary));
  style.setForegroundPaintID(0);
  style.setLocale(SkString(s.lang == "inherit" ? "" : s.lang.c_str()));
  if (s.kerning != "auto")
    style.addFontFeature(SkString("kern"), s.kerning == "normal");
  else if (s.rendering == "optimizeSpeed")
    style.addFontFeature(SkString("kern"), 0);
  if (s.rendering == "optimizeSpeed")
    style.addFontFeature(SkString("liga"), 0);
  if (caps == "small-caps" || caps == "all-small-caps")
    style.addFontFeature(SkString("smcp"), 1);
  if (caps == "all-small-caps")
    style.addFontFeature(SkString("c2sc"), 1);
  if (caps == "petite-caps" || caps == "all-petite-caps")
    style.addFontFeature(SkString("pcap"), 1);
  if (caps == "all-petite-caps")
    style.addFontFeature(SkString("c2pc"), 1);
  if (caps == "unicase")
    style.addFontFeature(SkString("unic"), 1);
  if (caps == "titling-caps")
    style.addFontFeature(SkString("titl"), 1);
  skt::ParagraphStyle ps;
  ps.setFakeMissingFontStyles(true);
  ps.setTextStyle(style);
  ps.setTextDirection(s.direction == "rtl" ? skt::TextDirection::kRtl
                                           : skt::TextDirection::kLtr);
  ps.setTextAlign(skt::TextAlign::kLeft);
  if (s.rendering != "optimizeSpeed")
    ps.turnHintingOff();
  ps.setApplyRoundingHack(false);
  ps.setReplaceTabCharacters(true);
  auto builder =
      skt::ParagraphBuilder::make(ps, collection, SkUnicodes::ICU::Make());
  builder->pushStyle(style);
  // Canvas text is one line; ASCII whitespace has the width of a space.
  for (auto& c : text)
    if (c == '\t' || c == '\n' || c == '\r' || c == '\f')
      c = ' ';
  builder->addText(text.c_str(), text.size());
  auto raw = builder->Build();
  raw->layout(std::numeric_limits<double>::infinity());
  raw->layout(raw->getMaxIntrinsicWidth());
  if (bounds && ink_bounds) {
    const auto baseline = raw->getAlphabeticBaseline();
    raw->visit([&](int, const skt::Paragraph::VisitorInfo* run) {
      if (!run)
        return;
      std::vector<SkRect> boxes(run->count);
      run->font.getBounds({run->glyphs, size_t(run->count)}, boxes, nullptr);
      for (int i = 0; i < run->count; i++) {
        // Outline bounds preserve fractional sizes that scaler bounds may
        // quantize.
        auto outline = run->font.getPath(run->glyphs[i]);
        const auto box = outline
                             ? outline->makeTransform(SkMatrix::Skew(skew, 0))
                                   .computeTightBounds()
                             : SkMatrix::Skew(skew, 0).mapRect(boxes[i]);
        const auto origin_y = run->origin.y() + run->positions[i].y();
        bounds->ink.join(box.makeOffset(run->origin.x() +
                                            run->positions[i].x() +
                                            skew * (origin_y - baseline),
                                        origin_y));
      }
    });
  }
  return std::make_unique<txt::ParagraphSkia>(
      std::move(raw), std::vector<DlPaint>{paint},
      UIDartState::Current()->IsImpellerEnabled());
}

std::array<double, 2> text_offset(const DrawState& s,
                                  txt::Paragraph& p,
                                  double x,
                                  double y,
                                  const TextBounds& metrics) {
  const auto width = p.GetMaxWidth();
  if (s.align == "center")
    x -= width / 2;
  else if (s.align == "right" || (s.align == "end" && s.direction != "rtl") ||
           (s.align == "start" && s.direction == "rtl"))
    x -= width;
  double baseline = 0;
  if (s.baseline == "top")
    baseline = metrics.em_ascent;
  else if (s.baseline == "middle")
    baseline = (metrics.em_ascent - metrics.em_descent) / 2;
  else if (s.baseline == "hanging")
    baseline = metrics.hanging;
  else if (s.baseline == "ideographic")
    baseline = metrics.ideographic;
  else if (s.baseline == "bottom")
    baseline = -metrics.em_descent;
  y += baseline - p.GetAlphabeticBaseline();
  return {x, y};
}

void apply(DisplayListBuilder& b, const DrawState& s) {
  b.TransformReset();
  for (const auto& clip : s.clips)
    b.ClipPath(DlPath(clip), DlClipOp::kIntersect, true);
  b.Transform(ToDlMatrix(s.transform));
}

std::shared_ptr<DlImageFilter> shadow_filter(DlColor color, double sigma) {
  return DlImageFilter::MakeCompose(
      DlImageFilter::MakeBlur(sigma, sigma, DlTileMode::kDecal),
      DlImageFilter::MakeColorFilter(
          DlColorFilter::MakeBlend(color, DlBlendMode::kSrcIn)));
}

void filtered(DisplayListBuilder& b,
              const sk_sp<DisplayList>& source,
              const std::shared_ptr<DlImageFilter>& filter,
              DlBlendMode blend = DlBlendMode::kSrcOver) {
  DlPaint paint;
  paint.setBlendMode(blend).setImageFilter(filter);
  // SrcOver needs only source coverage; other operators may affect the clip's
  // transparent pixels too.
  const auto bounds = blend == DlBlendMode::kSrcOver
                          ? std::optional<DlRect>(source->GetBounds().Expand(1))
                          : std::nullopt;
  b.SaveLayer(bounds, &paint);
  b.DrawDisplayList(source);
  b.Restore();
}

void draw_shadow(DisplayListBuilder& b,
                 const sk_sp<DisplayList>& source,
                 DlColor color,
                 double sigma,
                 double x,
                 double y) {
  b.Save();
  b.Translate(x, y);
  filtered(b, source, shadow_filter(color, sigma));
  b.Restore();
}

bool unbounded_composite(DlBlendMode blend) {
  return blend == DlBlendMode::kSrc || blend == DlBlendMode::kSrcIn ||
         blend == DlBlendMode::kSrcOut || blend == DlBlendMode::kDstIn ||
         blend == DlBlendMode::kDstATop || blend == DlBlendMode::kClear;
}

template <typename Draw>
void draw(DisplayListBuilder& b,
          const Surface& surface,
          bool fill,
          Draw action,
          bool image = false) {
  const auto& s = surface.state;
  const bool shadow = s.shadow.getAlphaF() != 0 &&
                      (s.blur != 0 || s.shadow_x != 0 || s.shadow_y != 0);
  const bool has_effects = shadow || !s.filters.empty();
  auto paint = s.paint(fill, image);
  const auto opaque = [&]() {
    if (!surface.alpha) {
      b.Save();
      b.TransformReset();
      b.DrawColor(DlColor::kBlack(), DlBlendMode::kDstOver);
      b.Restore();
    }
  };
  if (!has_effects && !unbounded_composite(s.blend)) {
    paint.setBlendMode(s.blend);
    action(b, paint);
    if (s.blend != DlBlendMode::kSrcOver)
      opaque();
    return;
  }
  // The source is un-clipped and in canvas pixels. Effects can bring off-canvas
  // geometry into view, and their distances must not scale with the user CTM.
  DisplayListBuilder source_builder(true);
  source_builder.Transform(ToDlMatrix(s.transform));
  action(source_builder, paint);
  auto source = source_builder.Build();
  if (has_effects) {
    // Effects consume an RGBA8 bitmap. Src prevents layer elision from keeping
    // solid/image opacity in floats while patterns have already quantized it.
    DlPaint bitmap;
    bitmap.setBlendMode(DlBlendMode::kSrc);
    DisplayListBuilder bitmap_builder(true);
    // Bound the destructive layer to source coverage, including antialiasing.
    bitmap_builder.ClipRect(source->GetBounds().Expand(1));
    bitmap_builder.SaveLayer(std::nullopt, &bitmap);
    bitmap_builder.DrawDisplayList(source);
    bitmap_builder.Restore();
    source = bitmap_builder.Build();
  }
  std::shared_ptr<DlImageFilter> chain;
  const auto flush = [&]() {
    if (!chain)
      return;
    DisplayListBuilder effects(true);
    filtered(effects, source, chain);
    source = effects.Build();
    chain.reset();
  };
  for (const auto& effect : s.filters) {
    if (!effect.drop_shadow) {
      chain = DlImageFilter::MakeCompose(effect.filter, chain);
      continue;
    }
    flush();
    DisplayListBuilder effects(true);
    draw_shadow(effects, source, effect.color, effect.sigma, effect.x,
                effect.y);
    filtered(effects, source, nullptr);
    source = effects.Build();
  }
  flush();
  b.Save();
  b.TransformReset();
  b.ClipRect(DlRect::MakeWH(surface.width, surface.height));
  if (shadow) {
    DisplayListBuilder shadow_builder(true);
    draw_shadow(shadow_builder, source, s.shadow, s.blur / 2, s.shadow_x,
                s.shadow_y);
    filtered(b, shadow_builder.Build(), nullptr, s.blend);
    opaque();
  }
  filtered(b, source, nullptr, s.blend);
  opaque();
  b.Restore();
}

SkPath dashed(const DrawState& s, const SkPath& path) {
  if (s.dash.empty())
    return path;
  double total = 0;
  for (const auto n : s.dash)
    total += n;
  if (total == 0)
    return path;
  const auto period = total * (s.dash.size() % 2 ? 2 : 1);
  SkPathMeasure measure(path, false);
  SkPathBuilder result;
  size_t index = 0, segments = 0;
  do {
    double distance = s.dash_offset;
    if (distance < 0)
      distance += std::floor(-distance / period) * period;
    bool drawing = true;
    while (distance < measure.getLength()) {
      if (++segments > 1000000)
        throw std::runtime_error("Canvas dash segment limit exceeded");
      const auto length = s.dash[index++ % s.dash.size()];
      if (drawing)
        measure.getSegment(distance, distance + length, &result, true);
      distance += length;
      drawing = !drawing;
    }
  } while (measure.nextContour());
  return result.detach();
}

std::shared_ptr<DlColorSource> gradient(Reader& r, unsigned type) {
  const auto p0 = r.point();
  SkPoint p1{};
  double r0 = 0, r1 = 0, angle = 0;
  if (type == 0)
    p1 = r.point();
  else if (type == 1) {
    r0 = r.f64();
    p1 = r.point();
    r1 = r.f64();
  } else
    angle = r.f64();
  auto count = r.u32();
  if (count > (r.size - r.offset) / 40)
    throw std::runtime_error("Invalid gradient stops");
  std::vector<std::pair<float, DlColor>> entries;
  for (uint32_t i = 0; i < count; i++) {
    const float stop = std::clamp(r.f64(), 0., 1.);
    entries.emplace_back(stop, r.color());
  }
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<DlColor> colors;
  std::vector<float> stops;
  for (const auto& entry : entries) {
    stops.push_back(entry.first);
    colors.push_back(entry.second);
  }
  if (count < 2) {
    const auto color = count == 0 ? DlColor::kTransparent() : colors.front();
    colors = {color, color};
    stops = {0, 1};
    count = 2;
  }
  const auto matrix = r.offset == r.size ? DlMatrix() : ToDlMatrix(r.matrix());
  if (type == 0)
    return DlColorSource::MakeLinear(ToDlPoint(p0), ToDlPoint(p1), count,
                                     colors.data(), stops.data(),
                                     DlTileMode::kClamp, &matrix);
  if (type == 1)
    return DlColorSource::MakeConical(ToDlPoint(p0), r0, ToDlPoint(p1), r1,
                                      count, colors.data(), stops.data(),
                                      DlTileMode::kClamp, &matrix);
  return DlColorSource::MakeSweep(
      ToDlPoint(p0), angle * 180 / pi, (angle + pi * 2) * 180 / pi, count,
      colors.data(), stops.data(), DlTileMode::kClamp, &matrix);
}

sk_sp<DlImage> pixels(Reader& r, uint32_t w, uint32_t h, bool opaque) {
  if (w == 0 || h == 0 || uint64_t(w) * h > (r.size - r.offset) / 4 ||
      w > INT_MAX / 4 || h > INT_MAX)
    throw std::runtime_error("Invalid canvas pixel dimensions");
  const size_t count = size_t(w) * h * 4;
  auto data = SkData::MakeWithCopy(r.data + r.offset, count);
  r.offset += count;
  if (opaque)
    for (size_t i = 3; i < count; i += 4)
      static_cast<uint8_t*>(data->writable_data())[i] = 255;
  auto* ui = UIDartState::Current();
  // ImageData is straight RGBA; GPU texture uploads require premultiplied RGBA.
  const auto info =
      SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
  auto* rgba = static_cast<uint8_t*>(data->writable_data());
  for (size_t i = 0; i < count; i += 4)
    for (size_t channel = 0; channel < 3; channel++)
      rgba[i + channel] =
          (uint32_t(rgba[i + channel]) * rgba[i + 3] + 127) / 255;
  auto image = SkImages::RasterFromData(info, data, size_t(w) * 4);
  if (!image)
    throw std::runtime_error("Cannot create canvas pixel image");
#if IMPELLER_SUPPORTS_RENDERING
  if (ui->IsImpellerEnabled())
    return PixelDeferredImageGPUImpeller::Make(
        image, ui->GetSnapshotDelegate(),
        ui->GetTaskRunners().GetRasterTaskRunner());
#endif
  return DlImageGPU::Make(SkiaGPUObject<SkImage>(
      std::move(image), UIDartState::Current()->GetSkiaUnrefQueue()));
}

bool decode(Host& host,
            Surface& surface,
            Reader r,
            DisplayListBuilder& target,
            double width,
            double height,
            bool alpha) {
  if (r.size < 24 || r.u32() != 0x31435643 || r.u32() != 1)
    throw std::runtime_error("Invalid canvas header");
  r.u32();
  r.u32();
  r.u32();
  if (r.u32() != r.size)
    throw std::runtime_error("Canvas command length mismatch");
  auto next = surface;
  next.width = width;
  next.height = height;
  next.alpha = alpha;
  auto& s = next.state;
  DisplayListBuilder b(true);
  b.Save();
  for (const auto& saved : next.stack) {
    apply(b, saved);
    b.Save();
    b.TransformReset();
  }
  apply(b, s);
  bool cleared = false;
  while (r.offset < r.size) {
    const auto opcode = r.u16();
    if (r.u16() != 0)
      throw std::runtime_error("Unsupported canvas flags");
    const auto length = r.u32();
    r.check(length);
    Reader p{r.data + r.offset, length};
    auto transform = [&](const SkMatrix& matrix) {
      s.transform = matrix;
      b.SetTransform(ToDlMatrix(matrix));
    };
    auto image = [&](uint64_t id) {
      const auto found = host.images.find(id);
      if (found == host.images.end())
        throw std::runtime_error("Unknown canvas image");
      return found->second;
    };
    switch (opcode) {
      case 1:
        next.stack.push_back(s);
        b.Save();
        break;
      case 2:
        if (!next.stack.empty()) {
          s = next.stack.back();
          next.stack.pop_back();
          b.Restore();
        }
        break;
      case 3:
        while (b.GetSaveCount() > 1)
          b.Restore();
        b.DrawColor(alpha ? DlColor::kTransparent() : DlColor::kBlack(),
                    DlBlendMode::kSrc);
        b.Save();
        s = DrawState();
        next.stack.clear();
        cleared = true;
        break;
      case 4:
        transform(p.matrix());
        break;
      case 5:
        transform(SkMatrix::Concat(s.transform, p.matrix()));
        break;
      case 6: {
        const auto point = p.point();
        transform(SkMatrix::Concat(s.transform, SkMatrix::Translate(point)));
        break;
      }
      case 7:
        transform(SkMatrix::Concat(s.transform, SkMatrix::RotateRad(p.f64())));
        break;
      case 8: {
        const auto point = p.point();
        transform(SkMatrix::Concat(s.transform,
                                   SkMatrix::Scale(point.x(), point.y())));
        break;
      }
      case 9:
        transform(SkMatrix::I());
        break;
      case 10:
      case 11: {
        const auto rect = p.rect();
        if (opcode == 10 && rect.isEmpty())
          break;
        if (opcode == 10)
          draw(b, next, true,
               [&](DisplayListBuilder& canvas, const DlPaint& paint) {
                 canvas.DrawRect(ToDlRect(rect), paint);
               });
        else {
          const auto shape = DlPath(dashed(s, SkPath::Rect(rect)));
          draw(b, next, false,
               [&](DisplayListBuilder& canvas, const DlPaint& paint) {
                 canvas.DrawPath(shape, paint);
               });
        }
        break;
      }
      case 12: {
        const auto rect = p.rect();
        b.Save();
        b.ClipRect(ToDlRect(rect));
        b.DrawColor(alpha ? DlColor::kTransparent() : DlColor::kBlack(),
                    alpha ? DlBlendMode::kClear : DlBlendMode::kSrc);
        b.Restore();
        if (rect.left() <= 0 && rect.top() <= 0 && rect.right() >= width &&
            rect.bottom() >= height && next.stack.empty() && s.clips.empty() &&
            s.transform.isIdentity())
          cleared = true;
        break;
      }
      case 13:
      case 14:
      case 15: {
        auto shape = path(p);
        if (opcode != 14) {
          const auto rule = p.u32();
          if (rule > 1)
            throw std::runtime_error("Invalid fill rule");
          shape = shape.makeFillType(rule ? SkPathFillType::kEvenOdd
                                          : SkPathFillType::kWinding);
        }
        if (opcode == 15) {
          b.ClipPath(DlPath(shape), DlClipOp::kIntersect, true);
          s.clips.push_back(shape.makeTransform(s.transform));
        } else {
          const auto drawn = DlPath(opcode == 14 ? dashed(s, shape) : shape);
          draw(b, next, opcode == 13,
               [&](DisplayListBuilder& canvas, const DlPaint& paint) {
                 canvas.DrawPath(drawn, paint);
               });
        }
        break;
      }
      case 16:
      case 17: {
        const auto text = p.text();
        const double x = p.f64(), y = p.f64();
        const double max_width = p.offset == p.size
                                     ? std::numeric_limits<double>::infinity()
                                     : p.f64();
        if (max_width <= 0)
          break;
        draw(b, next, opcode == 16,
             [&](DisplayListBuilder& canvas, const DlPaint& paint) {
               TextBounds metrics;
               auto para = paragraph(s, host.fonts, text, paint, &metrics);
               const double scale = para->GetMaxWidth() > max_width
                                        ? max_width / para->GetMaxWidth()
                                        : 1;
               if (scale == 1 && metrics.skew == 0) {
                 const auto at = text_offset(s, *para, x, y, metrics);
                 para->Paint(&canvas, at[0], at[1]);
               } else if (scale >= std::numeric_limits<float>::min()) {
                 const auto at = text_offset(s, *para, 0, y, metrics);
                 const auto baseline = para->GetAlphabeticBaseline();
                 const auto transform = SkMatrix::Concat(
                     SkMatrix::Translate(x, at[1] + baseline),
                     SkMatrix::Concat(SkMatrix::Scale(scale, 1),
                                      SkMatrix::Skew(metrics.skew, 0)));
                 if (paint.getColorSourcePtr())
                   para = paragraph(s, host.fonts, text,
                                    text_transformed_paint(paint, transform));
                 canvas.Save();
                 canvas.Transform(ToDlMatrix(transform));
                 para->Paint(&canvas, at[0], -baseline);
                 canvas.Restore();
               }
             });
        break;
      }
      case 18:
      case 48: {
        sk_sp<DlImage> bitmap;
        uint32_t w = 0, h = 0;
        if (opcode == 18) bitmap = image(p.u32());
        else { w = p.u32(); h = p.u32(); }
        const auto src = p.rect(), dst = p.rect();
        if (opcode == 48) bitmap = pixels(p, w, h, false);
        draw(
            b, next, true,
            [&](DisplayListBuilder& canvas, const DlPaint& paint) {
              canvas.DrawImageRect(bitmap, ToDlRect(src), ToDlRect(dst),
                                   s.sampling(), &paint);
            },
            true);
        break;
      }
      case 19: {
        const auto w = p.u32(), h = p.u32();
        const auto at = p.point();
        const auto bitmap = pixels(p, w, h, !alpha);
        // Pixel writes ignore the complete drawing state, including clipping.
        while (b.GetSaveCount() > 1)
          b.Restore();
        DlPaint paint;
        paint.setBlendMode(DlBlendMode::kSrc);
        b.DrawImage(bitmap, ToDlPoint(at), DlImageSampling::kNearestNeighbor,
                    &paint);
        b.Save();
        for (const auto& saved : next.stack) {
          apply(b, saved);
          b.Save();
          b.TransformReset();
        }
        apply(b, s);
        break;
      }
      case 20:
        s.fill = p.color();
        s.fill_shader.reset();
        break;
      case 21:
        s.stroke = p.color();
        s.stroke_shader.reset();
        break;
      case 22:
      case 23:
      case 24:
      case 25:
      case 26:
      case 27: {
        auto shader = gradient(p, (opcode - 22) / 2);
        if (opcode % 2 == 0)
          s.fill_shader = shader;
        else
          s.stroke_shader = shader;
        break;
      }
      case 28:
        s.alpha = std::clamp(p.f64(), 0., 1.);
        break;
      case 29:
        s.blend = blend(p.text(), s.blend);
        break;
      case 30:
        s.width = std::max(0., p.f64());
        break;
      case 31: {
        const auto value = p.text();
        s.cap = value == "round"    ? DlStrokeCap::kRound
                : value == "square" ? DlStrokeCap::kSquare
                                    : DlStrokeCap::kButt;
        break;
      }
      case 32: {
        const auto value = p.text();
        s.join = value == "round"   ? DlStrokeJoin::kRound
                 : value == "bevel" ? DlStrokeJoin::kBevel
                                    : DlStrokeJoin::kMiter;
        break;
      }
      case 33:
        s.miter = std::max(0., p.f64());
        break;
      case 34: {
        const auto count = p.u32();
        if (count > (p.size - p.offset) / 8)
          throw std::runtime_error("Invalid dash count");
        s.dash.clear();
        for (uint32_t i = 0; i < count; i++)
          s.dash.push_back(std::max(0., p.f64()));
        break;
      }
      case 35:
        s.dash_offset = p.f64();
        break;
      case 36:
        s.shadow = p.color();
        s.blur = std::max(0., p.f64());
        s.shadow_x = p.f64();
        s.shadow_y = p.f64();
        break;
      case 37:
        s.smoothing = p.u32() != 0;
        if (p.offset < p.size)
          s.quality = std::min(p.u32(), 2u);
        break;
      case 38: {
        const auto text = p.text();
        if (p.offset == p.size) {
          filter(s, text);
          break;
        }
        const auto count = p.u32();
        if (count > 256 || count > (p.size - p.offset) / 4)
          throw std::runtime_error("Canvas filter count limit exceeded");
        s.filters.clear();
        for (uint32_t i = 0; i < count; i++) {
          FilterEffect effect;
          const auto kind = p.u32();
          if (kind == 0) {
            std::array<float, 20> matrix;
            for (auto& value : matrix)
              value = p.f64();
            effect.filter = DlImageFilter::MakeColorFilter(
                DlColorFilter::MakeMatrix(matrix.data()));
          } else if (kind == 1) {
            const auto sigma = p.f64();
            if (sigma < 0)
              throw std::runtime_error("Negative filter blur");
            effect.filter =
                DlImageFilter::MakeBlur(sigma, sigma, DlTileMode::kDecal);
          } else if (kind == 2) {
            effect.drop_shadow = true;
            effect.x = p.f64();
            effect.y = p.f64();
            effect.sigma = p.f64();
            if (effect.sigma < 0)
              throw std::runtime_error("Negative filter shadow blur");
            effect.color = p.color();
          } else
            throw std::runtime_error("Unknown canvas filter step");
          append_filter(s.filters, std::move(effect));
        }
        break;
      }
      case 39:
        s.font = p.text();
        s.caps = "normal";
        s.stretch = "normal";
        break;
      case 40:
        s.align = p.text();
        break;
      case 41:
        s.baseline = p.text();
        break;
      case 42:
        s.direction = p.text();
        break;
      case 43:
        s.letter = p.f64();
        s.letter_css = p.offset == p.size ? "" : p.text();
        break;
      case 44:
        s.word = p.f64();
        s.word_css = p.offset == p.size ? "" : p.text();
        break;
      case 45:
      case 46: {
        const auto bitmap = image(p.u32());
        const auto repeat = p.text();
        const auto matrix = ToDlMatrix(p.matrix());
        auto shader = DlColorSource::MakeImage(
            bitmap,
            repeat == "repeat" || repeat == "repeat-x" ? DlTileMode::kRepeat
                                                       : DlTileMode::kDecal,
            repeat == "repeat" || repeat == "repeat-y" ? DlTileMode::kRepeat
                                                       : DlTileMode::kDecal,
            s.sampling(), &matrix);
        if (opcode == 45)
          s.fill_shader = shader;
        else
          s.stroke_shader = shader;
        break;
      }
      case 47:
        s.lang = p.text();
        s.kerning = p.text();
        s.stretch = p.text();
        s.caps = p.text();
        s.rendering = p.text();
        break;
      default:
        throw std::runtime_error("Unknown canvas opcode");
    }
    p.done();
    r.offset += length;
    const auto padding = (8 - r.offset % 8) % 8;
    r.check(padding);
    r.offset += padding;
  }
  if (!alpha && width > 0 && height > 0) {
    while (b.GetSaveCount() > 1)
      b.Restore();
    DlPaint black;
    black.setColor(DlColor::kBlack()).setBlendMode(DlBlendMode::kDstOver);
    b.DrawRect(DlRect::MakeLTRB(0, 0, width, height), black);
  }
  const auto picture = b.Build();
  if (!picture->isUIThreadSafe())
    throw std::runtime_error("Canvas contains an unsafe image");
  target.DrawDisplayList(picture);
  surface = std::move(next);
  return cleared;
}

std::vector<uint8_t> read_pixels(sk_sp<DisplayList> display_list,
                                 int width,
                                 int height,
                                 int sx,
                                 int sy,
                                 int sw,
                                 int sh) {
  struct Result {
    fml::AutoResetWaitableEvent ready;
    std::vector<uint8_t> bytes;
    std::string error;
  };
  auto result = std::make_shared<Result>();
  auto* ui = UIDartState::Current();
  const auto delegate = ui->GetSnapshotDelegate();
  const bool impeller = ui->IsImpellerEnabled();
  fml::TaskRunner::RunNowOrPostTask(
      ui->GetTaskRunners().GetRasterTaskRunner(),
      [result, delegate, impeller, display_list = std::move(display_list),
       width, height, sx, sy, sw, sh]() {
        auto finish = [result, sx, sy, sw, sh](
                          sk_sp<SkImage> image,
                          GrDirectContext* context = nullptr) {
          if (!image)
            result->error = "Canvas raster snapshot failed";
          else {
            result->bytes.resize(size_t(sw) * sh * 4, 0);
            const auto info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType,
                                                kUnpremul_SkAlphaType);
            const bool overlaps = int64_t(sx) + sw > 0 &&
                                  int64_t(sy) + sh > 0 && sx < image->width() &&
                                  sy < image->height();
            if (overlaps &&
                !image->readPixels(context, info, result->bytes.data(),
                                   size_t(sw) * 4, sx, sy))
              result->error = "Canvas pixel readback failed";
          }
          result->ready.Signal();
        };
        if (!delegate) {
          finish(nullptr);
          return;
        }
#if IMPELLER_SUPPORTS_RENDERING
        if (impeller) {
          const auto texture = delegate->MakeImpellerSnapshotSync(
              display_list, DlISize(width, height),
              SnapshotPixelFormat::kRGBA8);
          const auto aiks = delegate->GetAiksContext();
          if (!texture || !aiks) {
            finish(nullptr);
            return;
          }
          auto image = impeller::DlImageImpeller::Make(
              texture, DlImage::OwningContext::kRaster);
          ImageEncodingImpeller::ConvertDlImageToSkImage(
              image,
              [finish](fml::StatusOr<sk_sp<SkImage>> image) {
                finish(image.ok() ? image.value() : nullptr);
              },
              delegate, aiks->GetContext());
          return;
        }
#endif
#if !SLIMPELLER
        finish(
            delegate->MakeSkiaSnapshotSync(display_list, DlISize(width, height),
                                           SnapshotPixelFormat::kDontCare),
            delegate->GetGrContext());
#else
        finish(nullptr);
#endif
      });
  // No Dart bytes are pinned while the GPU completes. The callback owns its
  // result even if a stopped renderer makes the bounded wait expire.
  if (result->ready.WaitWithTimeout(fml::TimeDelta::FromSeconds(10)))
    throw std::runtime_error("Canvas synchronous pixel readback timed out");
  if (!result->error.empty())
    throw std::runtime_error(result->error);
  return std::move(result->bytes);
}

tonic::DartWrappable* wrapped(Dart_Handle value, const char* type) {
  auto* object = tonic::DartConverterWrappable::FromDart(value);
  if (!object || std::strcmp(object->GetDartWrapperInfo().library_name, "ui") ||
      std::strcmp(object->GetDartWrapperInfo().interface_name, type))
    throw std::runtime_error("Invalid native canvas resource");
  return object;
}
std::shared_ptr<txt::FontCollection> font_collection(Dart_Handle value) {
  if (Dart_IsNull(value) || Dart_IsInteger(value)) {
    if (!Dart_IsNull(value)) {
      int64_t sentinel = -1;
      if (Dart_IsError(Dart_IntegerToInt64(value, &sentinel)) || sentinel != 0)
        throw std::runtime_error("Invalid canvas font collection");
    }
    return UIDartState::Current()
        ->platform_configuration()
        ->client()
        ->GetFontCollection()
        .GetFontCollection();
  }
  auto* scoped =
      static_cast<FlaxFontCollection*>(wrapped(value, "FlaxFontCollection"));
  auto fonts = scoped->GetFontCollection();
  if (!fonts)
    throw std::runtime_error("Canvas font collection is disposed");
  return fonts;
}
uint64_t integer(Dart_Handle value) {
  int64_t result = 0;
  if (Dart_IsError(Dart_IntegerToInt64(value, &result)) || result < 0)
    throw std::runtime_error("Invalid canvas resource id");
  return result;
}
Dart_Handle field(Dart_Handle list, intptr_t index) {
  const auto value = Dart_ListGetAt(list, index);
  if (Dart_IsError(value))
    throw std::runtime_error("Invalid canvas options");
  return value;
}

Dart_Handle bytes_result(const std::vector<uint8_t>& data) {
  auto result = Dart_NewTypedData(Dart_TypedData_kUint8, data.size());
  if (Dart_IsError(result))
    return result;
  {
    Bytes output(result);
    std::memcpy(const_cast<uint8_t*>(output.data), data.data(), data.size());
  }
  return result;
}

Dart_Handle encode_bitmap(Dart_Handle pixels, Dart_Handle options) {
  const auto width = integer(field(options, 0)),
             height = integer(field(options, 1));
  const auto format = integer(field(options, 2)),
             quality = integer(field(options, 3));
  if (width == 0 || height == 0 || width > INT_MAX / 4 || height > INT_MAX ||
      width * height > INT_MAX / 4 || quality > 100 ||
      (format != 1 && format != 2))
    throw std::runtime_error("Invalid canvas encoding options");
  std::vector<uint8_t> encoded;
  {
    Bytes bytes(pixels);
    if (bytes.size != intptr_t(width * height * 4))
      throw std::runtime_error("Invalid canvas encoding pixels");
    const auto info =
        SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                          kUnpremul_SkAlphaType, SkColorSpace::MakeSRGB());
    const SkPixmap source(info, bytes.data, width * 4);
    SkDynamicMemoryWStream stream;
    bool ok;
    if (format == 1) {
      SkJpegEncoder::Options jpeg;
      jpeg.fQuality = quality;
      jpeg.fAlphaOption = SkJpegEncoder::AlphaOption::kBlendOnBlack;
      ok = SkJpegEncoder::Encode(&stream, source, jpeg);
    } else {
      SkWebpEncoder::Options webp;
      webp.fQuality = quality;
      ok = SkWebpEncoder::Encode(&stream, source, webp);
    }
    if (!ok)
      throw std::runtime_error("Canvas image encoding failed");
    const auto data = stream.detachAsData();
    encoded.assign(data->bytes(), data->bytes() + data->size());
  }
  // Encoding is CPU-only and can run in a Dart worker isolate. Its input is no
  // longer pinned when the Dart result is allocated.
  return bytes_result(encoded);
}

// CPU-only bitmap creation runs in a Dart worker isolate, before any UI upload.
SkImageInfo bitmap_info(int64_t width, int64_t height, bool premultiplied) {
  if (width <= 0 || height <= 0 || width > INT_MAX / 4 ||
      height > INT_MAX / 4 || width * height > INT_MAX / 4)
    throw std::runtime_error("Invalid bitmap dimensions");
  return SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                          premultiplied ? kPremul_SkAlphaType : kUnpremul_SkAlphaType);
}

Dart_Handle decode_bitmap(Dart_Handle input, Dart_Handle options) {
  const bool convert = integer(field(options, 0)) != 0;
  const bool premultiplied = integer(field(options, 1)) != 0;
  const bool orient = integer(field(options, 2)) != 0;
  sk_sp<SkData> encoded;
  {
    Bytes bytes(input);
    encoded = SkData::MakeWithCopy(bytes.data, bytes.size);
  }
  auto codec = SkCodec::MakeFromData(std::move(encoded));
  if (!codec) throw std::runtime_error("The source image could not be decoded");
  const auto dimensions = codec->dimensions();
  auto info = bitmap_info(dimensions.width(), dimensions.height(), premultiplied);
  if (convert) info = info.makeColorSpace(SkColorSpace::MakeSRGB());
  std::vector<uint8_t> pixels(info.computeMinByteSize());
  if (codec->getPixels(info, pixels.data(), info.minRowBytes()) != SkCodec::kSuccess)
    throw std::runtime_error("The source image could not be decoded");
  auto origin = orient ? codec->getOrigin() : kTopLeft_SkEncodedOrigin;
  if (origin != kTopLeft_SkEncodedOrigin) {
    const auto output_info = SkEncodedOriginSwapsWidthHeight(origin)
                                 ? SkPixmapUtils::SwapWidthHeight(info) : info;
    std::vector<uint8_t> output(output_info.computeMinByteSize());
    if (!SkPixmapUtils::Orient(
            SkPixmap(output_info, output.data(), output_info.minRowBytes()),
            SkPixmap(info, pixels.data(), info.minRowBytes()), origin))
      throw std::runtime_error("Image orientation failed");
    info = output_info;
    pixels = std::move(output);
  }
  auto result = Dart_NewList(3);
  Dart_ListSetAt(result, 0, bytes_result(pixels));
  Dart_ListSetAt(result, 1, Dart_NewInteger(info.width()));
  Dart_ListSetAt(result, 2, Dart_NewInteger(info.height()));
  return result;
}

Dart_Handle transform_bitmap(Dart_Handle input, Dart_Handle options) {
  const int64_t width = integer(field(options, 0)), height = integer(field(options, 1));
  int64_t sx = 0, sy = 0;
  if (Dart_IsError(Dart_IntegerToInt64(field(options, 2), &sx)) ||
      Dart_IsError(Dart_IntegerToInt64(field(options, 3), &sy)) ||
      sx < INT_MIN || sx > INT_MAX || sy < INT_MIN || sy > INT_MAX)
    throw std::runtime_error("Invalid bitmap crop origin");
  const int64_t sw = integer(field(options, 4)), sh = integer(field(options, 5));
  const int64_t dw = integer(field(options, 6)), dh = integer(field(options, 7));
  const bool flip = integer(field(options, 8)) != 0;
  const auto quality = integer(field(options, 9));
  const bool source_premul = integer(field(options, 10)) != 0;
  const bool output_premul = integer(field(options, 11)) != 0;
  const auto source_info = bitmap_info(width, height, source_premul);
  const auto cropped_info = bitmap_info(sw, sh, source_premul);
  const auto scaled_info = bitmap_info(dw, dh, source_premul);
  const auto output_info = bitmap_info(dw, dh, output_premul);
  if (quality > 3) throw std::runtime_error("Invalid bitmap resize quality");
  std::vector<uint8_t> crop(cropped_info.computeMinByteSize());
  {
    Bytes bytes(input);
    if (size_t(bytes.size) != source_info.computeMinByteSize())
      throw std::runtime_error("Invalid bitmap pixel length");
    const int64_t left = std::max<int64_t>(0, sx), top = std::max<int64_t>(0, sy);
    const int64_t right = std::min<int64_t>(width, sx + sw);
    const int64_t bottom = std::min<int64_t>(height, sy + sh);
    if (right > left)
      for (int64_t y = top; y < bottom; ++y)
        std::memcpy(crop.data() + ((y - sy) * sw + left - sx) * 4,
                    bytes.data + (y * width + left) * 4, (right - left) * 4);
  }
  std::vector<uint8_t> scaled(scaled_info.computeMinByteSize());
  const SkPixmap cropped(cropped_info, crop.data(), cropped_info.minRowBytes());
  const SkPixmap target(scaled_info, scaled.data(), scaled_info.minRowBytes());
  if (sw == dw && sh == dh) {
    scaled = std::move(crop);
  } else if (quality == 0) {
    // Pixelated first chooses the nearest integer scale, then bilinear remainder.
    const int mw = sw * std::max(1, int(std::round(double(dw) / sw)));
    const int mh = sh * std::max(1, int(std::round(double(dh) / sh)));
    const auto middle_info = bitmap_info(mw, mh, source_premul);
    std::vector<uint8_t> middle(middle_info.computeMinByteSize());
    const SkPixmap midpoint(middle_info, middle.data(), middle_info.minRowBytes());
    if (!cropped.scalePixels(midpoint, SkSamplingOptions(SkFilterMode::kNearest)) ||
        !midpoint.scalePixels(target, SkSamplingOptions(SkFilterMode::kLinear)))
      throw std::runtime_error("Bitmap resize failed");
  } else {
    const auto sampling = quality == 3 ? SkSamplingOptions(SkCubicResampler::Mitchell())
                                      : SkSamplingOptions(SkFilterMode::kLinear);
    if (!cropped.scalePixels(target, sampling))
      throw std::runtime_error("Bitmap resize failed");
  }
  std::vector<uint8_t> output(output_info.computeMinByteSize());
  for (int y = 0; y < dh; ++y)
    std::memcpy(output.data() + size_t(y) * dw * 4,
                scaled.data() + size_t(flip ? dh - 1 - y : y) * dw * 4,
                size_t(dw) * 4);
  if (source_premul != output_premul)
    for (size_t i = 0; i < output.size(); i += 4)
      for (size_t c = 0; c < 3; ++c)
        output[i + c] = output_premul
            ? (output[i + c] * output[i + 3] + 127) / 255
            : output[i + 3] == 0 ? 0
            : std::min(255, (output[i + c] * 255 + output[i + 3] / 2) / output[i + 3]);
  return bytes_result(output);
}

}  // namespace flax::canvas

extern "C" __attribute__((visibility("default"))) uint32_t
flax_canvas_version() {
  return FLAX_CANVAS_VERSION;
}

// One non-leaf call per batch. No Dart API is called while command bytes are
// pinned.
extern "C" __attribute__((visibility("default"))) Dart_Handle
flax_canvas_dispatch(uint64_t token,
                     uint32_t operation,
                     Dart_Handle a,
                     Dart_Handle b,
                     Dart_Handle c) {
  using namespace flax::canvas;
  try {
    if (operation == 14)
      return encode_bitmap(a, b);
    if (operation == 16)
      return decode_bitmap(a, b);
    if (operation == 17)
      return transform_bitmap(a, b);
    if (!UIDartState::Current())
      throw std::runtime_error("Canvas requires the Flutter UI isolate");
    if (operation == 13) {
      auto* picture = static_cast<Picture*>(wrapped(a, "Picture"));
      auto display_list = picture->display_list();
      if (!display_list)
        throw std::runtime_error("Canvas picture is disposed");
      const auto width = integer(field(b, 0)), height = integer(field(b, 1));
      if (width == 0 || height == 0 || width > INT_MAX / 4 ||
          height > INT_MAX || width * height > INT_MAX / 4)
        throw std::runtime_error("Invalid canvas snapshot dimensions");
      auto* ui = UIDartState::Current();
      sk_sp<DlImage> snapshot;
#if IMPELLER_SUPPORTS_RENDERING
      if (ui->IsImpellerEnabled())
        snapshot = DlDeferredImageGPUImpeller::Make(
            std::move(display_list), DlISize(width, height),
            SnapshotPixelFormat::kRGBA8, ui->GetSnapshotDelegate(),
            ui->GetTaskRunners().GetRasterTaskRunner());
#endif
#if !SLIMPELLER
      if (!ui->IsImpellerEnabled())
        snapshot = DlDeferredImageGPUSkia::Make(
            SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                              kPremul_SkAlphaType),
            std::move(display_list), ui->GetSnapshotDelegate(),
            ui->GetTaskRunners().GetRasterTaskRunner(),
            ui->GetSkiaUnrefQueue());
#endif
      if (!snapshot)
        throw std::runtime_error("Canvas snapshot is unavailable");
      auto image = CanvasImage::Create();
      image->set_image(snapshot);
      return image->CreateOuterWrapping();
    }
    if (operation == 10) {
      uint64_t count = 0;
      std::lock_guard<std::mutex> lock(hosts_mutex);
      for (const auto& entry : hosts)
        if (entry.second->owner == Dart_CurrentIsolate())
          count +=
              1 + entry.second->surfaces.size() + entry.second->images.size();
      return Dart_NewInteger(count);
    }
    if (operation == 0) {
      auto host = std::make_shared<Host>();
      std::lock_guard<std::mutex> lock(hosts_mutex);
      const auto id = next_host++;
      hosts.emplace(id, host);
      return Dart_NewInteger(id);
    }
    std::shared_ptr<Host> host;
    {
      std::lock_guard<std::mutex> lock(hosts_mutex);
      const auto found = hosts.find(token);
      if (found == hosts.end())
        throw std::runtime_error("Canvas host is closed");
      host = found->second;
    }
    if (host->owner != Dart_CurrentIsolate())
      throw std::runtime_error("Wrong canvas owner isolate");
    switch (operation) {
      case 1: {
        const auto id = host->next++;
        host->surfaces.emplace(id, Surface());
        return Dart_NewInteger(id);
      }
      case 2: {
        const auto id = integer(field(a, 0));
        const auto width = integer(field(a, 1)), height = integer(field(a, 2));
        const auto alpha = integer(field(a, 3)) != 0;
        // Refresh the Session's public fallback cache before decoding. No Dart
        // handle is retained, and bytes are not pinned during this lookup.
        host->fonts = font_collection(field(a, 4));
        auto* target = static_cast<Canvas*>(wrapped(c, "Canvas"));
        if (!target->builder())
          throw std::runtime_error("Canvas target has finished recording");
        auto& surface = host->surfaces.at(id);
        bool cleared;
        {
          Bytes bytes(b);
          cleared = decode(*host, surface, {bytes.data, size_t(bytes.size)},
                           *target->builder(), width, height, alpha);
        }
        return Dart_NewBoolean(cleared);
      }
      case 3:
        host->surfaces.at(integer(a)) = Surface();
        return Dart_Null();
      case 4: {
        const auto id = integer(a);
        auto* shader = static_cast<ImageShader*>(wrapped(b, "ImageShader"));
        const auto source = shader->shader(DlImageSampling::kLinear);
        if (!source || !source->asImage())
          throw std::runtime_error("Invalid canvas image");
        host->images[id] =
            sk_ref_sp(const_cast<DlImage*>(source->asImage()->image().get()));
        return Dart_Null();
      }
      case 5:
        host->images.erase(integer(a));
        return Dart_Null();
      case 6: {
        host->fonts = font_collection(c);
        const auto& s = host->surfaces.at(integer(a)).state;
        uint8_t* text;
        intptr_t length;
        if (Dart_IsError(Dart_StringToUTF8(b, &text, &length)))
          throw std::runtime_error("Expected canvas text");
        TextBounds bounds;
        auto p =
            paragraph(s, host->fonts,
                      std::string(reinterpret_cast<const char*>(text), length),
                      DlPaint(), &bounds, true);
        const auto at = text_offset(s, *p, 0, 0, bounds);
        const auto baseline = p->GetAlphabeticBaseline() + at[1];
        const double metrics[] = {
            p->GetMaxWidth(),
            bounds.ink.isEmpty() ? 0 : -bounds.ink.left() - at[0],
            bounds.ink.isEmpty() ? 0 : bounds.ink.right() + at[0],
            bounds.ink.isEmpty() ? 0 : -bounds.ink.top() - at[1],
            bounds.ink.isEmpty() ? 0 : bounds.ink.bottom() + at[1],
            bounds.ascent - baseline,
            bounds.descent + baseline,
            bounds.em_ascent - baseline,
            bounds.em_descent + baseline,
            bounds.hanging - baseline,
            -baseline,
            bounds.ideographic - baseline};
        auto result = Dart_NewList(12);
        for (int i = 0; i < 12; i++)
          Dart_ListSetAt(result, i, Dart_NewDouble(metrics[i]));
        return result;
      }
      case 15: {
        auto fonts = font_collection(a)->CreateSktFontCollection();
        auto faces =
            fonts->findTypefaces({SkString("sans-serif")}, SkFontStyle());
        auto face = faces.empty() ? fonts->defaultFallback() : faces.front();
        SkFont reference(face, 10);
        SkString family;
        if (face)
          face->getFamilyName(&family);
        auto result = Dart_NewList(4);
        Dart_ListSetAt(result, 0, Dart_NewDouble(spacing("1ex", 0, reference)));
        Dart_ListSetAt(result, 1, Dart_NewDouble(spacing("1ch", 0, reference)));
        Dart_ListSetAt(result, 2, Dart_NewDouble(spacing("1ic", 0, reference)));
        Dart_ListSetAt(result, 3,
                       Dart_NewStringFromUTF8(
                           reinterpret_cast<const uint8_t*>(family.c_str()),
                           family.size()));
        return result;
      }
      case 7: {
        double x, y;
        if (Dart_IsError(Dart_DoubleValue(field(b, 0), &x)) ||
            Dart_IsError(Dart_DoubleValue(field(b, 1), &y)))
          throw std::runtime_error("Expected canvas point");
        const auto rule = integer(field(b, 2));
        bool contains;
        {
          Bytes bytes(a);
          Reader r{bytes.data, size_t(bytes.size)};
          auto shape = path(r);
          r.done();
          if (rule)
            shape = shape.makeFillType(SkPathFillType::kEvenOdd);
          contains = shape.contains(x, y);
        }
        return Dart_NewBoolean(contains);
      }
      case 11: {
        const auto& s = host->surfaces.at(integer(a)).state;
        double x, y;
        if (Dart_IsError(Dart_DoubleValue(field(c, 0), &x)) ||
            Dart_IsError(Dart_DoubleValue(field(c, 1), &y)))
          throw std::runtime_error("Expected canvas point");
        const bool explicit_path = integer(field(c, 2)) != 0;
        SkMatrix inverse;
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !s.transform.invert(&inverse))
          return Dart_False();
        const auto at = inverse.mapPoint({float(x), float(y)});
        bool hit;
        {
          Bytes bytes(b);
          Reader r{bytes.data, size_t(bytes.size)};
          auto shape = path(r);
          r.done();
          if (!explicit_path)
            shape = shape.makeTransform(inverse);
          SkPaint paint;
          paint.setStyle(SkPaint::kStroke_Style);
          paint.setStrokeWidth(s.width);
          paint.setStrokeCap(ToSk(s.cap));
          paint.setStrokeJoin(ToSk(s.join));
          paint.setStrokeMiter(s.miter);
          hit = skpathutils::FillPathWithPaint(dashed(s, shape), paint)
                    .contains(at.x(), at.y());
        }
        return Dart_NewBoolean(hit);
      }
      case 12: {
        auto* picture = static_cast<Picture*>(wrapped(a, "Picture"));
        auto display_list = picture->display_list();
        if (!display_list)
          throw std::runtime_error("Canvas picture is disposed");
        const int width = integer(field(b, 0)), height = integer(field(b, 1));
        int64_t sx, sy;
        if (Dart_IsError(Dart_IntegerToInt64(field(b, 2), &sx)) ||
            Dart_IsError(Dart_IntegerToInt64(field(b, 3), &sy)) ||
            sx < INT_MIN || sx > INT_MAX || sy < INT_MIN || sy > INT_MAX)
          throw std::runtime_error("Invalid canvas pixel origin");
        const auto sw = integer(field(b, 4)), sh = integer(field(b, 5));
        if (width <= 0 || height <= 0 || sw == 0 || sh == 0 ||
            sw > INT_MAX / 4 || sh > INT_MAX || sw * sh > INT_MAX / 4)
          throw std::runtime_error("Invalid canvas pixel dimensions");
        const auto data =
            read_pixels(std::move(display_list), width, height, sx, sy, sw, sh);
        return bytes_result(data);
      }
      case 8:
        host->surfaces.erase(integer(a));
        return Dart_Null();
      case 9: {
        std::lock_guard<std::mutex> lock(hosts_mutex);
        hosts.erase(token);
        return Dart_Null();
      }
      default:
        throw std::runtime_error("Unknown canvas engine operation");
    }
  } catch (const std::exception& error) {
    return Dart_NewStringFromCString(error.what());
  }
}
