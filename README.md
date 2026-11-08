# scmllama-extensions

Build and distribution home for [scmllama](https://github.com/zhuzhonghua/llama.cpp-scheme)
as a Godot 4 GDExtension — and, later, a home for other extensions built on the
same core.

## Purpose

Run scmllama inside Godot without a server, a subprocess, or a language bridge:
the extension loads a Scheme VM and llama.cpp into the engine and exposes them
as one GDScript class. This repo exists so that scmllama itself stays focused on
its own job (the C++ bindings and the host executable), while packaging,
version pinning, and platform artifacts live here.

## What it provides

- **`ScmLlama` class** (`RefCounted`): a dummyscheme VM with all `llm-*`
  llama.cpp bindings pre-registered. Load `.scm` files, evaluate snippets, and
  call Scheme functions with GDScript values.
- **Local inference from Scheme**: model loading, context, batch, sampler, and
  generation calls happen in-process on the extension's own thread contract
  (main thread only — neither the VM nor `llama_context` is thread-safe).
- **Safe value conversion**: `Variant` ↔ Scheme for nil, bool, number, string,
  Array (list), and Dictionary (alist), with `StkVar` roots so GC never sees a
  dangling value.
- **Reproducible builds**: scmllama, its `llama.cpp` and `dummyscheme`
  submodules, and `godot-cpp` are all pinned by commit. The extension keeps C++
  exception handling enabled because scmllama reports errors via exceptions.
- **Drop-in output**: the build emits `libscmllama_gdextension` plus a
  `.gdextension` manifest (entry symbol `scmllama_library_init`), forming an
  `addons/scmllama/` folder that any Godot project can copy in.
- **Room to grow**: the core library and each extension are separate CMake
  targets, so future extensions can share scmllama without touching this repo's
  root build logic.

## Compatibility

| Component | Version |
| --- | --- |
| Godot | 4.5+ (tested with 4.7.2) |
| godot-cpp | `godot-4.5-stable` |
| scmllama / llama.cpp / dummyscheme | pinned by submodule commit |

## Build

```bash
git clone --recursive https://github.com/zhuzhonghua/scmllama-extensions.git
cd scmllama-extensions
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target scmllama_gdextension -j
```

The result lands in `addons/scmllama/bin/`. To write straight into a game
project instead:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DSCMLLAMA_GDENGINE_OUTPUT_DIR=$HOME/clojureserver/theford/addons/scmllama/bin
```

## Using it in a game

Set up the bridge from GDScript:

```gdscript
var vm := ScmLlama.new()
vm.add_scheme_path("res://scheme;res://scheme/?.scm")
vm.load_file("res://scheme/game/main.scm")
var reply: Variant = vm.call_fn("game:talk", ["smith", "village"])
```

## Maintaining

Bumping scmllama:

```bash
git -C scmllama fetch origin
git -C scmllama checkout <commit>
git add scmllama && git commit -m "bump scmllama"
```
