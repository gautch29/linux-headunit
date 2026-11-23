# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Explicit, pinned optional dependency. Build outside the source tree with
# cmake/skia-pi.gnargs. Never download or silently select a system Skia at configure.
set(HEADUNIT_THEME_SKIA_SOURCE "" CACHE PATH "Pinned Skia checkout")
set(HEADUNIT_THEME_SKIA_BUILD "" CACHE PATH "Pinned Skia GN output directory")
set(_skia_revision "a8363ad146938c9df9ec0c975c81849a6494d175")
file(READ "${HEADUNIT_THEME_SKIA_BUILD}/source-receipt.json" _skia_receipt)
string(JSON _skia_actual GET "${_skia_receipt}" revision)
string(JSON _skia_args GET "${_skia_receipt}" argsSha256)
file(SHA256 "${HEADUNIT_THEME_SKIA_BUILD}/args.gn" _skia_args_actual)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/cmake/skia-pi.gnargs" _skia_expected_args)
string(JSON _skia_semantic ERROR_VARIABLE _skia_semantic_missing GET "${_skia_receipt}" semanticArgsSha256)
if(_skia_semantic_missing)
  set(_skia_semantic "${_skia_args}")
endif()
if(NOT _skia_actual STREQUAL _skia_revision OR NOT _skia_args STREQUAL _skia_args_actual OR NOT _skia_semantic STREQUAL _skia_expected_args)
  message(FATAL_ERROR "Skia revision/build flags do not match the pinned configuration")
endif()
if(NOT EXISTS "${HEADUNIT_THEME_SKIA_SOURCE}/include/effects/SkGradient.h" OR NOT EXISTS "${HEADUNIT_THEME_SKIA_BUILD}/libskia.a")
  message(FATAL_ERROR "Build pinned Skia before enabling the scene adapter")
endif()
add_library(headunit-skia STATIC IMPORTED)
set_target_properties(headunit-skia PROPERTIES IMPORTED_LOCATION "${HEADUNIT_THEME_SKIA_BUILD}/libskia.a"
  INTERFACE_INCLUDE_DIRECTORIES "${HEADUNIT_THEME_SKIA_SOURCE}"
  INTERFACE_COMPILE_DEFINITIONS "SK_RELEASE;SK_GANESH;SK_GL;SK_R32_SHIFT=16;SK_ASSUME_GL_ES=1;SK_DISABLE_TRACING;SK_GAMMA_APPLY_TO_A8")
