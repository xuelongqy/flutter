#include <deque>

namespace {
struct WorkerPacket {
  std::unique_ptr<uint8_t, decltype(&std::free)> bytes{nullptr, std::free};
  size_t length = 0;
  std::vector<std::shared_ptr<v8::BackingStore>> buffers;
  std::vector<v8::CompiledWasmModule> modules;
};
struct WorkerChannel {
  JsiRuntimeState* parent;
  JsiRuntimeState* child = nullptr;
  int64_t parentPort;
  int64_t childPort = 0;
  bool closed = false;
  bool finished = false;
  std::deque<WorkerPacket> parentQueue, childQueue;
};
std::mutex workersMutex;
uint64_t nextWorkerChannel = 1;
std::unordered_map<uint64_t, WorkerChannel> workerChannels;

void wakeWorker(int64_t port, uint64_t id) {
  if (!port)
    return;
  Dart_CObject message{};
  message.type = Dart_CObject_kInt64;
  message.value.as_int64 = id;
  Dart_PostCObject(port, &message);
}
void workerCloneError(v8::Isolate* isolate, v8::Local<v8::String> message) {
  auto context = isolate->GetCurrentContext();
  auto error = v8::Exception::Error(message).As<v8::Object>();
  auto name = v8::String::NewFromUtf8Literal(isolate, "DataCloneError");
  error->Set(context, v8::String::NewFromUtf8Literal(isolate, "name"), name)
      .Check();
  v8::Local<v8::Value> constructor;
  if (context->Global()
          ->Get(context,
                v8::String::NewFromUtf8Literal(isolate, "DOMException"))
          .ToLocal(&constructor) &&
      constructor->IsFunction()) {
    v8::Local<v8::Value> args[]{message, name};
    v8::Local<v8::Object> exception;
    if (constructor.As<v8::Function>()
            ->NewInstance(context, 2, args)
            .ToLocal(&exception))
      error = exception;
  }
  isolate->ThrowException(error);
}
class WorkerSerializer : public v8::ValueSerializer::Delegate {
 public:
  WorkerSerializer(v8::Isolate* isolate,
                   WorkerPacket& packet,
                   v8::Local<v8::Function> host)
      : isolate_(isolate), packet_(packet), host_(host) {}
  v8::ValueSerializer* serializer = nullptr;
  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    workerCloneError(isolate_, message);
  }
  v8::Maybe<bool> WriteHostObject(v8::Isolate*,
                                  v8::Local<v8::Object> object) override {
    if (!host_.IsEmpty()) {
      auto context = isolate_->GetCurrentContext();
      v8::Local<v8::Value> argument = object, record;
      if (!host_->Call(context, v8::Undefined(isolate_), 1, &argument)
               .ToLocal(&record))
        return v8::Nothing<bool>();
      if (!record->IsUndefined())
        return serializer->WriteValue(context, record);
    }
    ThrowDataCloneError(v8::String::NewFromUtf8Literal(
        isolate_, "Platform objects are not cloneable"));
    return v8::Nothing<bool>();
  }
  v8::Maybe<uint32_t> GetSharedArrayBufferId(
      v8::Isolate*,
      v8::Local<v8::SharedArrayBuffer>) override {
    ThrowDataCloneError(v8::String::NewFromUtf8Literal(
        isolate_, "SharedArrayBuffer is not supported"));
    return v8::Nothing<uint32_t>();
  }
  v8::Maybe<uint32_t> GetWasmModuleTransferId(
      v8::Isolate*,
      v8::Local<v8::WasmModuleObject> module) override {
    const uint32_t id = packet_.modules.size();
    packet_.modules.push_back(module->GetCompiledModule());
    return v8::Just(id);
  }
  bool HasCustomHostObject(v8::Isolate*) override { return true; }
  v8::Maybe<bool> IsHostObject(v8::Isolate*,
                               v8::Local<v8::Object> object) override {
    if (object->InternalFieldCount())
      return v8::Just(true);
    return object->Has(
        isolate_->GetCurrentContext(),
        v8::Symbol::For(isolate_, v8::String::NewFromUtf8Literal(
                                      isolate_, "flax.uncloneable")));
  }

 private:
  v8::Isolate* isolate_;
  WorkerPacket& packet_;
  v8::Local<v8::Function> host_;
};

