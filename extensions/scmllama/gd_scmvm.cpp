// gd_scmvm.cpp — ScmLlama implementation (see gd_scmvm.h).
//
// GC rules respected (context.md section 4.6):
//   * every Scheme value that must survive later allocations is held in a
//     Scheme::StkVar (RAII root chained into the VM root list);
//   * strings handed to the VM are copies (scm_make_string copies);
//   * the VM is only touched from the caller's thread (main, per
//     GDExtension conventions). No locking: dummyscheme has none.
//
// Error convention: sets last_error_ and returns a NIL Variant; the GDScript
// bridge surfaces it. Scheme-side "ScmError" strings from scm_error surface
// through pcall-style fact (scm_apply stores the message in *out).

#include "gd_scmvm.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "vm.h"
#include "scmapi.h"

#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace godot {

using namespace Scheme;  // ValueT / VM / macros (Scar, Scdr, setnull, isnull)

// ---- small byte bridging helpers -------------------------------------------

static std::string gd_bytes(const String& s) {
  CharString cs = s.utf8();
  return std::string(cs.get_data(), (size_t)cs.length());
}

static godot::String gd_str(const char* p, int len) {
  return String::utf8(p, len);
}

// RAII root list: holds StkVar roots for one embedded call.
struct GdRoots {
  VM* vm;
  std::vector<StkVar*> roots;

  explicit GdRoots(VM* v) : vm(v) {}
  ~GdRoots() {
    for (std::vector<StkVar*>::iterator it = roots.begin(); it != roots.end(); ++it) {
      delete *it;
    }
  }

  ValueT* push() {
    StkVar* r = new StkVar(vm);
    roots.push_back(r);
    return &(r->val);
  }
};

static const int kGdMaxArgs = 32;

// ---- streaming queue ---------------------------------------------------------
//
// The Scheme generation callback (running on whatever thread owns the VM)
// pushes token pieces here via the registered native fn `llm-stream-push`;
// Godot drains them with ScmLlama::stream_poll() from any thread (the game's
// main thread in practice). Owner-free on purpose: the VM may be created and
// destroyed on a worker thread while the UI polls.
static std::mutex g_stream_mutex;
static std::deque<std::string> g_stream;

static ValueT scmllama_stub_llm_stream_push(VM* vm, ValueT* a) {
  if (scm_is_string(vm, a)) {
    int len = 0;
    const char* s = scm_get_string(vm, a, &len);
    std::lock_guard<std::mutex> lock(g_stream_mutex);
    g_stream.push_back(std::string(s, (size_t)len));
  }
  return scm_make_bool(vm, true);
}

void ScmLlama::stream_register(VM* vm) {
  static const RegCProc stream_lib[] = {
    RegCProc("llm-stream-push", scmllama_stub_llm_stream_push),
    RegCProc(NULL, -1),
  };
  scm_register_lib(vm, stream_lib);
}

PackedStringArray ScmLlama::stream_poll() {
  PackedStringArray out;
  std::lock_guard<std::mutex> lock(g_stream_mutex);
  while (!g_stream.empty()) {
    out.push_back(String::utf8(g_stream.front().c_str()));
    g_stream.pop_front();
  }
  return out;
}

// ---- lifecycle --------------------------------------------------------------

ScmLlama::ScmLlama() {
  // Runtime does llama_backend_init + scm_create_vm + binding registration.
  runtime_ = new scmllama::Runtime();
  if (runtime_ == nullptr || runtime_->vm() == nullptr) {
    UtilityFunctions::push_error("[scmllama] failed to create Scheme VM / llama runtime");
    delete runtime_;
    runtime_ = nullptr;
    return;
  }
  // Token streaming: register `llm-stream-push` so Scheme callbacks can hand
  // pieces to the UI while a generation is still in flight.
  stream_register(runtime_->vm());
}

ScmLlama::~ScmLlama() {
  delete runtime_;
}

// ---- scripting (public methods) ----------------------------------------------

String ScmLlama::add_scheme_path(const String& path) {
  last_error_ = String();
  if (!is_valid()) {
    return String("VM not initialized");
  }
  VM* vm = runtime_->vm();
  std::string b = gd_bytes(path);
  std::string code = std::string("(require-path! \"") + b + "\")";
  try {
    vm->evalstr(code.c_str());
  } catch (const char* err) {
    last_error_ = String(err);
  }
  return last_error_;
}

String ScmLlama::load_file(const String& path) {
  last_error_ = String();
  if (!is_valid()) {
    return String("VM not initialized");
  }
  std::string b = gd_bytes(path);
  try {
    runtime_->vm()->loadfile(b.c_str());
  } catch (const char* err) {
    last_error_ = String(err);
  }
  return last_error_;
}

Variant ScmLlama::eval_str(const String& code) {
  last_error_ = String();
  if (!is_valid()) {
    last_error_ = String("VM not initialized");
    return Variant();
  }
  VM* vm = runtime_->vm();
  std::string b = gd_bytes(code);
  ValueT out;
  setnull(&out);
  try {
    out = vm->evalstr(b.c_str());
  } catch (const char* err) {
    last_error_ = String(err);
    return Variant();
  }
  return scheme_to_variant(vm, &out);
}

