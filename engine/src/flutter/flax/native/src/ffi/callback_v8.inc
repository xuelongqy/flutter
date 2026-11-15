#include "jsi_abi/JsiAbiRuntime.h"
flax::Uint8View flaxFfiReadBytes(facebook::jsi::Runtime &runtime,
                                 const facebook::jsi::Object &object) {
  auto *state = getState(jsi::abi::getAbiRuntime(runtime));
  V8Scope scope(state);
  auto value = toObjectHandle(jsi::abi::getAbiObject(runtime, object))
                   ->get(state->isolate);
  if (!value->IsUint8Array())
    throw std::invalid_argument("Expected an actual Uint8Array");
  auto array = value.As<v8::Uint8Array>();
  auto buffer = array->Buffer();
  std::shared_ptr<v8::BackingStore> storage;
  if (buffer->IsSharedArrayBuffer())
    storage = buffer.As<v8::SharedArrayBuffer>()->GetBackingStore();
  else {
    if (!buffer->IsArrayBuffer() || buffer->WasDetached())
      throw std::invalid_argument("Expected attached byte storage");
    storage = buffer->GetBackingStore();
  }
  auto offset = array->ByteOffset(), length = array->ByteLength();
  if (offset > storage->ByteLength() || length > storage->ByteLength() - offset)
    throw std::invalid_argument("Byte source is out of bounds");
  return {std::make_shared<flax::V8Buffer>(std::move(storage)), offset, length};
}
std::unique_ptr<facebook::jsi::Runtime> flaxFfiCreateCallbackRuntime() {
  struct Detached {
    bool previous = flaxFfiDetachedRuntime;
    Detached() { flaxFfiDetachedRuntime = true; }
    ~Detached() { flaxFfiDetachedRuntime = previous; }
  } detached;
  return jsi::abi::makeJsiAbiRuntime(
      v8_create_runtime,
      [](void *, jsi_config config) {
        v8_jsi_config_set_explicit_microtask_policy(config, true);
        v8_jsi_config_enable_multi_thread(config, true);
        return jsi_no_error;
      },
      nullptr);
}
