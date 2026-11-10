#include <algorithm>
#include <deque>

namespace {
struct WorkerPacket {
  std::unique_ptr<uint8_t, decltype(&std::free)> bytes{nullptr, std::free};
  size_t length = 0;
  std::vector<std::shared_ptr<v8::BackingStore>> buffers;
  std::vector<v8::CompiledWasmModule> modules;
  std::vector<uint64_t> ports;
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
struct MessagePortEndpoint {
  MessagePortEndpoint(uint64_t id, uint64_t peer) : id(id), peer(peer) {}

  bool canReceive() const { return peer || pendingSends || !queue.empty(); }

  uint64_t id;
  uint64_t peer;
  JsiRuntimeState* owner = nullptr;
  uint64_t ticket = 0;
  int64_t ownerPort = 0;
  bool bound = false;
  bool started = false;
  size_t pendingSends = 0;
  // The endpoint whose queue owns this in-transit port, if any.
  uint64_t carrier = 0;
  std::deque<WorkerPacket> queue;
};

constexpr uint64_t maxWorkerId = 9007199254740991;
std::mutex workersMutex;
uint64_t nextWorkerChannel = 1;
std::unordered_map<uint64_t, WorkerChannel> workerChannels;
uint64_t nextMessagePort = 1;
std::unordered_map<uint64_t, MessagePortEndpoint> messagePorts;
std::unordered_map<uint64_t, MessagePortEndpoint*> messagePortTickets;

void wakeWorker(int64_t port, int64_t id) {
  if (!port)
    return;
  Dart_CObject message{};
  message.type = Dart_CObject_kInt64;
  message.value.as_int64 = id;
  Dart_PostCObject(port, &message);
}

// Registry operations are called with workersMutex held. Endpoint addresses
// are stable until erasure and tickets are never reused.
MessagePortEndpoint* ownedMessagePort(JsiRuntimeState* owner, uint64_t ticket) {
  auto found = messagePortTickets.find(ticket);
  if (found == messagePortTickets.end() || found->second->owner != owner)
    return nullptr;
  return found->second;
}

void attachMessagePort(MessagePortEndpoint& endpoint,
                       uint64_t ticket,
                       JsiRuntimeState* owner,
                       int64_t ownerPort) {
  endpoint.ticket = ticket;
  endpoint.owner = owner;
  endpoint.ownerPort = ownerPort;
  endpoint.carrier = 0;
  messagePortTickets.emplace(ticket, &endpoint);
}

void wakeMessagePort(const MessagePortEndpoint& endpoint) {
  if (endpoint.started)
    wakeWorker(endpoint.ownerPort, -static_cast<int64_t>(endpoint.ticket));
}

void discardMessagePorts(std::vector<uint64_t> pending) {
  while (!pending.empty()) {
    const auto id = pending.back();
    pending.pop_back();
    auto found = messagePorts.find(id);
    if (found == messagePorts.end())
      continue;
    auto& endpoint = found->second;
    if (auto peer = messagePorts.find(endpoint.peer);
        peer != messagePorts.end()) {
      peer->second.peer = 0;
      if (!peer->second.pendingSends)
        wakeMessagePort(peer->second);
    }
    if (endpoint.ticket)
      messagePortTickets.erase(endpoint.ticket);
    for (const auto& packet : endpoint.queue)
      pending.insert(pending.end(), packet.ports.begin(), packet.ports.end());
    messagePorts.erase(found);
  }
}

void discardWorkerQueue(std::deque<WorkerPacket>& queue) {
  for (auto& packet : queue)
    discardMessagePorts(std::move(packet.ports));
  queue.clear();
}

v8::Local<v8::Private> messagePortKey(v8::Isolate* isolate) {
  return v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.messagePort"));
}

v8::Local<v8::Symbol> workerCloneMarker(v8::Isolate* isolate) {
  return v8::Symbol::For(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.uncloneable"));
}

uint64_t messagePortTicket(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::Local<v8::Value> ticket;
  if (!value->IsObject() || value->IsProxy() ||
      !value.As<v8::Object>()
           ->GetPrivate(isolate->GetCurrentContext(), messagePortKey(isolate))
           .ToLocal(&ticket) ||
      !ticket->IsNumber())
    return 0;
  return static_cast<uint64_t>(ticket.As<v8::Number>()->Value());
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
                   v8::Local<v8::Function> host,
                   const std::unordered_map<uint64_t, uint32_t>& ports)
      : isolate_(isolate), packet_(packet), host_(host), ports_(ports) {}
  v8::ValueSerializer* serializer = nullptr;
  void ThrowDataCloneError(v8::Local<v8::String> message) override {
    workerCloneError(isolate_, message);
  }
  v8::Maybe<bool> WriteHostObject(v8::Isolate*,
                                  v8::Local<v8::Object> object) override {
    auto context = isolate_->GetCurrentContext();
    if (const auto ticket = messagePortTicket(isolate_, object)) {
      auto port = ports_.find(ticket);
      if (port != ports_.end()) {
        // Platform clone records are arrays. A number denotes a port ordinal.
        return serializer->WriteValue(
            context, v8::Integer::NewFromUnsigned(isolate_, port->second));
      }
      ThrowDataCloneError(v8::String::NewFromUtf8Literal(
          isolate_, "MessagePort must be transferred"));
      return v8::Nothing<bool>();
    }
    if (!host_.IsEmpty()) {
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
    return object->Has(isolate_->GetCurrentContext(),
                       workerCloneMarker(isolate_));
  }

 private:
  v8::Isolate* isolate_;
  WorkerPacket& packet_;
  v8::Local<v8::Function> host_;
  const std::unordered_map<uint64_t, uint32_t>& ports_;
};

class WorkerDeserializer : public v8::ValueDeserializer::Delegate {
 public:
  WorkerDeserializer(WorkerPacket& packet,
                     v8::Local<v8::Function> host,
                     const std::vector<v8::Local<v8::Value>>& ports)
      : packet_(packet), host_(host), ports_(ports) {}
  v8::ValueDeserializer* deserializer = nullptr;
  v8::MaybeLocal<v8::Object> ReadHostObject(v8::Isolate* isolate) override {
    auto context = isolate->GetCurrentContext();
    v8::Local<v8::Value> record, result;
    if (!deserializer->ReadValue(context).ToLocal(&record))
      return {};
    if (record->IsUint32()) {
      const auto index = record.As<v8::Uint32>()->Value();
      if (index < ports_.size())
        return ports_[index].As<v8::Object>();
      return {};
    }
    if (host_.IsEmpty())
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
  const std::vector<v8::Local<v8::Value>>& ports_;
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

uint64_t workerId(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (!args.Length() || !args[0]->IsNumber())
    return 0;
  auto n = args[0].As<v8::Number>()->Value();
  if (n <= 0 || n > maxWorkerId || std::floor(n) != n)
    return 0;
  return static_cast<uint64_t>(n);
}
v8::Local<v8::Function> workerCallback(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    int index) {
  return args.Length() > index && args[index]->IsFunction()
             ? args[index].As<v8::Function>()
             : v8::Local<v8::Function>();
}

int64_t workerDartPort(const v8::FunctionCallbackInfo<v8::Value>& args) {
  return args.Data().As<v8::BigInt>()->Int64Value();
}

bool serializeWorker(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     v8::Local<v8::Array> list,
                     WorkerPacket& packet,
                     v8::Local<v8::Function> host,
                     v8::Local<v8::Function> precommit,
                     uint64_t sourcePort = 0) {
  auto context = isolate->GetCurrentContext();
  auto transferable = [&](v8::Local<v8::ArrayBuffer> buffer) {
    return !buffer->WasDetached() && buffer->IsDetachable() &&
           !buffer->GetBackingStore()->IsImmutable();
  };
  std::vector<v8::Local<v8::ArrayBuffer>> buffers;
  std::unordered_map<uint64_t, uint32_t> portOrdinals;
  WorkerSerializer delegate(isolate, packet, host, portOrdinals);
  v8::ValueSerializer serializer(isolate, &delegate);
  delegate.serializer = &serializer;
  for (uint32_t i = 0; i < list->Length(); i++) {
    v8::Local<v8::Value> entry;
    if (!list->Get(context, i).ToLocal(&entry))
      return false;
    bool valid;
    if (entry->IsArrayBuffer()) {
      auto buffer = entry.As<v8::ArrayBuffer>();
      valid = transferable(buffer);
      for (auto previous : buffers)
        if (previous == buffer)
          valid = false;
      if (valid) {
        serializer.TransferArrayBuffer(buffers.size(), buffer);
        buffers.push_back(buffer);
        packet.buffers.push_back(buffer->GetBackingStore());
      }
    } else {
      const auto ticket = messagePortTicket(isolate, entry);
      valid = ticket && ticket != sourcePort &&
              portOrdinals.emplace(ticket, portOrdinals.size()).second;
    }
    if (!valid) {
      workerCloneError(isolate,
                       v8::String::NewFromUtf8Literal(
                           isolate, "Invalid or duplicate transfer entry"));
      return false;
    }
  }
  auto validateBuffers = [&] {
    for (auto buffer : buffers) {
      if (!transferable(buffer)) {
        workerCloneError(
            isolate,
            v8::String::NewFromUtf8Literal(
                isolate, "Transfer buffer changed during serialization"));
        return false;
      }
    }
    return true;
  };
  auto* owner = JsiRuntimeState::fromContext(context);
  const auto portsValid = [&] {
    for (const auto& entry : portOrdinals)
      if (!ownedMessagePort(owner, entry.first))
        return false;
    return true;
  };
  const auto portError = [&] {
    workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                  isolate, "MessagePort is detached"));
    return false;
  };
  std::unique_lock lock(workersMutex, std::defer_lock);
  const auto validatePorts = [&] {
    if (portOrdinals.empty())
      return true;
    lock.lock();
    const bool valid = portsValid();
    lock.unlock();
    return valid || portError();
  };
  if (!validatePorts())
    return false;
  serializer.WriteHeader();
  if (!serializer.WriteValue(context, value).FromMaybe(false))
    return false;
  // Getters can reenter postMessage or acquire a canvas context while cloning.
  if (!validateBuffers() || !validatePorts())
    return false;
  if (!precommit.IsEmpty() &&
      precommit->Call(context, v8::Undefined(isolate), 0, nullptr).IsEmpty())
    return false;
  if (!validateBuffers())
    return false;
  // Cloning and precommit exceptions precede detach-key failures.
  auto mapping = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, "flax.webgpu.mapping"));
  for (auto buffer : buffers) {
    bool mapped;
    if (!buffer->HasPrivate(context, mapping).To(&mapped))
      return false;
    if (mapped) {
      isolate->ThrowException(v8::Exception::TypeError(
          v8::String::NewFromUtf8Literal(isolate,
                                       "Cannot transfer a mapped GPU buffer")));
      return false;
    }
  }
  auto released = serializer.Release();
  packet.bytes.reset(released.first);
  packet.length = released.second;
  if (!portOrdinals.empty()) {
    lock.lock();
    if (!portsValid()) {
      lock.unlock();
      return portError();
    }
  }
  for (auto buffer : buffers) {
    if (!buffer->Detach(v8::Local<v8::Value>()).FromMaybe(false)) {
      if (lock.owns_lock())
        lock.unlock();
      workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                    isolate, "Cannot detach transfer buffer"));
      return false;
    }
  }
  packet.ports.resize(portOrdinals.size());
  for (const auto& [ticket, ordinal] : portOrdinals) {
    auto& endpoint = *ownedMessagePort(owner, ticket);
    packet.ports[ordinal] = endpoint.id;
    messagePortTickets.erase(ticket);
    endpoint.owner = nullptr;
    endpoint.ownerPort = 0;
    endpoint.ticket = 0;
    endpoint.bound = false;
    endpoint.started = false;
    endpoint.carrier = 0;
  }
  return true;
}

