// register_types.cpp — GDExtension entry point for the scmllama extension.
// The entry symbol matches scmllama.gdextension (entry_symbol).

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include "gd_scmvm.h"

using namespace godot;

static void scmllama_initialize_module(ModuleInitializationLevel p_level) {
  if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
    return;
  }
  ClassDB::register_class<ScmLlama>();
}

// No deinitializer needed: RefCounted objects clean themselves up, and the
// ScmLlama destructor releases its own Runtime.

extern "C" GDExtensionBool GDE_EXPORT scmllama_library_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address,
    const GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization) {
  GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library,
                                          r_initialization);
  init_obj.register_initializer(scmllama_initialize_module);
  init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
  return init_obj.init();
}
