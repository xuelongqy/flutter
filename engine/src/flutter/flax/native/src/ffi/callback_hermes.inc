std::unique_ptr<facebook::jsi::Runtime> flaxFfiCreateCallbackRuntime() {
  auto config = ::hermes::vm::RuntimeConfig::Builder()
                    .withES6BlockScoping(true)
                    .withMicrotaskQueue(true)
                    .build();
  return facebook::hermes::makeHermesRuntime(config);
}
flax::Uint8View flaxFfiReadBytes(facebook::jsi::Runtime &runtime,
                                 const facebook::jsi::Object &object) {
  return flax::readUint8Array(runtime, object);
}
