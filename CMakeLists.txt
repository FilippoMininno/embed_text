cmake_minimum_required(VERSION 3.10)

# Set extension name here
set(TARGET_NAME embed_text)

find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.5
  )
  set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(httplib)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/embed_text_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} httplib::httplib nlohmann_json::nlohmann_json)
target_link_libraries(${LOADABLE_EXTENSION_NAME} httplib::httplib nlohmann_json::nlohmann_json)

if(APPLE)
    # macOS: use built-in SecureTransport, no OpenSSL needed
    target_compile_definitions(${EXTENSION_NAME} PRIVATE CPPHTTPLIB_USE_OPENSSL=0)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE CPPHTTPLIB_USE_OPENSSL=0)
else()
    # Linux / Windows: link system OpenSSL
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(${EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
