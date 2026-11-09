# bundle_addon_runtime.cmake — make the macOS addons/ folder relocatable.
#
# Inputs (passed with -D from CMakeLists.txt):
#   OUT_DIR       directory that holds libscmllama_gdextension.dylib
#   EXTENSION     full path to libscmllama_gdextension.dylib
#   RUNTIME_DIR   directory that holds llama.cpp's built dylibs (build/bin)
#   RUNTIME_FILES semicolon-separated list of the llama/ggml dylib files
#
# What it does:
#   1. copy the current llama.cpp/ggml dylibs next to the extension,
#   2. create the usual versioned-soname symlinks (libllama.0.dylib -> ...),
#   3. replace the absolute build-tree LC_RPATH with @loader_path,
#   4. re-sign ad-hoc after install_name_tool invalidates the linker signature.
#
# This keeps the artifact layout unchanged (one addons/scmllama folder) while
# removing any dependency on this machine's build directory.

if(NOT APPLE)
  return()
endif()

if(NOT EXISTS "${EXTENSION}")
  message(FATAL_ERROR "bundle_addon_runtime: extension not found: ${EXTENSION}")
endif()

if(NOT EXISTS "${RUNTIME_DIR}")
  message(FATAL_ERROR "bundle_addon_runtime: runtime dir not found: ${RUNTIME_DIR}")
endif()

file(MAKE_DIRECTORY "${OUT_DIR}")

# Drop stale copies from an older llama.cpp version before copying the current
# set; otherwise the addon slowly accumulates incompatible dylibs.
file(GLOB _stale
  "${OUT_DIR}/libllama*.dylib"
  "${OUT_DIR}/libggml*.dylib")
if(_stale)
  file(REMOVE ${_stale})
endif()

foreach(_lib IN LISTS RUNTIME_FILES)
  if(NOT EXISTS "${_lib}")
    message(FATAL_ERROR "bundle_addon_runtime: missing runtime library: ${_lib}")
  endif()

  get_filename_component(_real_name "${_lib}" NAME)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lib}" "${OUT_DIR}/${_real_name}"
    RESULT_VARIABLE _copy_rc)
  if(NOT _copy_rc EQUAL 0)
    message(FATAL_ERROR "bundle_addon_runtime: failed to copy ${_lib}")
  endif()

  # install name is the soname, e.g. @rpath/libllama.0.dylib
  execute_process(
    COMMAND /usr/bin/otool -D "${_lib}"
    OUTPUT_VARIABLE _otool_d
    RESULT_VARIABLE _otool_rc)
  if(_otool_rc EQUAL 0)
    string(REGEX MATCH "@rpath/[^\n\r]+" _install_name "${_otool_d}")
    if(_install_name)
      get_filename_component(_soname "${_install_name}" NAME)
      if(NOT _soname STREQUAL _real_name)
        file(REMOVE "${OUT_DIR}/${_soname}")
        execute_process(
          COMMAND ${CMAKE_COMMAND} -E create_symlink "${_real_name}" "${OUT_DIR}/${_soname}")
      endif()
    endif()
  endif()
endforeach()

function(scmllama_patch_rpath _file)
  if(IS_SYMLINK "${_file}")
    return()
  endif()

  # The dylibs produced by llama.cpp carry the absolute build-tree rpath.
  # Remove it so a copy can never accidentally resolve back into build/bin.
  execute_process(
    COMMAND /usr/bin/install_name_tool -delete_rpath "${RUNTIME_DIR}" "${_file}"
    RESULT_VARIABLE _rc
    ERROR_QUIET)
  execute_process(
    COMMAND /usr/bin/install_name_tool -add_rpath "@loader_path" "${_file}"
    RESULT_VARIABLE _rc
    ERROR_QUIET)

  # install_name_tool invalidates the linker signature; ad-hoc signing is
  # enough for a locally built GDExtension and keeps dyld happy on arm64.
  execute_process(
    COMMAND /usr/bin/codesign --force --sign - "${_file}"
    RESULT_VARIABLE _rc
    ERROR_QUIET)
endfunction()

scmllama_patch_rpath("${EXTENSION}")

file(GLOB _bundled
  "${OUT_DIR}/libllama*.dylib"
  "${OUT_DIR}/libggml*.dylib")
foreach(_lib IN LISTS _bundled)
  scmllama_patch_rpath("${_lib}")
endforeach()
