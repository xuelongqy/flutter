// Real Dart VM + public V8 CppHeap experiment; no shadow objects or additional
// JS heap scanner. V8 still performs its ordinary GC tracing.
#include <v8config.h>
// Match the already verified SDK's calling convention.
#undef V8_PRESERVE_MOST
#define V8_PRESERVE_MOST
#include <cppgc/allocation.h>
#include <cppgc/persistent.h>
#include <dart_api_dl.h>
#include <dart_flax_api.h>
#include <dlfcn.h>
#include <libplatform/libplatform.h>
#include <v8-cppgc.h>
#include <v8.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

static decltype(&Dart_FlaxSetCollector) setCollector;
static decltype(&Dart_FlaxRegisterHandle) registerHandle;
static decltype(&Dart_FlaxCollect) collectDart;
template <typename T>
T api(const char* name) {
  auto value = reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
  if (!value)
    throw std::runtime_error(std::string("Patched VM export missing: ") + name);
  return value;
}
static void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
struct Probe;
struct Cell;
struct Peer final : v8::Object::Wrappable {
  Probe* probe;
  Cell* cell;
  v8::TracedReference<v8::Function> callback;
  Peer(Probe* p, Cell* c) : probe(p), cell(c) {}
  ~Peer();
  void Trace(cppgc::Visitor* visitor) const;
};
struct Cell {
  int id;
  Dart_WeakPersistentHandle target = nullptr, origin = nullptr;
  cppgc::Persistent<Peer> root;
  cppgc::WeakPersistent<Peer> peer;
  v8::Global<v8::Object> wrapper;
  std::atomic<bool> targetDead{false}, originDead{false}, cppDead{false};
  explicit Cell(int value) : id(value) {}
};
Peer::~Peer() {
  cell->cppDead.store(true);
}
constexpr auto tag = v8::CppHeapPointerTag::kFirstObjectWrappableTag;
using Tick = double (*)(double);