Variant ScmLlama::call_fn0(const String& name) {
  return call_fn(name, Array());
}

Variant ScmLlama::call_fn(const String& name, const Array& args) {
  last_error_ = String();
  if (!is_valid()) {
    last_error_ = String("VM not initialized");
    return Variant();
  }
  VM* vm = runtime_->vm();
  std::string nb = gd_bytes(name);

  GdRoots scope(vm);

  ValueT fn;
  setnull(&fn);
  try {
    fn = scm_get_global(vm, nb.c_str());
  } catch (const char* err) {
    last_error_ = String(err);
    return Variant();
  }
  if (isnull(&fn)) {
    last_error_ = String("undefined Scheme function: ") + name;
    return Variant();
  }

  int argc = args.size();
  if (argc > kGdMaxArgs) {
    last_error_ = String("too many arguments (max ") + String::num_int64(kGdMaxArgs) + ")";
    return Variant();
  }

  ValueT* argvp[kGdMaxArgs];
  for (int i = 0; i < argc; i++) {
    ValueT* slot = scope.push();
    if (!variant_to_scheme(vm, args[i], slot)) {
      return Variant();  // last_error_ set by conversion
    }
    argvp[i] = slot;
  }

  // Root the callee before the likelihood of GC during argument boxing is gone.
  ValueT* fn_root = scope.push();
  *fn_root = fn;

  ValueT out;
  setnull(&out);
  try {
    bool ok = scm_apply(vm, fn_root, argc > 0 ? argvp : NULL, argc, &out);
    if (!ok) {
      if (scm_is_string(vm, &out)) {
        int len = 0;
        const char* s = scm_get_string(vm, &out, &len);
        std::string msg(s, (size_t)len);
        last_error_ = String::utf8(msg.c_str(), (int64_t)msg.size());
      } else {
        last_error_ = String("Scheme call failed: ") + name;
      }
      return Variant();
    }
  } catch (const char* err) {
    last_error_ = String(err);
    return Variant();
  }

  // Conversion may allocate; out itself is stack-local, so root a copy first.
  ValueT* out_root = scope.push();
  *out_root = out;
  return scheme_to_variant(vm, out_root);
}

void ScmLlama::set_global(const String& name, const Variant& value) {
  last_error_ = String();
  if (!is_valid()) {
    last_error_ = String("VM not initialized");
    return;
  }
  VM* vm = runtime_->vm();
  std::string nb = gd_bytes(name);
  GdRoots scope(vm);
  ValueT* slot = scope.push();
  if (!variant_to_scheme(vm, value, slot)) {
    return;
  }
  try {
    scm_set_global(vm, nb.c_str(), *slot);
  } catch (const char* err) {
    last_error_ = String(err);
  }
}

// ---- scheme -> Variant --------------------------------------------------------

Variant ScmLlama::scheme_to_variant(VM* vm, ValueT* v) {
  if (scm_is_int(vm, v)) {
    return Variant((int64_t)scm_get_int(vm, v));
  }
  if (scm_is_float(vm, v)) {
    return Variant((double)scm_get_float(vm, v));
  }
  if (scm_is_bool(vm, v)) {
    return Variant(scm_get_bool(vm, v));
  }
  if (scm_is_string(vm, v)) {
    int len = 0;
    const char* s = scm_get_string(vm, v, &len);
    std::string copy(s, (size_t)len);
    return Variant(String::utf8(copy.c_str(), (int64_t)copy.size()));
  }
  // Dotted pair (a . b) -> [a, b]. The topic menus use (key . label) pairs;
  // without this they would collapse to a one-element array or a string.
  if (scm_is_pair(vm, v)) {
    ValueT cdr = *Scdr(v);
    if (!scm_is_pair(vm, &cdr) && !scm_is_null(vm, &cdr)) {
      ValueT car = *Scar(v);
      Array arr;
      arr.push_back(scheme_to_variant(vm, &car));
      arr.push_back(scheme_to_variant(vm, &cdr));
      return Variant(arr);
    }
  }
  if (scm_is_list(vm, v)) {
    Array arr;
    ValueT it = *v;
    for (int guard = 0; guard < 65536 && scm_is_pair(vm, &it); guard++) {
      ValueT car = *Scar(&it);
      arr.push_back(scheme_to_variant(vm, &car));
      it = *Scdr(&it);
    }
    return Variant(arr);
  }
  if (scm_is_vector(vm, v)) {
    int n = scm_vector_len(vm, v);
    Array arr;
    for (int i = 0; i < n; i++) {
      ValueT item = scm_vector_ref(vm, v, i);
      arr.push_back(scheme_to_variant(vm, &item));
    }
    return Variant(arr);
  }
  if (scm_is_null(vm, v)) {
    return Variant();
  }
  // Fallback: the write-syntax form (symbols, tables, closures, ...).
  int len = 0;
  const char* s = scm_tostring(vm, v, &len);
  std::string copy(s, (size_t)len);
  return Variant(String::utf8(copy.c_str(), (int64_t)copy.size()));
}