bool restoreMessagePorts(v8::Isolate* isolate,
                         const WorkerPacket& packet,
                         v8::Local<v8::Function> factory,
                         int64_t ownerPort,
                         std::vector<v8::Local<v8::Value>>& ports) {
  if (packet.ports.empty())
    return true;
  auto context = isolate->GetCurrentContext();
  auto* owner = JsiRuntimeState::fromContext(context);
  uint64_t firstTicket = 0;
  if (!factory.IsEmpty()) {
    std::lock_guard lock(workersMutex);
    if (nextMessagePort + packet.ports.size() - 1 <= maxWorkerId) {
      firstTicket = nextMessagePort;
      for (const auto id : packet.ports)
        attachMessagePort(messagePorts.at(id), nextMessagePort++, owner,
                          ownerPort);
    }
  }
  if (!firstTicket) {
    workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                  isolate, "Cannot receive MessagePort"));
    return false;
  }
  ports.reserve(packet.ports.size());
  for (size_t i = 0; i < packet.ports.size(); i++) {
    const auto ticket = firstTicket + i;
    v8::Local<v8::Value> argument = v8::Number::New(isolate, ticket), port;
    if (!factory->Call(context, v8::Undefined(isolate), 1, &argument)
             .ToLocal(&port))
      return false;
    if (messagePortTicket(isolate, port) != ticket) {
      workerCloneError(isolate, v8::String::NewFromUtf8Literal(
                                    isolate, "Invalid MessagePort wrapper"));
      return false;
    }
    ports.push_back(port);
  }
  return true;
}

