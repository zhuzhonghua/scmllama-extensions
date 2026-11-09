cmake_minimum_required(VERSION 3.14...3.28)

project(scmllama-extensions LANGUAGES C CXX)

# Same language level as llama.cpp / godot-cpp. GNU extensions stay on because
# ggml's CPU/Metal backends use them (llama itself builds as gnu++17).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# llama.cpp — from the scmllama submodule, libraries only
# ---------------------------------------------------------------------------
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)

add_subdirectory(scmllama/llama.cpp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# scmllama core: bindings + dummyscheme, compiled straight from the submodule.
# The submodule only pins source code; the target and artifacts live here, so
# another extension can link the same core without touching scmllama's build.
# ---------------------------------------------------------------------------
file(GLOB SCMLLAMA_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama/scmllama*.cpp)
file(GLOB DUMMYSCHEME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama/dummyscheme/vm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama/dummyscheme/scm*.cpp)

add_library(scmllama_core STATIC
    ${SCMLLAMA_CORE_SOURCES}
    ${DUMMYSCHEME_SOURCES})

target_include_directories(scmllama_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama/dummyscheme)

target_compile_features(scmllama_core PUBLIC cxx_std_17)

target_link_libraries(scmllama_core PUBLIC
    llama
    Threads::Threads)

# ---------------------------------------------------------------------------
# godot-cpp (submodule, pinned to a 4.5-stable tag)
# ---------------------------------------------------------------------------
# scmllama reports errors with C++ exceptions (throw const char*), so the
# extension must keep exception handling even though Godot itself doesn't.
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
add_subdirectory(godot-cpp EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Extension: scmllama (class ScmLlama)
# ---------------------------------------------------------------------------
add_library(scmllama_gdextension SHARED
    extensions/scmllama/gd_scmvm.cpp
    extensions/scmllama/register_types.cpp)

target_include_directories(scmllama_gdextension PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama
    ${CMAKE_CURRENT_SOURCE_DIR}/scmllama/dummyscheme)

target_compile_features(scmllama_gdextension PRIVATE cxx_std_17)

target_link_libraries(scmllama_gdextension PRIVATE
    scmllama_core
    godot-cpp)

# Default: self-contained addons/ folder, ready to copy into a game project.
# Point it at a game directly for a tighter loop, e.g.
#   -DSCMLLAMA_GDENGINE_OUTPUT_DIR=$HOME/clojureserver/theford/addons/scmllama/bin
set(SCMLLAMA_GDENGINE_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/addons/scmllama/bin"
    CACHE PATH "Output dir for libscmllama_gdextension")

set_target_properties(scmllama_gdextension PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${SCMLLAMA_GDENGINE_OUTPUT_DIR}
    PREFIX "lib"
    # Do not bake this machine's build/bin into the addon. The extension and
    # the bundled llama/ggml dylibs resolve each other via @loader_path.
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "@loader_path")

# ---------------------------------------------------------------------------
# macOS: bundle the llama.cpp/ggml runtime dylibs next to the extension.
# The addons/ folder must be copyable to another machine; an absolute rpath to
# this build tree would break the moment the project is moved or shipped.
# ---------------------------------------------------------------------------
if(APPLE)
  set(_scmllama_runtime_targets
      llama
      ggml
      ggml-base
      ggml-cpu
      ggml-blas
      ggml-metal)
  set(_scmllama_runtime_files "")
  foreach(_target IN LISTS _scmllama_runtime_targets)
    if(TARGET ${_target})
      list(APPEND _scmllama_runtime_files "$<TARGET_FILE:${_target}>")
    endif()
  endforeach()

  add_custom_command(TARGET scmllama_gdextension POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DOUT_DIR=${SCMLLAMA_GDENGINE_OUTPUT_DIR}
        -DEXTENSION=$<TARGET_FILE:scmllama_gdextension>
        -DRUNTIME_DIR=$<TARGET_FILE_DIR:llama>
        "-DRUNTIME_FILES=${_scmllama_runtime_files}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle_addon_runtime.cmake
    COMMENT "Bundling llama.cpp runtime libraries into the addon"
    VERBATIM)
endif()
