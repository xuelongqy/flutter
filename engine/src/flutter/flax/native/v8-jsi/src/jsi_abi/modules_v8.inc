// Included after V8Scope: every entry uses its owning Context and ordinary FFI
// stack.
struct FlaxModuleFailure {};

template <class T>
v8::Local<T> moduleLocal(v8::MaybeLocal<T> value) {
  v8::Local<T> result;
  if (!value.ToLocal(&result))
    throw FlaxModuleFailure{};
  return result;
}

struct FlaxModuleRecord {
  std::string key, url;
  int kind = 0;
  int identity = 0;
  v8::Global<v8::Module> module;
  v8::Global<v8::Value> payload;
  v8::Global<v8::Promise> evaluation;
  std::vector<std::string> exports;
  std::map<std::tuple<std::string, std::string, int>, std::string> edges;
};

struct FlaxModules {
  JsiRuntimeState* state;
  bool closed = false;
  v8::Global<v8::Function> importer;
  v8::Global<v8::Function> wasmInstance, wasmImports, wasmExports,
      wasmInstanceExports;
  v8::Global<v8::Value> lastFailure;
  std::unordered_map<std::string, std::shared_ptr<FlaxModuleRecord>> records;

  FlaxModules(JsiRuntimeState* state, v8::Local<v8::Function> callback)
      : state(state), importer(state->isolate, callback) {
#ifdef V8_ENABLE_WEBASSEMBLY
    auto context = state->getContextLocal();
    auto wasm =
        moduleLocal(context->Global()->Get(context, str("WebAssembly")));
    if (!wasm->IsObject())
      throw std::runtime_error("WebAssembly is unavailable");
    auto object = wasm.As<v8::Object>();
    auto instance = moduleLocal(object->Get(context, str("Instance")));
    auto module = moduleLocal(object->Get(context, str("Module")));
    if (!instance->IsFunction() || !module->IsFunction())
      throw std::runtime_error("Invalid WebAssembly intrinsics");
    auto imports =
        moduleLocal(module.As<v8::Object>()->Get(context, str("imports")));
    auto exports =
        moduleLocal(module.As<v8::Object>()->Get(context, str("exports")));
    if (!imports->IsFunction() || !exports->IsFunction())
      throw std::runtime_error("Invalid WebAssembly reflection intrinsics");
    wasmInstance.Reset(state->isolate, instance.As<v8::Function>());
    auto prototype =
        moduleLocal(instance.As<v8::Object>()->Get(context, str("prototype")));
    if (!prototype->IsObject())
      throw std::runtime_error("Invalid Wasm instance prototype");
    auto value =
        moduleLocal(prototype.As<v8::Object>()->GetOwnPropertyDescriptor(
            context, str("exports")));
    if (!value->IsObject())
      throw std::runtime_error("Invalid Wasm exports descriptor");
    auto descriptor = value.As<v8::Object>();
    auto getter = moduleLocal(descriptor->Get(context, str("get")));
    if (!getter->IsFunction())
      throw std::runtime_error("Invalid Wasm exports getter");
    wasmInstanceExports.Reset(state->isolate, getter.As<v8::Function>());
    wasmImports.Reset(state->isolate, imports.As<v8::Function>());
    wasmExports.Reset(state->isolate, exports.As<v8::Function>());
#endif
  }

  v8::Local<v8::String> str(const std::string& value) {
    return moduleLocal(v8::String::NewFromUtf8(state->isolate, value.data(),
                                               v8::NewStringType::kNormal,
                                               static_cast<int>(value.size())));
  }

  static std::string text(v8::Isolate* isolate, v8::Local<v8::Value> value) {
    if (!value->IsString())
      throw std::invalid_argument("Expected a module string");
    v8::String::Utf8Value bytes(isolate, value);
    return {*bytes, static_cast<size_t>(bytes.length())};
  }

  static FlaxModules* current(v8::Local<v8::Context> context) {
    if (context->GetNumberOfEmbedderDataFields() <=
        v8rt::ContextEmbedderIndex::kRuntime)
      return nullptr;
    auto* owner = static_cast<JsiRuntimeState*>(
        context->GetAlignedPointerFromEmbedderData(
            v8rt::ContextEmbedderIndex::kRuntime,
            v8::kEmbedderDataTypeTagDefault));
    return owner ? owner->modules.get() : nullptr;
  }