struct Probe {
  v8::Isolate* isolate = nullptr;
  v8::CppHeap* cppHeap = nullptr;  // Owned by the V8 isolate.
  std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
  v8::Global<v8::Context> context;
  std::map<int, std::unique_ptr<Cell>> cells;
  std::unordered_map<Dart_WeakPersistentHandle, Cell*> origins;
  Tick tick;
  std::string error;
  std::thread::id owner = std::this_thread::get_id();
  const Dart_FlaxGC* joint = nullptr;
  int collections = 0, traceCount = 0, jitCount = 0, activeCalls = 0;
  bool registered = false;
  struct Scope {
    v8::Isolate::Scope entered;
    v8::HandleScope handles;
    v8::Local<v8::Context> context;
    v8::Context::Scope current;
    explicit Scope(Probe& p)
        : entered(p.isolate),
          handles(p.isolate),
          context(p.context.Get(p.isolate)),
          current(context) {}
  };
  static std::unique_ptr<v8::Platform>& platform() {
    static std::unique_ptr<v8::Platform> value;
    static std::once_flag once;
    std::call_once(once, [] {
      // JIT remains enabled. Expose GC and optimization intrinsics only for
      // proof.
      v8::V8::SetFlagsFromString("--expose-gc --allow-natives-syntax");
      value = v8::platform::NewDefaultPlatform();
      v8::V8::InitializePlatform(value.get());
      require(v8::V8::Initialize(), "V8 initialization failed");
    });
    return value;
  }
  explicit Probe(Tick callback) : tick(callback) {
    auto& p = platform();
    allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
    v8::CppHeapCreateParams cppParams({});
    // ponytail: atomic tracing keeps the experimental Dart marker on its owner;
    // concurrent collection needs an explicit handshake, not background
    // reentry.
    cppParams.marking_support = cppgc::Heap::MarkingType::kAtomic;
    cppParams.sweeping_support = cppgc::Heap::SweepingType::kAtomic;
    auto heap = v8::CppHeap::Create(p.get(), cppParams);
    v8::Isolate::CreateParams params;
    params.array_buffer_allocator = allocator.get();
    params.cpp_heap = heap.release();
    isolate = v8::Isolate::New(params);
    cppHeap = isolate->GetCppHeap();
    isolate->SetData(0, this);
    v8::Isolate::Scope entered(isolate);
    v8::HandleScope handles(isolate);
    context.Reset(isolate, v8::Context::New(isolate));
    auto ctx = context.Get(isolate);
    v8::Context::Scope current(ctx);
    auto native = v8::Function::New(ctx, nativeTick).ToLocalChecked();
    ctx->Global()->Set(ctx, text("nativeTick"), native).Check();
    ctx->Global()->Set(ctx, text("business"), v8::Array::New(isolate)).Check();
    isolate->SetJitCodeEventHandler(
        v8::kJitCodeEventDefault, [](const v8::JitCodeEvent* event) {
          if (event->type != v8::JitCodeEvent::CODE_ADDED ||
              event->code_type != v8::JitCodeEvent::JIT_CODE)
            return;
          if (std::string(event->name.str, event->name.len)
                  .find("flaxJitHot") != std::string::npos)
            static_cast<Probe*>(event->isolate->GetData(0))->jitCount++;
        });
    eval(
        "function flaxJitHot(x) { return (x * 17 + 3) | 0; }"
        "%PrepareFunctionForOptimization(flaxJitHot);"
        "for (let i=0;i<10000;i++) flaxJitHot(i);"
        "%OptimizeFunctionOnNextCall(flaxJitHot); flaxJitHot(11);");
    require(jitCount > 0, "No native JIT code event for flaxJitHot");
    registered = setCollector(jointCollect, this);
    require(registered, "Collector already registered in this isolate group");
  }
  v8::Local<v8::String> text(const char* s) {
    return v8::String::NewFromUtf8(isolate, s).ToLocalChecked();
  }
  v8::Local<v8::Value> eval(const char* source) {
    auto ctx = context.Get(isolate);
    v8::TryCatch caught(isolate);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (!v8::Script::Compile(ctx, text(source)).ToLocal(&script) ||
        !script->Run(ctx).ToLocal(&result)) {
      v8::String::Utf8Value msg(isolate, caught.Exception());
      throw std::runtime_error(*msg ? *msg : "JavaScript failed");
    }
    return result;
  }
  static void nativeTick(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto* p = static_cast<Probe*>(info.GetIsolate()->GetData(0));
    if (info.Length() != 1 || !info[0]->IsObject()) {
      info.GetIsolate()->ThrowException(p->text("JS peer collected"));
      return;
    }
    auto* peer = v8::Object::Unwrap<tag, Peer>(info.GetIsolate(),
                                               info[0].As<v8::Object>());
    if (!peer) {
      info.GetIsolate()->ThrowException(p->text("Invalid peer"));
      return;
    }
    Dart_EnterScope_DL();
    auto target = Dart_HandleFromWeakPersistent_DL(peer->cell->target);
    if (Dart_IsNull_DL(target)) {
      Dart_ExitScope_DL();
      info.GetIsolate()->ThrowException(p->text("Dart peer collected"));
      return;
    }
    const auto value = p->tick(peer->cell->id);
    Dart_ExitScope_DL();
    info.GetReturnValue().Set(value);
  }
  void make(int id,
            Dart_Handle target,
            Dart_Handle origin,
            bool jsRoot,
            int mode) {
    require(!cells.contains(id), "Duplicate peer");
    auto c = std::make_unique<Cell>(id);
    c->target = Dart_NewWeakPersistentHandle_DL(
        target, c.get(), 0,
        [](void*, void* p) { static_cast<Cell*>(p)->targetDead.store(true); });
    c->origin = Dart_NewWeakPersistentHandle_DL(
        origin, c.get(), 0,
        [](void*, void* p) { static_cast<Cell*>(p)->originDead.store(true); });
    require(c->target && c->origin, "Weak handle creation failed");
    require(registerHandle(c->target, false) && registerHandle(c->origin, true),
            "Traceable handle registration failed");
    Scope scope(*this);
    auto* peer = cppgc::MakeGarbageCollected<Peer>(
        cppHeap->GetAllocationHandle(), this, c.get());
    auto factory = v8::FunctionTemplate::New(isolate)
                       ->GetFunction(scope.context)
                       .ToLocalChecked();
    auto wrapper = factory->NewInstance(scope.context).ToLocalChecked();
    v8::Object::Wrap<tag>(isolate, wrapper, peer);
    auto build =
        eval(mode == 3
                 // Separate factory: the other branches capture `w` in a shared
                 // function context, which would turn this into a strong edge.
                 ? "(function(w) { const weak=new WeakRef(w); "
                   "return ()=>nativeTick(weak.deref()); })"
                 : "(function(w, mode) {"
                   "if(mode===1) { const box = new class { #peer; "
                   "constructor(){this.#peer=w} get(){return this.#peer} };"
                   " return ()=>nativeTick(box.get()); }"
                   "if(mode===2) { const key={}; const map=new "
                   "WeakMap([[key,w]]); "
                   "return ()=>nativeTick(map.get(key)); }"
                   "return ()=>nativeTick(w); })")
            .As<v8::Function>();
    v8::Local<v8::Value> args[] = {wrapper, v8::Integer::New(isolate, mode)};
    auto callback = build->Call(scope.context, v8::Undefined(isolate), 2, args)
                        .ToLocalChecked()
                        .As<v8::Function>();
    peer->callback.Reset(isolate, callback);
    c->peer = peer;
    c->root = peer;  // Conservative only between joint major collections.
    c->wrapper.Reset(isolate, wrapper);
    c->wrapper.SetWeak();
    if (jsRoot)
      scope.context->Global()
          ->Get(scope.context, text("business"))
          .ToLocalChecked()
          .As<v8::Array>()
          ->Set(scope.context, id, callback)
          .Check();
    origins.emplace(c->origin, c.get());
    cells.emplace(id, std::move(c));
  }
  static void jointCollect(void* value, const Dart_FlaxGC* gc) {
    auto& p = *static_cast<Probe*>(value);
    if (std::this_thread::get_id() != p.owner || p.joint)
      std::abort();
    p.joint = gc;
    for (auto& [id, c] : p.cells)
      c->root.Clear();
    while (auto origin = gc->next_marked_origin(gc->context)) {
      auto* c = p.origins.at(origin);
      if (!c->peer)
        std::abort();
      c->root = c->peer.Get();
    }
    v8::Isolate::Scope entered(p.isolate);
    p.isolate->ClearKeptObjects();
    p.isolate->RequestGarbageCollectionForTesting(
        v8::Isolate::kFullGarbageCollection,
        p.activeCalls ? cppgc::EmbedderStackState::kMayContainHeapPointers
                      : cppgc::EmbedderStackState::kNoHeapPointers);
    p.cppHeap->FinishSweepingForTesting();
    for (auto& [id, c] : p.cells) {
      if (gc->is_marked(gc->context, c->origin)) {
        if (!c->peer)
          std::abort();
        c->root = c->peer.Get();
      }
    }
    p.joint = nullptr;
    p.collections++;
  }
  void trace(Cell* c, cppgc::Visitor* visitor) {
    if (!joint)
      return;
    if (std::this_thread::get_id() != owner)
      std::abort();
    traceCount++;
    joint->mark(joint->context, c->target);
    while (auto origin = joint->next_marked_origin(joint->context)) {
      auto* found = origins.at(origin);
      if (!found->peer)
        std::abort();
      visitor->Trace(cppgc::Member<Peer>(found->peer.Get()));
    }
  }
  double call(int id, bool fromJs) {
    Scope scope(*this);
    v8::TryCatch caught(isolate);
    v8::Local<v8::Value> value;
    if (fromJs)
      value = scope.context->Global()
                  ->Get(scope.context, text("business"))
                  .ToLocalChecked()
                  .As<v8::Array>()
                  ->Get(scope.context, id)
                  .ToLocalChecked();
    else {
      require(cells.at(id)->peer.Get() != nullptr, "JS peer collected");
      value = cells.at(id)->peer->callback.Get(isolate);
    }
    require(value->IsFunction(), "JS callback missing");
    v8::Local<v8::Value> result;
    activeCalls++;
    bool ok = value.As<v8::Function>()
                  ->Call(scope.context, v8::Undefined(isolate), 0, nullptr)
                  .ToLocal(&result);
    activeCalls--;
    if (!ok) {
      v8::String::Utf8Value msg(isolate, caught.Exception());
      throw std::runtime_error(*msg ? *msg : "Callback failed");
    }
    return result->NumberValue(scope.context).FromMaybe(NAN);
  }
  ~Probe() {
    if (registered && !setCollector(nullptr, nullptr))
      std::abort();
    for (auto& [id, c] : cells) {
      c->root.Clear();
      c->wrapper.Reset();
      c->peer.Clear();
    }
    context.Reset();
    isolate->Dispose();
    cppHeap = nullptr;
    for (auto& [id, c] : cells) {
      if (c->target)
        Dart_DeleteWeakPersistentHandle_DL(c->target);
      if (c->origin)
        Dart_DeleteWeakPersistentHandle_DL(c->origin);
    }
  }
};
void Peer::Trace(cppgc::Visitor* visitor) const {
  visitor->Trace(callback);
  probe->trace(cell, visitor);
}