class WorkerDeserializer : public v8::ValueDeserializer::Delegate {
 public:
  WorkerDeserializer(WorkerPacket& packet, v8::Local<v8::Function> host)
      : packet_(packet), host_(host) {}
  v8::ValueDeserializer* deserializer = nullptr;
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override {
    auto context = isolate->GetCurrentContext();
    v8::Local<v8::Value> record, result;
    if (host_.IsEmpty() || !deserializer->ReadValue(context).ToLocal(&record))
      return {};
    // Core's platform constructors are JS; packet data supplies no callbacks.
    v8::Isolate::AllowJavascriptExecutionScope allow_host_constructor(isolate);
    v8::Isolate::SuppressMicrotaskExecutionScope suppress_microtasks(isolate);
    if (!host_->Call(context, v8::Undefined(isolate), 1, &record)
             .ToLocal(&result))
      return {};
    if (!result->IsObject()) {
      workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                    isolate, "Invalid platform clone record"));
      return {};
    }
    return result.As<v8::Object>();
  }
  v8::MaybeLocal<v8::WasmModuleObject> GetWasmModuleFromId(
      v8::Isolate* isolate,
      uint32_t id) override {
    if (id >= packet_.modules.size())
      return {};
    return v8::WasmModuleObject::FromCompiledModule(isolate,
                                                    packet_.modules[id]);
  }

 private:
  WorkerPacket& packet_;
  v8::Local<v8::Function> host_;
};

void workerRejections(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  auto* state = JsiRuntimeState::fromContext(context);
  auto result = v8::Array::New(isolate, state->worker_rejections.size());
  uint32_t index = 0;
  for (const auto& entry : state->worker_rejections) {
    v8::Local<v8::Value> values[]{entry.promise.Get(isolate),
                                  entry.reason.Get(isolate),
                                  v8::Boolean::New(isolate, entry.handled)};
    result->Set(context, index++, v8::Array::New(isolate, values, 3)).Check();
  }
  state->worker_rejections.clear();
  args.GetReturnValue().Set(result);
}

void workerErrorDetails(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  auto value = args.Length() ? args[0] : v8::Undefined(isolate).As<v8::Value>();
  v8::TryCatch caught(isolate);
  auto text = value->ToString(context).FromMaybe(
      v8::String::NewFromUtf8Literal(isolate, "Worker exception"));
  v8::Local<v8::Value> filename = v8::String::Empty(isolate);
  int line = 0, column = 0;
  // A fresh Message points at this reporting helper, not the original throw.
  auto trace = v8::Exception::GetStackTrace(value);
  if (!trace.IsEmpty() && trace->GetFrameCount()) {
    auto frame = trace->GetFrame(isolate, 0);
    auto name = frame->GetScriptNameOrSourceURL();
    if (!name.IsEmpty())
      filename = name;
    line = frame->GetLineNumber();
    column = frame->GetColumn();
  }
  v8::Local<v8::Value> stack = v8::String::Empty(isolate);
  if (value->IsObject()) {
    auto candidate = value.As<v8::Object>()->Get(
        context, v8::String::NewFromUtf8Literal(isolate, "stack"));
    v8::Local<v8::Value> found;
    if (candidate.ToLocal(&found) && found->IsString())
      stack = found;
  }
  caught.Reset();
  v8::Local<v8::Value> values[]{
      text, filename,
      v8::Integer::New(isolate, line)->ToString(context).ToLocalChecked(),
      v8::Integer::New(isolate, column)->ToString(context).ToLocalChecked(),
      stack};
  args.GetReturnValue().Set(v8::Array::New(isolate, values, 5));
}