  std::shared_ptr<FlaxModuleRecord> get(const std::string& key) {
    if (closed)
      throw std::runtime_error("Flax module loader is closed");
    auto found = records.find(key);
    if (found == records.end())
      throw std::invalid_argument("Module is not loaded: " + key);
    return found->second;
  }

  std::shared_ptr<FlaxModuleRecord> find(v8::Local<v8::Module> module) {
    const auto identity = module->GetIdentityHash();
    for (auto& entry : records) {
      auto& record = entry.second;
      if (record->identity == identity &&
          record->module.Get(state->isolate) == module)
        return record;
    }
    throw std::runtime_error("Module belongs to another loader");
  }

  static std::string attribute(v8::Local<v8::Context> context,
                               v8::Local<v8::FixedArray> values,
                               bool locations) {
    std::string result;
    const int width = locations ? 3 : 2;
    if (values->Length() % width)
      throw std::invalid_argument("Invalid import attributes");
    for (int i = 0; i < values->Length(); i += width) {
      auto key =
          text(v8::Isolate::GetCurrent(), values->Get(i).As<v8::Value>());
      auto value =
          text(v8::Isolate::GetCurrent(), values->Get(i + 1).As<v8::Value>());
      if (key != "type" || value != "json" || !result.empty())
        throw std::invalid_argument("Unsupported import attribute: " + key);
      result = value;
    }
    return result;
  }

  static void error(v8::Isolate* isolate, const std::exception& exception) {
    auto message = v8::String::NewFromUtf8(isolate, exception.what());
    v8::Local<v8::String> value;
    if (message.ToLocal(&value))
      isolate->ThrowException(v8::Exception::TypeError(value));
  }

  static v8::MaybeLocal<v8::Module> resolve(
      v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes,
      v8::Local<v8::Module> referrer) {
    try {
      auto* self = current(context);
      if (!self || self->closed)
        throw std::runtime_error("Module loader is closed");
      auto parent = self->find(referrer);
      auto type = attribute(context, attributes, true);
      auto edge = parent->edges.find(
          {text(v8::Isolate::GetCurrent(), specifier), type, 0});
      if (edge == parent->edges.end())
        throw std::runtime_error("Unresolved module import");
      auto target = self->get(edge->second);
      if ((type == "json") != (target->kind == 1))
        throw std::invalid_argument("JSON imports require type: json");
      return target->module.Get(v8::Isolate::GetCurrent());
    } catch (const std::exception& e) {
      error(v8::Isolate::GetCurrent(), e);
    }
    return {};
  }

  static v8::MaybeLocal<v8::Object> resolveSource(
      v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes,
      v8::Local<v8::Module> referrer) {
    try {
      auto* self = current(context);
      if (!self || self->closed)
        throw std::runtime_error("Module loader is closed");
      auto parent = self->find(referrer);
      auto type = attribute(context, attributes, true);
      auto edge = parent->edges.find(
          {text(v8::Isolate::GetCurrent(), specifier), type, 1});
      if (edge == parent->edges.end())
        throw std::runtime_error("Unresolved source import");
      auto target = self->get(edge->second);
      if (target->kind != 2 || !type.empty())
        throw std::invalid_argument("Only Wasm supports source imports");
      return target->payload.Get(v8::Isolate::GetCurrent()).As<v8::Object>();
    } catch (const std::exception& e) {
      error(v8::Isolate::GetCurrent(), e);
    }
    return {};
  }

  static v8::MaybeLocal<v8::Promise> synthetic(v8::Local<v8::Context> context,
                                               v8::Local<v8::Module> module) {
    try {
      auto* self = current(context);
      if (!self || self->closed)
        throw std::runtime_error("Module loader is closed");
      auto record = self->find(module);
      auto payload = record->payload.Get(v8::Isolate::GetCurrent());
      for (const auto& name : record->exports) {
        auto value = record->kind == 1
                         ? payload
                         : moduleLocal(payload.As<v8::Object>()->Get(
                               context, self->str(name)));
        if (self->closed)
          throw std::runtime_error("Module loader closed during evaluation");
        if (module
                ->SetSyntheticModuleExport(v8::Isolate::GetCurrent(),
                                           self->str(name), value)
                .IsNothing())
          return {};
      }
      auto promise = moduleLocal(v8::Promise::Resolver::New(context));
      if (promise->Resolve(context, v8::Undefined(v8::Isolate::GetCurrent()))
              .IsNothing())
        return {};
      return promise->GetPromise();
    } catch (const FlaxModuleFailure&) {
      return {};
    } catch (const std::exception& e) {
      error(v8::Isolate::GetCurrent(), e);
    }
    return {};
  }

