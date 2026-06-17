cmake_minimum_required(VERSION 3.10)
set(TARGET_NAME embed_text)

find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

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