uint64_t workerChannelId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (!args.Length() || !args[0]->IsNumber())
    return 0;
  auto n = args[0].As<v8::Number>()->Value();
  if (n <= 0 || n > 9007199254740991.0 || std::floor(n) != n)
    return 0;
  return static_cast<uint64_t>(n);
}
bool serializeWorker(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     v8::Local<v8::Array> list,
                     WorkerPacket& packet,
                     v8::Local<v8::Function> host) {
  auto context = isolate->GetCurrentContext();
  auto mapping = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.webgpu.mapping"));
  auto transferable = [&](v8::Local<v8::ArrayBuffer> buffer) {
    return !buffer->WasDetached() && buffer->IsDetachable() &&
           !buffer->GetBackingStore()->IsImmutable() &&
           !buffer->HasPrivate(context, mapping).FromMaybe(true);
  };
  std::vector<v8::Local<v8::ArrayBuffer>> buffers;
  WorkerSerializer delegate(isolate, packet, host);
  v8::ValueSerializer serializer(isolate, &delegate);
  delegate.serializer = &serializer;
  for (uint32_t i = 0; i < list->Length(); i++) {
    v8::Local<v8::Value> entry;
    if (!list->Get(context, i).ToLocal(&entry))
      return false;
    bool valid = entry->IsArrayBuffer();
    if (valid) {
      auto buffer = entry.As<v8::ArrayBuffer>();
      valid = transferable(buffer);
      for (auto previous : buffers)
        if (previous == buffer)
          valid = false;
      if (valid) {
        buffers.push_back(buffer);
        packet.buffers.push_back(buffer->GetBackingStore());
        serializer.TransferArrayBuffer(i, buffer);
      }
    }
    if (!valid) {
      workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                    isolate,
                                    "Expected unique, attached, detachable, "
                                    "ArrayBuffers in transfer list"));
      return false;
    }
  }
  serializer.WriteHeader();
  if (!serializer.WriteValue(context, value).FromMaybe(false))
    return false;
  // Getters can reenter postMessage and detach a transfer while serializing.
  for (auto buffer : buffers) {
    if (!transferable(buffer)) {
      workerCloneError(
          isolate,
          v8::String::NewFromUtf8Literal(
              isolate, "Transfer buffer changed during serialization"));
      return false;
    }
  }
  auto released = serializer.Release();
  packet.bytes.reset(released.first);
  packet.length = released.second;
  for (auto buffer : buffers) {
    if (!buffer->Detach(v8::Local<v8::Value>()).FromMaybe(false)) {
      workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                    isolate, "Cannot detach transfer buffer"));
      return false;
    }
  }
  return true;
}
v8::MaybeLocal<v8::Value> deserializeWorker(v8::Isolate* isolate,
                                            WorkerPacket& packet,
                                            v8::Local<v8::Function> host) {
  auto context = isolate->GetCurrentContext();
  WorkerDeserializer delegate(packet, host);
  v8::ValueDeserializer deserializer(isolate, packet.bytes.get(), packet.length,
                                     &delegate);
  delegate.deserializer = &deserializer;
  for (uint32_t i = 0; i < packet.buffers.size(); i++)
    deserializer.TransferArrayBuffer(
        i, v8::ArrayBuffer::New(isolate, packet.buffers[i]));
  if (!deserializer.ReadHeader(context).FromMaybe(false))
    return {};
  v8::Local<v8::Value> message;
  if (!deserializer.ReadValue(context).ToLocal(&message))
    return {};
  return message;
}
void workerClone(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 4 || !args[1]->IsArray() || !args[2]->IsFunction() ||
      !args[3]->IsFunction())
    return;
  WorkerPacket packet;
  auto* isolate = args.GetIsolate();
  if (!serializeWorker(isolate, args[0], args[1].As<v8::Array>(), packet,
                       args[2].As<v8::Function>()))
    return;
  v8::Local<v8::Value> result;
  if (deserializeWorker(isolate, packet, args[3].As<v8::Function>())
          .ToLocal(&result))
    args.GetReturnValue().Set(result);
}