  static void instantiateWasm(const v8::FunctionCallbackInfo<v8::Value>& info) {
    try {
      auto* self = current(info.GetIsolate()->GetCurrentContext());
      if (!self || self->closed)
        throw std::runtime_error("Module loader is closed");
      if (info.Length() != 2)
        throw std::invalid_argument("Invalid Wasm instantiation");
      auto context = self->state->getContextLocal();
      v8::Local<v8::Value> arguments[] = {info[0], info[1]};
      auto instance = moduleLocal(self->wasmInstance.Get(info.GetIsolate())
                                      ->NewInstance(context, 2, arguments));
      info.GetReturnValue().Set(
          moduleLocal(self->wasmInstanceExports.Get(info.GetIsolate())
                          ->Call(context, instance, 0, nullptr)));
    } catch (const FlaxModuleFailure&) {
    } catch (const std::exception& e) {
      error(info.GetIsolate(), e);
    }
  }

  static void importMeta(v8::Local<v8::Context> context,
                         v8::Local<v8::Module> module,
                         v8::Local<v8::Object> meta) {
    try {
      auto* self = current(context);
      if (!self || self->closed)
        throw std::runtime_error("Module loader is closed");
      auto record = self->find(module);
      if (meta->CreateDataProperty(context, self->str("url"),
                                   self->str(record->url))
              .IsNothing())
        return;
      if (record->kind == 2) {
        auto instantiate =
            moduleLocal(v8::Function::New(context, instantiateWasm));
        if (meta->CreateDataProperty(
                    context, self->str("__flaxWasm"),
                    record->payload.Get(v8::Isolate::GetCurrent()))
                .IsNothing())
          return;
        meta->CreateDataProperty(context, self->str("__flaxInstantiate"),
                                 instantiate)
            .FromMaybe(false);
      }
    } catch (const FlaxModuleFailure&) {
    } catch (const std::exception& e) {
      error(v8::Isolate::GetCurrent(), e);
    }
  }

  static v8::MaybeLocal<v8::Promise> importWithPhase(
      v8::Local<v8::Context> context,
      v8::Local<v8::Data> options,
      v8::Local<v8::Value> resource,
      v8::Local<v8::String> specifier,
      v8::ModuleImportPhase phase,
      v8::Local<v8::FixedArray> attributes) {
    try {
      auto* self = current(context);
      if (!self || self->closed)
        throw std::runtime_error("Native ESM is not active");
      if (phase != v8::ModuleImportPhase::kSource &&
          phase != v8::ModuleImportPhase::kEvaluation)
        throw std::invalid_argument("Unsupported module import phase");
      auto type = attribute(context, attributes, false);
      if (!options.IsEmpty() && options->IsFixedArray()) {
        auto values = options.As<v8::PrimitiveArray>();
        if (values->Length() == 1) {
          auto key = text(v8::Isolate::GetCurrent(),
                          values->Get(v8::Isolate::GetCurrent(), 0));
          resource = self->str(self->get(key)->url);
        }
      }
      v8::Local<v8::Value> arguments[] = {
          resource, specifier, self->str(type),
          v8::Integer::New(v8::Isolate::GetCurrent(),
                           phase == v8::ModuleImportPhase::kSource ? 1 : 0)};
      auto result = moduleLocal(
          self->importer.Get(v8::Isolate::GetCurrent())
              ->Call(context, v8::Undefined(v8::Isolate::GetCurrent()), 4,
                     arguments));
      if (!result->IsPromise())
        throw std::runtime_error("Importer must return a Promise");
      return result.As<v8::Promise>();
    } catch (const FlaxModuleFailure&) {
      return {};
    } catch (const std::exception& e) {
      v8::Local<v8::Promise::Resolver> promise;
      v8::Local<v8::String> message;
      if (!v8::Promise::Resolver::New(context).ToLocal(&promise) ||
          !v8::String::NewFromUtf8(v8::Isolate::GetCurrent(), e.what())
               .ToLocal(&message))
        return {};
      if (promise->Reject(context, v8::Exception::TypeError(message))
              .IsNothing())
        return {};
      return promise->GetPromise();
    }
  }

