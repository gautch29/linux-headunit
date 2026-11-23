# Author: gautch29
# Notice date: 2026-11-23
# Provided as is, without warranty. See NOTICE.md.

# Luau 0.739 converts arbitrary numeric table keys to int before checking bounds.
# The reference assets legitimately use 2^31 as a key. Guard that conversion so
# UBSan can remain enabled; this changes no valid integer-index behavior.
# Keep this small local patch explicit and fail on any unreviewed source change.
function(headunit_patch_luau relative original_hash before after)
  set(path "${luau_SOURCE_DIR}/${relative}")
  file(READ "${path}" source)
  string(SHA256 actual_hash "${source}")
  string(REPLACE "${after}" "${before}" original "${source}")
  string(SHA256 normalized_hash "${original}")
  if(NOT actual_hash STREQUAL original_hash AND NOT normalized_hash STREQUAL original_hash)
    message(FATAL_ERROR "Unreviewed Luau source at ${relative}; update the pinned patch explicitly")
  endif()
  string(REPLACE "${before}" "${after}" patched "${original}")
  if(NOT source STREQUAL patched)
    file(WRITE "${path}" "${patched}")
  endif()
endfunction()

headunit_patch_luau(VM/src/lnumutils.h c441a8e9a50dddb585a9262060e4e4c9e76462c14be1b84348afe1b4e88e2686
  "#define luai_num2int(i, d) ((i) = (int)(d))"
  "// Headunit: reject non-representable candidates before the integer-index fast path.\ninline int headunit_num2int(double d)\n{\n    return d >= -2147483648.0 && d <= 2147483647.0 ? int(d) : 0;\n}\n\n#define luai_num2int(i, d) ((i) = headunit_num2int(d))")
headunit_patch_luau(VM/src/lvmexecute.cpp c43d56da69f6b153c98de85e4cef33a65ca5efdd549055cfd125558e53bed7f5
  "int index = int(indexd);"
  "int index = headunit_num2int(indexd);")