void workerWasmChunk(const v8::FunctionCallbackInfo<v8::Value>& args);
void workerWasmFinish(const v8::FunctionCallbackInfo<v8::Value>& args);
void workerWasmAbort(const v8::FunctionCallbackInfo<v8::Value>& args);
void workerWasmStreaming(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  auto streaming = v8::WasmStreaming::Unpack(isolate, args.Data());
  v8::TryCatch caught(isolate);
  v8::Local<v8::Value> callback;
  auto key = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.wasm.streaming"));
  if (!context->Global()->GetPrivate(context, key).ToLocal(&callback) ||
      !callback->IsFunction()) {
    streaming->Abort(v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
        isolate, "Wasm streaming host is unavailable")));
    return;
  }
  auto sink = v8::Object::New(isolate);
  for (auto entry : {std::pair{"write", workerWasmChunk},
                     std::pair{"finish", workerWasmFinish},
                     std::pair{"abort", workerWasmAbort}})
    sink->Set(context,
              v8::String::NewFromUtf8(isolate, entry.first).ToLocalChecked(),
              v8::Function::New(context, entry.second, args.Data())
                  .ToLocalChecked())
        .Check();
  v8::Local<v8::Value> parameters[]{args[0], sink};
  if (callback.As<v8::Function>()
          ->Call(context, v8::Undefined(isolate), 2, parameters)
          .IsEmpty()) {
    auto error = caught.Exception();
    caught.Reset();
    streaming->Abort(error);
  }
}
void workerInstallStreaming(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (!args.Length() || !args[0]->IsFunction())
    return;
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  auto key = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.wasm.streaming"));
  context->Global()->SetPrivate(context, key, args[0]).Check();
  isolate->SetWasmStreamingCallback(workerWasmStreaming);
  isolate->InstallConditionalFeatures(context);
}
void workerWasmChunk(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (!args.Length() || !args[0]->IsUint8Array()) {
    args.GetIsolate()->ThrowException(
        v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
            args.GetIsolate(), "Expected a Wasm byte chunk")));
    return;
  }
  auto view = args[0].As<v8::Uint8Array>();
  auto buffer = view->Buffer();
  if (buffer->WasDetached()) {
    args.GetIsolate()->ThrowException(
        v8::Exception::TypeError(v8::String::NewFromUtf8Literal(
            args.GetIsolate(), "Detached Wasm chunk")));
    return;
  }
  if (!view->ByteLength())
    return;
  auto store = buffer->GetBackingStore();
  auto streaming = v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->OnBytesReceived(
      static_cast<const uint8_t*>(store->Data()) + view->ByteOffset(),
      view->ByteLength());
}
void workerWasmFinish(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto streaming = v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  if (args.Length() && args[0]->IsString()) {
    v8::String::Utf8Value url(args.GetIsolate(), args[0]);
    streaming->SetUrl(*url, url.length());
  }
  streaming->Finish({});
}
void workerWasmAbort(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto streaming = v8::WasmStreaming::Unpack(args.GetIsolate(), args.Data());
  streaming->Abort(args[0]);
}
void workerPost(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  const auto id = workerChannelId(args);
  const bool parent = args.Length() > 1 && args[1]->IsTrue();
  auto* owner = JsiRuntimeState::fromContext(context);
  {
    std::lock_guard lock(workersMutex);
    auto found = workerChannels.find(id);
    if (found != workerChannels.end() &&
        (parent ? found->second.child : found->second.parent) != owner) {
      isolate->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(isolate, "Foreign worker channel")));
      return;
    }
  }
  if (args.Length() < 4 || !args[3]->IsArray())
    return;
  WorkerPacket packet;
  auto host = args.Length() > 4 && args[4]->IsFunction()
                  ? args[4].As<v8::Function>()
                  : v8::Local<v8::Function>();
  if (!serializeWorker(isolate, args[2], args[3].As<v8::Array>(), packet, host))
    return;
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(id);
  if (found == workerChannels.end() || found->second.closed)
    return;
  auto& channel = found->second;
  if (!parent && channel.finished)
    return;
  (parent ? channel.parentQueue : channel.childQueue)
      .push_back(std::move(packet));
  wakeWorker(parent ? channel.parentPort : channel.childPort, id);
}
void workerFinish(const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(workerChannelId(args));
  if (found == workerChannels.end() ||
      found->second.parent !=
          JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext()))
    return;
  found->second.finished = true;
  found->second.childQueue.clear();
  if (!found->second.child)
    workerChannels.erase(found);
}
void workerTake(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  const auto id = workerChannelId(args);
  const bool parent = args.Length() > 1 && args[1]->IsTrue();
  auto* owner = JsiRuntimeState::fromContext(context);
  WorkerPacket packet;
  {
    std::lock_guard lock(workersMutex);
    auto found = workerChannels.find(id);
    if (found == workerChannels.end())
      return;
    auto& channel = found->second;
    if ((parent ? channel.parent : channel.child) != owner)
      return;
    auto& queue = parent ? channel.parentQueue : channel.childQueue;
    if (channel.closed || queue.empty())
      return;
    packet = std::move(queue.front());
    queue.pop_front();
  }
  v8::Local<v8::Value> message;
  auto host = args.Length() > 2 && args[2]->IsFunction()
                  ? args[2].As<v8::Function>()
                  : v8::Local<v8::Function>();
  if (!deserializeWorker(isolate, packet, host).ToLocal(&message))
    return;
  auto result = v8::Array::New(isolate, 1);
  result->Set(context, 0, message).Check();
  args.GetReturnValue().Set(result);
}
}  // namespace
namespace flax {
std::unique_ptr<facebook::jsi::Runtime> createDetachedRuntime() {
  struct Detached {
    bool previous = flaxFfiDetachedRuntime;
    Detached() { flaxFfiDetachedRuntime = true; }
    ~Detached() { flaxFfiDetachedRuntime = previous; }
  } detached;
  auto runtime = jsi::abi::makeJsiAbiRuntime(
      v8_create_runtime,
      [](void*, jsi_config config) {
        v8_jsi_config_set_explicit_microtask_policy(config, true);
        v8_jsi_config_enable_multi_thread(config, true);
        return jsi_no_error;
      },
      nullptr);
  auto* state = getState(jsi::abi::getAbiRuntime(*runtime));
  V8Scope scope(state);
  state->isolate->SetAllowAtomicsWait(false);
  state->worker_rejection_tracking = true;
  return runtime;
}
void installWorkerBridge(facebook::jsi::Runtime& runtime, int64_t port) {
  auto* state = getState(jsi::abi::getAbiRuntime(runtime));
  V8Scope scope(state);
  auto context = state->getContextLocal();
  auto bridge = v8::Object::New(state->isolate);
  for (auto entry :
       {std::pair{"post", workerPost}, std::pair{"take", workerTake},
        std::pair{"clone", workerClone},
        std::pair{"streaming", workerInstallStreaming},
        std::pair{"finish", workerFinish},
        std::pair{"rejections", workerRejections},
        std::pair{"details", workerErrorDetails}})
    bridge
        ->Set(context,
              v8::String::NewFromUtf8(state->isolate, entry.first)
                  .ToLocalChecked(),
              v8::Function::New(context, entry.second).ToLocalChecked())
        .Check();
  auto open =
      v8::Function::New(
          context,
          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            std::lock_guard lock(workersMutex);
            if (nextWorkerChannel > 9007199254740991ULL) {
              args.GetIsolate()->ThrowException(
                  v8::Exception::Error(v8::String::NewFromUtf8Literal(
                      args.GetIsolate(), "Worker channel IDs exhausted")));
              return;
            }
            const auto id = nextWorkerChannel++;
            workerChannels.emplace(
                id, WorkerChannel{JsiRuntimeState::fromContext(
                                      args.GetIsolate()->GetCurrentContext()),
                                  nullptr,
                                  args.Data().As<v8::BigInt>()->Int64Value()});
            args.GetReturnValue().Set(static_cast<double>(id));
          },
          v8::BigInt::New(state->isolate, port))
          .ToLocalChecked();
  bridge
      ->Set(context, v8::String::NewFromUtf8Literal(state->isolate, "open"),
            open)
      .Check();
  context->Global()
      ->Set(
          context,
          v8::String::NewFromUtf8Literal(state->isolate, "__flaxNativeWorker"),
          bridge)
      .Check();
}
void attachWorker(facebook::jsi::Runtime& runtime, uint64_t id, int64_t port) {
  auto* state = getState(jsi::abi::getAbiRuntime(runtime));
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(id);
  if (found == workerChannels.end() || found->second.closed ||
      found->second.child)
    throw std::invalid_argument("Worker was terminated before attachment");
  found->second.child = state;
  found->second.childPort = port;
  for (size_t i = 0; i < found->second.childQueue.size(); i++)
    wakeWorker(port, id);
}
uint64_t workerChannelCount() {
  std::lock_guard lock(workersMutex);
  return workerChannels.size();
}
void interruptWorker(uint64_t id) {
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(id);
  if (found == workerChannels.end())
    return;
  auto& channel = found->second;
  channel.closed = true;
  channel.parentQueue.clear();
  channel.childQueue.clear();
  if (channel.child) {
    channel.child->isolate->TerminateExecution();
    wakeWorker(channel.childPort, 0);
  } else
    workerChannels.erase(found);
}
void retireWorkerRuntime(facebook::jsi::Runtime& runtime) {
  auto* state = getState(jsi::abi::getAbiRuntime(runtime));
  auto* isolate = state->isolate;
  std::lock_guard lock(workersMutex);
  for (auto it = workerChannels.begin(); it != workerChannels.end();) {
    auto& channel = it->second;
    if (channel.parent == state) {
      channel.parent = nullptr;
      channel.closed = true;
      channel.parentQueue.clear();
      channel.childQueue.clear();
      if (channel.child) {
        channel.child->isolate->TerminateExecution();
        wakeWorker(channel.childPort, 0);
      }
    }
    if (channel.child == state) {
      channel.child = nullptr;
      channel.childPort = 0;
      channel.finished = true;
      channel.childQueue.clear();
      isolate->CancelTerminateExecution();
    }
    if (!channel.child && (channel.closed || !channel.parent))
      it = workerChannels.erase(it);
    else
      ++it;
  }
}
}  // namespace flax