extern "C" int flax_gc_init(void* data) {
  if (Dart_InitializeApiDL(data) != 0)
    return -1;
  try {
    setCollector = api<decltype(setCollector)>("Dart_FlaxSetCollector");
    registerHandle = api<decltype(registerHandle)>("Dart_FlaxRegisterHandle");
    collectDart = api<decltype(collectDart)>("Dart_FlaxCollect");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return -2;
  }
}
extern "C" Probe* flax_gc_create(Tick tick) {
  return new Probe(tick);
}
extern "C" int flax_gc_make(Probe* p,
                            Dart_Handle target,
                            Dart_Handle origin,
                            int id,
                            int js,
                            int mode) {
  try {
    p->make(id, target, origin, js, mode);
    return 0;
  } catch (const std::exception& e) {
    p->error = e.what();
    return -1;
  }
}
extern "C" double flax_gc_call(Probe* p, int id, int js) {
  try {
    return p->call(id, js);
  } catch (const std::exception& e) {
    p->error = e.what();
    return NAN;
  }
}
extern "C" const char* flax_gc_error(Probe* p) {
  return p->error.c_str();
}
extern "C" void flax_gc_collect(int young, int compact) {
  collectDart(young, compact);
}
extern "C" int flax_gc_alive(Probe* p, int id) {
  auto& c = *p->cells.at(id);
  return (!c.targetDead.load() ? 1 : 0) | (!c.originDead.load() ? 2 : 0) |
         (c.peer.Get() ? 4 : 0);
}
extern "C" int flax_gc_stat(Probe* p, int stat) {
  return stat == 0 ? p->collections : stat == 1 ? p->traceCount : p->jitCount;
}
extern "C" void flax_gc_drop_js(Probe* p) {
  Probe::Scope scope(*p);
  scope.context->Global()
      ->Set(scope.context, p->text("business"), v8::Array::New(p->isolate))
      .Check();
}
extern "C" void flax_gc_js_pressure(Probe* p, int count) {
  Probe::Scope scope(*p);
  auto source =
      "globalThis.ordinary = Array.from({length:" + std::to_string(count) +
      "}, (_,i)=>({i, value:i*3}));";
  p->eval(source.c_str());
}
extern "C" void flax_gc_js_collect(Probe* p) {
  v8::Isolate::Scope entered(p->isolate);
  p->isolate->ClearKeptObjects();
  p->isolate->RequestGarbageCollectionForTesting(
      v8::Isolate::kFullGarbageCollection,
      cppgc::EmbedderStackState::kNoHeapPointers);
  p->cppHeap->FinishSweepingForTesting();
}
extern "C" void flax_gc_destroy(Probe* p) {
  delete p;
}
