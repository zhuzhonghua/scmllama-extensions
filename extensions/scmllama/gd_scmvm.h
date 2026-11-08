#pragma once

// gd_scmvm.h — Godot GDExtension wrapper for an scmllama Runtime.
// Exposes one class, ScmLlama: a dummyscheme VM with all llm-* llama.cpp
// bindings pre-registered (scmllama::Runtime does the RAII).
//
// Threading contract: call everything from the main thread. The dummyscheme
// VM has no synchronization (context.md §4.7.7) and llama_context is not
// thread-safe either. Generation is synchronous for now; a worker thread +
// signals is the planned v2 shape.

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "scmllama.h"

namespace godot {

class ScmLlama : public RefCounted {
  GDCLASS(ScmLlama, RefCounted)

private:
  scmllama::Runtime* runtime_ = nullptr;
  String last_error_;

protected:
  static void _bind_methods();

public:
  ScmLlama();
  ~ScmLlama() override;

  // ---- lifecycle --------------------------------------------------------

  // True once the Runtime/VM exist.
  bool is_valid() const { return runtime_ != nullptr; }

  // Last error from load_file / call_fn / eval_str ("" when none).
  String get_last_error() const { return last_error_; }

  // ---- scripting --------------------------------------------------------

  // (require-path! "...") with a ';'-separated template list.
  String add_scheme_path(const String& path);
  // Load and run one .scm file (top-level defines go into genv).
  String load_file(const String& path);
  // Evaluate a snippet; returns the last value converted to a Variant.
  Variant eval_str(const String& code);
  // Call a no-arg helper defined in Scheme.
  Variant call_fn0(const String& name);
  // Call fn(name args...) with 0..n args; result converted to Variant.
  Variant call_fn(const String& name, const Array& args);
  // Direct global set (GC-safe: values are rooted during conversion).
  void set_global(const String& name, const Variant& value);

private:
  Variant scheme_to_variant(Scheme::VM* vm, Scheme::ValueT* v);
  // Converts `value` into the rooted slot `out`; roots are created
  // RAII-style (Scheme::StkVar) inside the call tree.
  bool variant_to_scheme(Scheme::VM* vm, const Variant& value,
                         Scheme::ValueT* out);
};

} // namespace godot