  static v8::MaybeLocal<v8::Promise> importDynamic(
      v8::Local<v8::Context> context,
      v8::Local<v8::Data> options,
      v8::Local<v8::Value> resource,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes) {
    return importWithPhase(context, options, resource, specifier,
                           v8::ModuleImportPhase::kEvaluation, attributes);
  }

  std::string quote(const std::string& text) {
    return FlaxModules::text(
        state->isolate,
        moduleLocal(v8::JSON::Stringify(state->getContextLocal(), str(text))));
  }

  std::string wasmWrapper(v8::Local<v8::Value> compiled) {
    auto context = state->getContextLocal();
    v8::Local<v8::Value> argument[] = {compiled};
    auto imported = moduleLocal(
        wasmImports.Get(state->isolate)
            ->Call(context, v8::Undefined(state->isolate), 1, argument));
    auto exported = moduleLocal(
        wasmExports.Get(state->isolate)
            ->Call(context, v8::Undefined(state->isolate), 1, argument));
    if (!imported->IsArray() || !exported->IsArray())
      throw std::runtime_error("Invalid Wasm reflection result");
    auto imports = imported.As<v8::Array>();
    auto exports = exported.As<v8::Array>();
    std::map<std::string, size_t> dependencies;
    std::ostringstream code;
    for (uint32_t i = 0; i < imports->Length(); ++i) {
      auto value = moduleLocal(imports->Get(context, i));
      if (!value->IsObject())
        throw std::runtime_error("Invalid Wasm import descriptor");
      auto item = value.As<v8::Object>();
      auto name =
          text(state->isolate, moduleLocal(item->Get(context, str("module"))));
      if (dependencies.count(name))
        continue;
      const size_t index = dependencies.size();
      dependencies.emplace(name, index);
      code << "import * as d" << index << " from " << quote(name) << ";\n";
    }
    code << "const e = import.meta.__flaxInstantiate(import.meta.__flaxWasm, {";
    for (auto& dependency : dependencies)
      code << "[" << quote(dependency.first) << "]:d" << dependency.second
           << ",";
    code << "});\n";
    for (uint32_t i = 0; i < exports->Length(); ++i) {
      auto value = moduleLocal(exports->Get(context, i));
      if (!value->IsObject())
        throw std::runtime_error("Invalid Wasm export descriptor");
      auto item = value.As<v8::Object>();
      auto name =
          text(state->isolate, moduleLocal(item->Get(context, str("name"))));
      code << "const e" << i << "=e[" << quote(name) << "];export{e" << i
           << " as " << quote(name) << "};\n";
    }
    return code.str();
  }

  v8::Local<v8::Array> requests(
      const std::shared_ptr<FlaxModuleRecord>& record) {
    auto context = state->getContextLocal();
    auto list = record->module.Get(state->isolate)->GetModuleRequests();
    if (list->Length() > 256)
      throw std::invalid_argument("Module dependency limit exceeded");
    auto result = v8::Array::New(state->isolate, list->Length());
    for (int i = 0; i < list->Length(); ++i) {
      auto request = list->Get(i).As<v8::ModuleRequest>();
      const auto phase = request->GetPhase();
      if (phase != v8::ModuleImportPhase::kSource &&
          phase != v8::ModuleImportPhase::kEvaluation)
        throw std::invalid_argument("Unsupported module import phase");
      auto type = attribute(context, request->GetImportAttributes(), true);
      v8::Local<v8::Value> values[] = {
          request->GetSpecifier(), str(type),
          v8::Integer::New(state->isolate,
                           phase == v8::ModuleImportPhase::kSource ? 1 : 0)};
      auto item = v8::Array::New(state->isolate, values, 3);
      if (result->Set(context, i, item).IsNothing())
        throw FlaxModuleFailure{};
    }
    return result;
  }

