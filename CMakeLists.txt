cmake_minimum_required(VERSION 3.10)
set(TARGET_NAME embed_text)

find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

# rtools/MinGW headers don't declare GetAddrInfoExCancel (a Win8+ API that
# cpp-httplib's non-blocking DNS path calls). The CPPHTTPLIB_USE_NON_BLOCKING_
# GETADDRINFO define reaches us through httplib's config and resists removal, so
# neutralize the call directly in the vcpkg-installed header at configure time.
file(GLOB_RECURSE _httplib_hdrs
     "${CMAKE_BINARY_DIR}/vcpkg_installed/*/include/httplib.h")
foreach(_hdr ${_httplib_hdrs})
    file(READ "${_hdr}" _hl)
    string(REPLACE
        "::GetAddrInfoExCancel(&cancel_handle)"
        "(void)cancel_handle"
        _hl2 "${_hl}")
    if(NOT _hl STREQUAL _hl2)
        file(WRITE "${_hdr}" "${_hl2}")
        message(STATUS "[embed_text] Patched ${_hdr}: neutralized GetAddrInfoExCancel")
    endif()
endforeach()


set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
set(EXTENSION_SOURCES src/embed_text_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Linux/RHEL: getaddrinfo_a lives in libanl; on Ubuntu it's in libc (so this is a no-op there)
find_library(LIBANL anl)

# Same config for both the static lib and the loadable extension.
foreach(_t ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})
    target_compile_definitions(${_t} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(${_t}
        httplib::httplib nlohmann_json::nlohmann_json
        OpenSSL::SSL OpenSSL::Crypto)

    if(APPLE)
        # cpp-httplib's Apple DNS path (getaddrinfo_with_timeout) uses CFHost.
        target_link_libraries(${_t}
            "-framework CoreFoundation" "-framework CFNetwork" "-framework Security")
        # Let OpenSSL verify against the system keychain (vcpkg OpenSSL ships no CA bundle).
        target_compile_definitions(${_t} PRIVATE CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
    elseif(LIBANL)
        target_link_libraries(${_t} ${LIBANL})
    endif()
endforeach()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
