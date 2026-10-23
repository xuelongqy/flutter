// Experiment-only solid fillRect recording. Not a Canvas 2D implementation.
#include "flutter/display_list/dl_builder.h"
#include "flutter/lib/ui/painting/canvas.h"
#include "third_party/tonic/converter/dart_converter.h"

namespace flax::canvas_prototype {
namespace jsi = facebook::jsi;

struct State;
std::mutex pending_mutex;
std::unordered_map<uint64_t, std::weak_ptr<State>> pending;
uint64_t next_token = 1;

struct State {
  Dart_Isolate owner = Dart_CurrentIsolate();
  std::unique_ptr<flutter::DisplayListBuilder> builder;
  flutter::DlPaint paint;
  sk_sp<flutter::DisplayList> picture;
  uint64_t token = 0;

  State() { paint.setAntiAlias(true); }
  ~State() { retire(); }
  void retire() {
    std::lock_guard<std::mutex> lock(pending_mutex);
    pending.erase(token);
    token = 0;
    picture.reset();
  }
  void begin() {
    retire();
    builder = std::make_unique<flutter::DisplayListBuilder>(true);
  }
};

double number(jsi::Runtime& js, const jsi::Value& value) {
  if (!value.isNumber() || !std::isfinite(value.getNumber()) ||
      std::abs(value.getNumber()) > 1e8) {
    throw jsi::JSError(js, "Expected a finite Canvas prototype coordinate");
  }
  return value.getNumber();
}

void rect(jsi::Runtime& js, State& state, double x, double y,
          double width, double height) {
  if (!state.builder)
    throw jsi::JSError(js, "Call begin before drawing");
  if (width < 0 || height < 0)
    throw jsi::JSError(js, "Prototype rectangles require nonnegative sizes");
  state.builder->DrawRect(
      flutter::DlRect::MakeLTRB(x, y, x + width, y + height), state.paint);
}

// The existing command format is little-endian and may start at an unaligned view.
uint32_t u32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
         (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

double f64(jsi::Runtime& js, const uint8_t* bytes) {
  uint64_t bits = uint64_t(u32(bytes)) | (uint64_t(u32(bytes + 4)) << 32);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  if (!std::isfinite(value) || std::abs(value) > 1e8)
    throw jsi::JSError(js, "Invalid Canvas prototype packet coordinate");
  return value;
}

void batch(jsi::Runtime& js, State& state, const jsi::Value& value) {
  if (!state.builder || !value.isObject())
    throw jsi::JSError(js, "Expected an active frame and Uint8Array");
  try {
    auto view = flax::readUint8Array(js, value.getObject(js));
    if (view.length < 24)
      throw jsi::JSError(js, "Truncated Canvas prototype header");
    const auto* bytes = view.storage->data() + view.offset;
    if (u32(bytes) != 0x31435643 || u32(bytes + 4) != 1 ||
        u32(bytes + 20) != view.length || (view.length - 24) % 40 != 0)
      throw jsi::JSError(js, "Invalid Canvas prototype header");
    for (size_t offset = 24; offset < view.length; offset += 40) {
      if (u32(bytes + offset) != 10 || u32(bytes + offset + 4) != 32)
        throw jsi::JSError(js, "Prototype packets support fillRect only");
      rect(js, state, f64(js, bytes + offset + 8),
           f64(js, bytes + offset + 16), f64(js, bytes + offset + 24),
           f64(js, bytes + offset + 32));
    }
  } catch (const std::exception& error) {
    state.builder.reset();
    throw jsi::JSError(js, error.what());
  }
}

std::shared_ptr<State> install(jsi::Runtime& js) {
  auto state = std::make_shared<State>();
  jsi::Object api(js);
  api.setProperty(js, "begin", jsi::Function::createFromHostFunction(
      js, jsi::PropNameID::forAscii(js, "begin"), 0,
      [state](jsi::Runtime& js, const jsi::Value&, const jsi::Value*, size_t count) {
        if (count != 0) throw jsi::JSError(js, "begin takes no arguments");
        state->begin();
        return jsi::Value::undefined();
      }));
  api.setProperty(js, "fillRect", jsi::Function::createFromHostFunction(
      js, jsi::PropNameID::forAscii(js, "fillRect"), 4,
      [state](jsi::Runtime& js, const jsi::Value&, const jsi::Value* args, size_t count) {
        if (count != 4) throw jsi::JSError(js, "fillRect takes four numbers");
        rect(js, *state, number(js, args[0]), number(js, args[1]),
             number(js, args[2]), number(js, args[3]));
        return jsi::Value::undefined();
      }));
  api.setProperty(js, "batch", jsi::Function::createFromHostFunction(
      js, jsi::PropNameID::forAscii(js, "batch"), 1,
      [state](jsi::Runtime& js, const jsi::Value&, const jsi::Value* args, size_t count) {
        if (count != 1) throw jsi::JSError(js, "batch takes one Uint8Array");
        batch(js, *state, args[0]);
        return jsi::Value::undefined();
      }));
  api.setProperty(js, "commit", jsi::Function::createFromHostFunction(
      js, jsi::PropNameID::forAscii(js, "commit"), 0,
      [state](jsi::Runtime& js, const jsi::Value&, const jsi::Value*, size_t count) {
        if (count != 0 || !state->builder)
          throw jsi::JSError(js, "Expected an active Canvas prototype frame");
        state->picture = state->builder->Build();
        state->builder.reset();
        std::lock_guard<std::mutex> lock(pending_mutex);
        if (next_token >= (uint64_t(1) << 53))
          throw jsi::JSError(js, "Canvas prototype tokens exhausted");
        state->token = next_token++;
        pending.emplace(state->token, state);
        return jsi::Value(double(state->token));
      }));
  js.global().setProperty(js, "__flaxCanvasPrototype", std::move(api));
  return state;
}
}  // namespace flax::canvas_prototype

extern "C" __attribute__((visibility("default"))) int32_t
flax_canvas_prototype_draw(uint64_t token, Dart_Handle canvas) {
  using namespace flax::canvas_prototype;
  std::shared_ptr<State> state;
  {
    std::lock_guard<std::mutex> lock(pending_mutex);
    const auto found = pending.find(token);
    if (found == pending.end()) return 1;
    state = found->second.lock();
  }
  if (!state || state->owner != Dart_CurrentIsolate()) return 2;
  auto* wrapped = tonic::DartConverterWrappable::FromDart(canvas);
  if (!wrapped ||
      std::strcmp(wrapped->GetDartWrapperInfo().library_name, "ui") != 0 ||
      std::strcmp(wrapped->GetDartWrapperInfo().interface_name, "Canvas") != 0)
    return 3;
  auto* target = static_cast<flutter::Canvas*>(wrapped);
  if (!target->builder() || !state->picture) return 3;
  target->builder()->DrawDisplayList(state->picture);
  state->retire();
  return 0;
}

extern "C" __attribute__((visibility("default"))) uint64_t
flax_canvas_prototype_pending() {
  std::lock_guard<std::mutex> lock(flax::canvas_prototype::pending_mutex);
  return flax::canvas_prototype::pending.size();
}