  void compile(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if ((info.Length() != 4 && info.Length() != 5) || !info[2]->IsInt32())
      throw std::invalid_argument("Invalid module compilation request");
    auto context = state->getContextLocal();
    auto key = text(state->isolate, info[0]);
    if (records.count(key))
      throw std::invalid_argument("Module is already compiled");
    if (records.size() >= 256)
      throw std::invalid_argument("Module graph limit exceeded");
    auto record = std::make_shared<FlaxModuleRecord>();
    record->key = key;
    record->url = text(state->isolate, info[1]);
    record->kind = info[2].As<v8::Int32>()->Value();
    v8::Local<v8::Module> module;
    if (record->kind == 0 || record->kind == 2) {
      v8::Local<v8::String> source;
      if (record->kind == 0) {
        if (!info[3]->IsString())
          throw std::invalid_argument("Expected JavaScript source");
        source = info[3].As<v8::String>();
      } else {
#ifdef V8_ENABLE_WEBASSEMBLY
        if (!info[3]->IsArrayBuffer())
          throw std::invalid_argument("Expected Wasm bytes");
        auto buffer = info[3].As<v8::ArrayBuffer>();
        auto storage = buffer->GetBackingStore();
        if (buffer->WasDetached() || storage->IsShared() ||
            storage->IsResizableByUserJavaScript())
          throw std::invalid_argument("Expected fixed, attached Wasm bytes");
        auto compiled = moduleLocal(v8::WasmModuleObject::Compile(
            state->isolate, {static_cast<const uint8_t*>(storage->Data()),
                             storage->ByteLength()}));
        record->payload.Reset(state->isolate, compiled);
        source = str(wasmWrapper(compiled));
#else
        throw std::runtime_error("This SDK does not include WebAssembly");
#endif
      }
      auto options = v8::PrimitiveArray::New(state->isolate, 1);
      options->Set(state->isolate, 0, str(record->key));
      auto diagnostic = info.Length() == 5 ? str(text(state->isolate, info[4]))
                                           : str(record->url);
      v8::ScriptOrigin origin(diagnostic, 0, 0, false, -1, {}, false, false,
                              true, options);
      v8::ScriptCompiler::Source input(source, origin);
      module = moduleLocal(
          v8::ScriptCompiler::CompileModule(state->isolate, &input));
    } else if (record->kind == 1 || record->kind == 3) {
      v8::Local<v8::Value> payload;
      if (record->kind == 1) {
        if (!info[3]->IsString())
          throw std::invalid_argument("Expected JSON text");
        payload =
            moduleLocal(v8::JSON::Parse(context, info[3].As<v8::String>()));
        record->exports.push_back("default");
      } else {
        if (!info[3]->IsObject())
          throw std::invalid_argument("Expected host exports");
        payload = info[3];
        auto names = moduleLocal(payload.As<v8::Object>()->GetOwnPropertyNames(
            context, v8::ONLY_ENUMERABLE));
        for (uint32_t i = 0; i < names->Length(); ++i)
          record->exports.push_back(
              text(state->isolate, moduleLocal(names->Get(context, i))));
      }
      record->payload.Reset(state->isolate, payload);
      std::vector<v8::Local<v8::String>> names;
      for (auto& name : record->exports)
        names.push_back(str(name));
      module = v8::Module::CreateSyntheticModule(
          state->isolate, str(record->url),
          std::span<const v8::Local<v8::String>>(names), synthetic);
    } else {
      throw std::invalid_argument("Unsupported module kind");
    }
    if (closed)
      throw std::runtime_error("Module loader closed during compilation");
    record->module.Reset(state->isolate, module);
    record->identity = module->GetIdentityHash();
    records.emplace(key, record);
    try {
      info.GetReturnValue().Set(requests(record));
    } catch (...) {
      records.erase(key);
      throw;
    }
  }