v8::MaybeLocal<v8::Value> deserializeWorker(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    WorkerPacket& packet,
    int hostIndex,
    int portFactoryIndex,
    std::vector<v8::Local<v8::Value>>& ports) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  v8::MaybeLocal<v8::Value> message;
  if (restoreMessagePorts(isolate, packet,
                          workerCallback(args, portFactoryIndex),
                          workerDartPort(args), ports)) {
    WorkerDeserializer delegate(packet, workerCallback(args, hostIndex), ports);
    v8::ValueDeserializer deserializer(isolate, packet.bytes.get(),
                                       packet.length, &delegate);
    delegate.deserializer = &deserializer;
    for (uint32_t i = 0; i < packet.buffers.size(); i++)
      deserializer.TransferArrayBuffer(
          i, v8::ArrayBuffer::New(isolate, packet.buffers[i]));
    if (deserializer.ReadHeader(context).FromMaybe(false))
      message = deserializer.ReadValue(context);
  }
  if (message.IsEmpty() && !packet.ports.empty()) {
    std::lock_guard lock(workersMutex);
    discardMessagePorts(std::move(packet.ports));
  }
  return message;
}
void workerClone(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 4 || !args[1]->IsArray() || !args[2]->IsFunction() ||
      !args[3]->IsFunction())
    return;
  WorkerPacket packet;
  auto* isolate = args.GetIsolate();
  if (!serializeWorker(isolate, args[0], args[1].As<v8::Array>(), packet,
                       args[2].As<v8::Function>(), workerCallback(args, 4)))
    return;
  v8::Local<v8::Value> result;
  std::vector<v8::Local<v8::Value>> ports;
  if (deserializeWorker(args, packet, 3, 5, ports).ToLocal(&result))
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
  const auto id = workerId(args);
  const bool parent = args.Length() > 1 && args[1]->IsTrue();
  auto* owner = JsiRuntimeState::fromContext(context);
  bool foreign;
  {
    std::lock_guard lock(workersMutex);
    auto found = workerChannels.find(id);
    foreign = found != workerChannels.end() &&
              (parent ? found->second.child : found->second.parent) != owner;
  }
  if (foreign) {
    isolate->ThrowException(v8::Exception::TypeError(
        v8::String::NewFromUtf8Literal(isolate, "Foreign worker channel")));
    return;
  }
  if (args.Length() < 4 || !args[3]->IsArray())
    return;
  WorkerPacket packet;
  if (!serializeWorker(isolate, args[2], args[3].As<v8::Array>(), packet,
                       workerCallback(args, 4), workerCallback(args, 5)))
    return;
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(id);
  if (found == workerChannels.end() || found->second.closed ||
      (!parent && found->second.finished)) {
    discardMessagePorts(std::move(packet.ports));
    return;
  }
  auto& channel = found->second;
  (parent ? channel.parentQueue : channel.childQueue)
      .push_back(std::move(packet));
  wakeWorker(parent ? channel.parentPort : channel.childPort, id);
}
void workerFinish(const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::lock_guard lock(workersMutex);
  auto found = workerChannels.find(workerId(args));
  if (found == workerChannels.end() ||
      found->second.parent !=
          JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext()))
    return;
  found->second.finished = true;
  discardWorkerQueue(found->second.childQueue);
  if (!found->second.child) {
    discardWorkerQueue(found->second.parentQueue);
    workerChannels.erase(found);
  }
}

