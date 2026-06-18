# This file is included by DuckDB's build system. It specifies which extension to load

# --- Windows / VS2026 fmt workaround -----------------------------------------
# VS2026 (MSVC 14.51+) removed stdext::checked_array_iterator, which DuckDB's
# bundled fmt still references under `#ifdef _SECURE_SCL`. The new STL still
# *defines* _SECURE_SCL, so the broken branch compiles and fails (C2653), and
# because it's `#ifdef` no /D flag can force the portable #else branch.
# Rewrite the guard to `#if 0` at configure time (this file is processed before
# add_third_party(fmt)) so fmt uses its plain-pointer branch. Idempotent;
# a no-op on non-MSVC and once DuckDB ships the upstream fix.
set(_fmt_header "${CMAKE_CURRENT_LIST_DIR}/duckdb/third_party/fmt/include/fmt/format.h")
if(EXISTS "${_fmt_header}")
    file(READ "${_fmt_header}" _fmt_src)
    string(REPLACE "#ifdef _SECURE_SCL" "#if 0 // patched for VS2026" _fmt_new "${_fmt_src}")
    if(NOT _fmt_src STREQUAL _fmt_new)
        file(WRITE "${_fmt_header}" "${_fmt_new}")
        message(STATUS "[embed_text] Patched bundled fmt for VS2026 (_SECURE_SCL -> #if 0)")
    endif()
endif()

# Extension from this repo
duckdb_extension_load(embed_text
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
