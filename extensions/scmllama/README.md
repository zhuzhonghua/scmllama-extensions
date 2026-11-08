# extensions/scmllama — Godot 4 GDExtension for scmllama

Embeds scmllama (the dummyscheme VM plus the llama.cpp bindings) into Godot 4
and exposes a single class, `ScmLlama` (`RefCounted`). The sources only depend
on headers from the `scmllama/` submodule.

## Class

```gdscript
var svm = ScmLlama.new()
svm.add_scheme_path("res://scheme;res://scheme/?.scm")  # equivalent of (require-path! ...)
svm.load_file("/abs/path/game.scm")
var out = svm.call_fn("game:talk", ["smith", "village"])  # Variant
svm.eval_str("(+ 1 2)")
svm.get_last_error()
```

Value conversion:

| Variant → Scheme                | Scheme → Variant                     |
| ------------------------------- | ------------------------------------ |
| nil → '()                       | '() → null                           |
| int/float/bool/String → same    | int/float/bool/string → same         |
| Array → list                    | proper list → Array                  |
| Dictionary → alist (keys become symbols) | vector → Array; anything else → `write` text |

GC rule: every Scheme value that must survive later allocations is rooted with
`StkVar` (RAII), as described in scmllama's `context.md` §4.6.

## Threading

The VM may only be touched from the main thread (dummyscheme has no locks and
`llama_context` is not thread-safe). Generation is a synchronous blocking call:
the GDScript side renders a "thinking" state first, then `await` two frames
before calling. A worker thread with signal-based streaming output is future
work.

## Build

From the repository root (not from this directory):

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target scmllama_gdextension -j
```

Default artifact: `addons/scmllama/bin/libscmllama_gdextension.dylib`, which
together with `addons/scmllama/scmllama.gdextension` forms an addons folder that
can be copied straight into a game project. To write into a game project
directly, configure with:

```bash
-DSCMLLAMA_GDENGINE_OUTPUT_DIR=$HOME/clojureserver/theford/addons/scmllama/bin
```

## Game project side (The Ford)

```
theford/addons/scmllama/
├── scmllama.gdextension      # entry_symbol = scmllama_library_init
└── bin/libscmllama_gdextension.dylib
```

The host `scmllama` executable and its unit tests stay in the upstream repo;
this repo does not duplicate them.

## Versions

- Godot: 4.5+ (4.7.2 is installed locally; the extension remains backward
  compatible)
- godot-cpp: `godot-4.5-stable` tag (pinned by submodule commit)
- scmllama / llama.cpp: pinned by submodule commits (see `git submodule status`)