void returnWorkerPacket(const v8::FunctionCallbackInfo<v8::Value>& args,
                        WorkerPacket& packet,
                        int hostIndex,
                        int portFactoryIndex) {
  auto* isolate = args.GetIsolate();
  std::vector<v8::Local<v8::Value>> ports;
  v8::Local<v8::Value> message;
  if (!deserializeWorker(args, packet, hostIndex, portFactoryIndex, ports)
           .ToLocal(&message))
    return;
  v8::Local<v8::Value> result[]{message, v8::Undefined(isolate)};
  if (!ports.empty())
    result[1] = v8::Array::New(isolate, ports.data(), ports.size());
  args.GetReturnValue().Set(
      v8::Array::New(isolate, result, ports.empty() ? 1 : 2));
}

void workerTake(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto context = isolate->GetCurrentContext();
  const auto id = workerId(args);
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
  returnWorkerPacket(args, packet, 2, 3);
}

void messagePortOpen(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto* owner = JsiRuntimeState::fromContext(isolate->GetCurrentContext());
  const auto ownerPort = workerDartPort(args);
  uint64_t first = 0;
  {
    std::lock_guard lock(workersMutex);
    if (nextMessagePort < maxWorkerId) {
      first = nextMessagePort;
      nextMessagePort += 2;
      auto& a = messagePorts.try_emplace(first, first, first + 1).first->second;
      auto& b =
          messagePorts.try_emplace(first + 1, first + 1, first).first->second;
      attachMessagePort(a, first, owner, ownerPort);
      attachMessagePort(b, first + 1, owner, ownerPort);
    }
  }
  if (!first) {
    isolate->ThrowException(v8::Exception::Error(
        v8::String::NewFromUtf8Literal(isolate, "MessagePort IDs exhausted")));
    return;
  }
  v8::Local<v8::Value> result[]{v8::Number::New(isolate, first),
                                v8::Number::New(isolate, first + 1)};
  args.GetReturnValue().Set(v8::Array::New(isolate, result, 2));
}