  void bind(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() != 5 || !info[3]->IsInt32())
      throw std::invalid_argument("Invalid module edge");
    auto parent = get(text(state->isolate, info[0]));
    auto target = get(text(state->isolate, info[4]));
    auto specifier = text(state->isolate, info[1]);
    auto type = text(state->isolate, info[2]);
    auto phase = info[3].As<v8::Int32>()->Value();
    if ((phase != 0 && phase != 1) || (type != "" && type != "json") ||
        ((type == "json") != (target->kind == 1)) ||
        (phase == 1 && target->kind != 2))
      throw std::invalid_argument("Module type or import phase mismatch");
    const auto edge = std::make_tuple(specifier, type, phase);
    auto existing = parent->edges.find(edge);
    if (existing != parent->edges.end() && existing->second != target->key)
      throw std::invalid_argument("Module resolution changed within a Session");
    parent->edges[edge] = target->key;
  }

  static void returnData(const v8::FunctionCallbackInfo<v8::Value>& info) {
    info.GetReturnValue().Set(info.Data());
  }

  void evaluate(const v8::FunctionCallbackInfo<v8::Value>& info) {
    if (info.Length() != 2 || !info[1]->IsInt32())
      throw std::invalid_argument("Invalid module evaluation");
    auto record = get(text(state->isolate, info[0]));
    auto context = state->getContextLocal();
    const auto phase = info[1].As<v8::Int32>()->Value();
    if (phase == 1) {
      if (record->kind != 2)
        throw std::invalid_argument("Only Wasm supports source imports");
      auto result = moduleLocal(v8::Promise::Resolver::New(context));
      if (result->Resolve(context, record->payload.Get(state->isolate))
              .IsNothing())
        throw FlaxModuleFailure{};
      info.GetReturnValue().Set(result->GetPromise());
      return;
    }
    if (phase != 0)
      throw std::invalid_argument("Unsupported import phase");
    auto module = record->module.Get(state->isolate);
    if (module->GetStatus() == v8::Module::kUninstantiated &&
        !module->InstantiateModule(context, resolve, resolveSource)
             .FromMaybe(false))
      throw FlaxModuleFailure{};
    if (module->GetStatus() == v8::Module::kErrored) {
      state->isolate->ThrowException(module->GetException());
      throw FlaxModuleFailure{};
    }
    if (record->evaluation.IsEmpty()) {
      auto evaluated = moduleLocal(module->Evaluate(context));
      // A namespace can export "then"; entry readiness must not assimilate it.
      v8::Local<v8::Value> values[] = {module->GetModuleNamespace()};
      auto result = v8::Object::New(state->isolate);
      if (result->SetPrototype(context, v8::Null(state->isolate)).IsNothing() ||
          result->CreateDataProperty(context, 0, values[0]).IsNothing())
        throw FlaxModuleFailure{};
      auto finish = moduleLocal(v8::Function::New(context, returnData, result));
      record->evaluation.Reset(state->isolate,
                               moduleLocal(evaluated->Then(context, finish)));
    }
    info.GetReturnValue().Set(record->evaluation.Get(state->isolate));
  }

  static void dispatch(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* self = current(info.GetIsolate()->GetCurrentContext());
    v8::TryCatch caught(info.GetIsolate());
    try {
      if (!self)
        throw std::runtime_error("Module loader is unavailable");
      const auto operation = info.Data().As<v8::Int32>()->Value();
      if (operation == 4) {
        if (!self->lastFailure.IsEmpty()) {
          v8::Local<v8::Value> value[] = {
              self->lastFailure.Get(info.GetIsolate())};
          info.GetReturnValue().Set(
              v8::Array::New(info.GetIsolate(), value, 1));
          self->lastFailure.Reset();
        }
        return;
      }
      self->lastFailure.Reset();
      if (operation == 3) {
        self->closed = true;
        self->records.clear();
        self->importer.Reset();
        return;
      }
      if (self->closed)
        throw std::runtime_error("Module loader is closed");
      switch (operation) {
        case 0:
          self->compile(info);
          break;
        case 1:
          self->bind(info);
          break;
        case 2:
          self->evaluate(info);
          break;
        default:
          throw std::invalid_argument("Unknown module operation");
      }
    } catch (const FlaxModuleFailure&) {
    } catch (const std::exception& e) {
      error(info.GetIsolate(), e);
    }
    if (caught.HasCaught()) {
      if (self)
        self->lastFailure.Reset(info.GetIsolate(), caught.Exception());
      caught.ReThrow();
    }
  }

  void install(v8::Local<v8::Object> controller) {
    const char* names[] = {"compile", "bind", "evaluate", "close", "takeError"};
    auto context = state->getContextLocal();
    for (int i = 0; i < 5; ++i) {
      auto function = moduleLocal(v8::Function::New(
          context, dispatch, v8::Integer::New(state->isolate, i)));
      if (controller->CreateDataProperty(context, str(names[i]), function)
              .IsNothing())
        throw FlaxModuleFailure{};
    }
  }
};