// ---- Variant -> scheme (GC careful: roots created via GdRoots pattern) --------

// Local RAII root, mirrors RootScope but usable inside the recursive builder.
struct GdLocalRoot {
  StkVar r;
  explicit GdLocalRoot(VM* v) : r(v) {}
  ValueT* slot() { return &(r.val); }
  ~GdLocalRoot() {}  // ~StkVar unlinks from the VM root chain
};

bool ScmLlama::variant_to_scheme(VM* vm, const Variant& value, ValueT* out) {
  switch (value.get_type()) {
    case Variant::NIL: {
      setnull(out);
      return true;
    }
    case Variant::BOOL: {
      *out = scm_make_bool(vm, (bool)value);
      return true;
    }
    case Variant::INT: {
      *out = scm_make_int(vm, (scm_int)(int64_t)(value));
      return true;
    }
    case Variant::FLOAT: {
      *out = scm_make_float(vm, (scm_float)(double)(value));
      return true;
    }
    case Variant::STRING: {
      std::string b = gd_bytes((const String&)value);
      *out = scm_make_string(vm, b.c_str(), (int)b.size());
      return true;
    }
    case Variant::ARRAY: {
      Array arr = value;
      int n = arr.size();
      GdLocalRoot acc(vm);
      setnull(acc.slot());
      // Back to front: each element must be rooted while consing.
      std::vector<StkVar*> elems;
      bool ok = true;
      for (int i = 0; i < n && ok; i++) {
        StkVar* slot = new StkVar(vm);
        elems.push_back(slot);
        ok = variant_to_scheme(vm, arr[i], &(slot->val));
      }
      if (!ok) {
        for (size_t k = 0; k < elems.size(); k++) delete elems[k];
        return false;
      }
      for (int i = n - 1; i >= 0; i--) {
        *acc.slot() = scm_make_pair(vm, &elems[i]->val, acc.slot());
      }
      for (size_t k = 0; k < elems.size(); k++) delete elems[k];
      *out = *acc.slot();
      return ok;
    }
    case Variant::DICTIONARY: {
      // alist: ((key . value) ...), keys become symbols when strings.
      Dictionary dict = value;
      Array keys = dict.keys();
      int n = keys.size();
      GdLocalRoot acc(vm);
      setnull(acc.slot());
      std::vector<StkVar*> cells;
      bool ok = true;
      for (int i = n - 1; i >= 0 && ok; i--) {
        Variant k = keys[i];
        Variant v2 = dict.get(k, Variant());
        StkVar* kc = new StkVar(vm);
        cells.push_back(kc);
        ok = variant_to_scheme(vm, k, &(kc->val));
        if (ok) {
          // String keys become symbols so Scheme can (alist-get 'key alist).
          StkVar* keyv = new StkVar(vm);
          cells.push_back(keyv);
          if (scm_is_string(vm, &(kc->val))) {
            ValueT* ks = &(kc->val);
            int len = 0;
            const char* s = scm_get_string(vm, ks, &len);
            keyv->val = scm_make_symbol(vm, s, len);
          } else {
            keyv->val = kc->val;
          }
          StkVar* vc = new StkVar(vm);
          cells.push_back(vc);
          ok = variant_to_scheme(vm, v2, &(vc->val));
          if (ok) {
            StkVar* cell = new StkVar(vm);
            cells.push_back(cell);
            cell->val = scm_make_pair(vm, &keyv->val, &vc->val);
            *acc.slot() = scm_make_pair(vm, &cell->val, acc.slot());
          }
        }
      }
      for (size_t k = 0; k < cells.size(); k++) delete cells[k];
      if (ok) *out = *acc.slot();
      return ok;
    }
    default: {
      last_error_ = String("unsupported Variant type for Scheme: ") +
                     Variant::get_type_name((Variant::Type)value.get_type());
      return false;
    }
  }
}

// / (int64_t) value casts of Variant use operator int64_t(); fine.

// ---- registration --------------------------------------------------------------

void ScmLlama::_bind_methods() {
  ClassDB::bind_method(D_METHOD("is_valid"), &ScmLlama::is_valid);
  ClassDB::bind_method(D_METHOD("get_last_error"), &ScmLlama::get_last_error);
  ClassDB::bind_method(D_METHOD("add_scheme_path", "path"), &ScmLlama::add_scheme_path);
  ClassDB::bind_method(D_METHOD("load_file", "path"), &ScmLlama::load_file);
  ClassDB::bind_method(D_METHOD("eval_str", "code"), &ScmLlama::eval_str);
  ClassDB::bind_method(D_METHOD("call_fn0", "name"), &ScmLlama::call_fn0);
  ClassDB::bind_method(D_METHOD("call_fn", "name", "args"), &ScmLlama::call_fn);
  ClassDB::bind_method(D_METHOD("set_global", "name", "value"), &ScmLlama::set_global);
  ClassDB::bind_static_method("ScmLlama", D_METHOD("stream_poll"),
                              &ScmLlama::stream_poll);
}

} // namespace godot