void messagePortBind(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  const auto id = workerId(args);
  auto context = isolate->GetCurrentContext();
  auto* owner = JsiRuntimeState::fromContext(context);
  args.GetReturnValue().Set(false);
  if (args.Length() < 2 || !args[1]->IsObject() || args[1]->IsProxy() ||
      messagePortTicket(isolate, args[1]))
    return;
  {
    std::lock_guard lock(workersMutex);
    auto* endpoint = ownedMessagePort(owner, id);
    if (!endpoint || endpoint->bound)
      return;
    endpoint->bound = true;
  }
  auto wrapper = args[1].As<v8::Object>();
  const auto attributes = static_cast<v8::PropertyAttribute>(
      v8::ReadOnly | v8::DontEnum | v8::DontDelete);
  if (!wrapper
           ->DefineOwnProperty(context, workerCloneMarker(isolate),
                               v8::True(isolate), attributes)
           .FromMaybe(false))
    return;
  args.GetReturnValue().Set(
      wrapper->SetPrivate(context, messagePortKey(isolate), args[0])
          .FromMaybe(false));
}

void messagePortStart(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* owner =
      JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext());
  const auto id = workerId(args);
  std::lock_guard lock(workersMutex);
  auto* endpoint = ownedMessagePort(owner, id);
  if (endpoint && !endpoint->started) {
    endpoint->started = true;
    for (size_t i = 0; i < endpoint->queue.size(); i++)
      wakeMessagePort(*endpoint);
    if (!endpoint->peer && !endpoint->pendingSends && !endpoint->queue.empty())
      wakeMessagePort(*endpoint);
  }
  args.GetReturnValue().Set(endpoint && endpoint->canReceive());
}

void messagePortClose(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* owner =
      JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext());
  const auto id = workerId(args);
  std::lock_guard lock(workersMutex);
  if (auto* endpoint = ownedMessagePort(owner, id))
    discardMessagePorts({endpoint->id});
}

void messagePortState(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* owner =
      JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext());
  const auto id = workerId(args);
  std::lock_guard lock(workersMutex);
  auto* endpoint = ownedMessagePort(owner, id);
  args.GetReturnValue().Set(!endpoint ? 0 : (endpoint->canReceive() ? 2 : 1));
}

bool queueMessagePort(MessagePortEndpoint& target, WorkerPacket& packet) {
  if (!packet.ports.empty()) {
    // Moving the root of this carrier chain into a descendant's queue would
    // make the chain unreachable.
    auto* root = &target;
    while (root->carrier)
      root = &messagePorts.at(root->carrier);
    if (std::find(packet.ports.begin(), packet.ports.end(), root->id) !=
        packet.ports.end())
      return false;
    for (const auto transferred : packet.ports)
      messagePorts.at(transferred).carrier = target.id;
  }
  target.queue.push_back(std::move(packet));
  wakeMessagePort(target);
  return true;
}

void messagePortPost(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args.Length() < 3 || !args[2]->IsArray())
    return;
  auto* isolate = args.GetIsolate();
  auto* owner = JsiRuntimeState::fromContext(isolate->GetCurrentContext());
  const auto id = workerId(args);
  uint64_t target = 0;
  {
    std::lock_guard lock(workersMutex);
    if (auto* endpoint = ownedMessagePort(owner, id))
      target = endpoint->peer;
    // A serialization getter can close the sender before the packet is queued.
    if (target)
      ++messagePorts.at(target).pendingSends;
  }
  WorkerPacket packet;
  const bool serialized =
      serializeWorker(isolate, args[1], args[2].As<v8::Array>(), packet,
                      workerCallback(args, 3), workerCallback(args, 4), id);
  std::lock_guard lock(workersMutex);
  auto found = messagePorts.find(target);
  if (serialized && (found == messagePorts.end() ||
                     !queueMessagePort(found->second, packet))) {
    discardMessagePorts(std::move(packet.ports));
    found = messagePorts.find(target);
  }
  if (found != messagePorts.end()) {
    auto& endpoint = found->second;
    if (--endpoint.pendingSends == 0 && !endpoint.peer)
      wakeMessagePort(endpoint);
  }
}

void messagePortTake(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* owner =
      JsiRuntimeState::fromContext(args.GetIsolate()->GetCurrentContext());
  const auto id = workerId(args);
  WorkerPacket packet;
  {
    std::lock_guard lock(workersMutex);
    auto* endpoint = ownedMessagePort(owner, id);
    if (!endpoint || !endpoint->started || endpoint->queue.empty())
      return;
    packet = std::move(endpoint->queue.front());
    endpoint->queue.pop_front();
    for (const auto transferred : packet.ports)
      messagePorts.at(transferred).carrier = 0;
  }
  returnWorkerPacket(args, packet, 1, 2);
}

void messagePortCount(const v8::FunctionCallbackInfo<v8::Value>& args) {
  std::lock_guard lock(workersMutex);
  args.GetReturnValue().Set(static_cast<double>(messagePorts.size()));
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
  auto dartPort = v8::BigInt::New(state->isolate, port);
  bridge
      ->Set(context,
            v8::String::NewFromUtf8Literal(state->isolate,
                                          "canvasTransferVersion"),
            v8::Integer::New(state->isolate, 1))
      .Check();
  for (auto entry :
       {std::pair{"post", workerPost}, std::pair{"take", workerTake},
        std::pair{"clone", workerClone},
        std::pair{"streaming", workerInstallStreaming},
        std::pair{"finish", workerFinish},
        std::pair{"rejections", workerRejections},
        std::pair{"details", workerErrorDetails},
        std::pair{"portOpen", messagePortOpen},
        std::pair{"portBind", messagePortBind},
        std::pair{"portStart", messagePortStart},
        std::pair{"portClose", messagePortClose},
        std::pair{"portState", messagePortState},
        std::pair{"portPost", messagePortPost},
        std::pair{"portTake", messagePortTake},
        std::pair{"portCount", messagePortCount}})
    bridge
        ->Set(
            context,
            v8::String::NewFromUtf8(state->isolate, entry.first)
                .ToLocalChecked(),
            v8::Function::New(context, entry.second, dartPort).ToLocalChecked())
        .Check();
  auto open =
      v8::Function::New(
          context,
          [](const v8::FunctionCallbackInfo<v8::Value>& args) {
            std::lock_guard lock(workersMutex);
            if (nextWorkerChannel > maxWorkerId) {
              args.GetIsolate()->ThrowException(
                  v8::Exception::Error(v8::String::NewFromUtf8Literal(
                      args.GetIsolate(), "Worker channel IDs exhausted")));
              return;
            }
            const auto id = nextWorkerChannel++;
            workerChannels.emplace(
                id, WorkerChannel{JsiRuntimeState::fromContext(
                                      args.GetIsolate()->GetCurrentContext()),
                                  nullptr, workerDartPort(args)});
            args.GetReturnValue().Set(static_cast<double>(id));
          },
          dartPort)
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
  discardWorkerQueue(channel.parentQueue);
  discardWorkerQueue(channel.childQueue);
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
  std::vector<uint64_t> ownedPorts;
  for (const auto& [id, endpoint] : messagePorts)
    if (endpoint.owner == state)
      ownedPorts.push_back(id);
  discardMessagePorts(std::move(ownedPorts));
  for (auto it = workerChannels.begin(); it != workerChannels.end();) {
    auto& channel = it->second;
    if (channel.parent == state) {
      channel.parent = nullptr;
      channel.closed = true;
      discardWorkerQueue(channel.parentQueue);
      discardWorkerQueue(channel.childQueue);
      if (channel.child) {
        channel.child->isolate->TerminateExecution();
        wakeWorker(channel.childPort, 0);
      }
    }
    if (channel.child == state) {
      channel.child = nullptr;
      channel.childPort = 0;
      channel.finished = true;
      discardWorkerQueue(channel.childQueue);
      isolate->CancelTerminateExecution();
    }
    if (!channel.child && (channel.closed || !channel.parent)) {
      discardWorkerQueue(channel.parentQueue);
      it = workerChannels.erase(it);
    } else {
      ++it;
    }
  }
}
}  // namespace flax
