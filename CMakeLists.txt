cmake_minimum_required(VERSION 3.16)
project(aproxy C)

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_HTTP2 "Build the HTTP/2 frontend (needs nghttp2)" ON)
option(ENABLE_HTTP3 "Build the HTTP/3 frontend (needs ngtcp2 + nghttp3)" ON)
option(ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds" ON)
option(BUILD_TESTING "Build unit tests" ON)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wpointer-arith
                    -Wstrict-prototypes -Wmissing-prototypes
                    -Wno-unused-parameter -fno-strict-aliasing)
add_compile_definitions(_GNU_SOURCE _DARWIN_C_SOURCE)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies. pkg-config is optional: on Homebrew we also look at the
# usual keg paths so the build works out of the box on macOS.
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(_brew_hints /opt/homebrew/opt /usr/local/opt)

# OpenSSL (TLS termination + QUIC crypto). Prefer the Homebrew keg so the
# QUIC-capable OpenSSL is picked over the system one.
foreach(_h ${_brew_hints})
  foreach(_v openssl@4 openssl@3 openssl)
    if(EXISTS "${_h}/${_v}" AND NOT OPENSSL_ROOT_DIR)
      set(OPENSSL_ROOT_DIR "${_h}/${_v}")
    endif()
  endforeach()
endforeach()
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL: ${OPENSSL_VERSION} (${OPENSSL_SSL_LIBRARY})")

# Small helper: find a header + library pair, with Homebrew hints.
function(aproxy_find_lib VAR header lib keg)
  set(_hints)
  foreach(_h ${_brew_hints})
    list(APPEND _hints "${_h}/${keg}")
  endforeach()
  find_path(${VAR}_INCLUDE_DIR ${header} HINTS ${_hints} PATH_SUFFIXES include)
  find_library(${VAR}_LIBRARY ${lib} HINTS ${_hints} PATH_SUFFIXES lib)
  if(${VAR}_INCLUDE_DIR AND ${VAR}_LIBRARY)
    set(${VAR}_FOUND TRUE PARENT_SCOPE)
  else()
    set(${VAR}_FOUND FALSE PARENT_SCOPE)
  endif()
endfunction()

set(HAVE_HTTP2 0)
if(ENABLE_HTTP2)
  aproxy_find_lib(NGHTTP2 nghttp2/nghttp2.h nghttp2 libnghttp2)
  if(NGHTTP2_FOUND)
    set(HAVE_HTTP2 1)
    message(STATUS "HTTP/2: enabled (${NGHTTP2_LIBRARY})")
  else()
    message(WARNING "HTTP/2: nghttp2 not found, frontend disabled")
  endif()
endif()

set(HAVE_HTTP3 0)
if(ENABLE_HTTP3)
  aproxy_find_lib(NGTCP2 ngtcp2/ngtcp2.h ngtcp2 libngtcp2)
  aproxy_find_lib(NGTCP2_OSSL ngtcp2/ngtcp2_crypto_ossl.h ngtcp2_crypto_ossl libngtcp2)
  aproxy_find_lib(NGHTTP3 nghttp3/nghttp3.h nghttp3 libnghttp3)
  if(NGTCP2_FOUND AND NGTCP2_OSSL_FOUND AND NGHTTP3_FOUND)
    set(HAVE_HTTP3 1)
    message(STATUS "HTTP/3: enabled (${NGTCP2_LIBRARY}, ${NGHTTP3_LIBRARY})")
  else()
    message(WARNING "HTTP/3: ngtcp2/nghttp3 not found, frontend disabled")
  endif()
endif()

# ---------------------------------------------------------------------------
# Core library: everything except main(), so tests can link it.
# ---------------------------------------------------------------------------
# Sources are globbed so that every component added to src/ is picked up;
# the HTTP/2 and HTTP/3 frontends are only compiled when their libraries exist.
file(GLOB_RECURSE APROXY_SOURCES CONFIGURE_DEPENDS src/*.c)
list(FILTER APROXY_SOURCES EXCLUDE REGEX "src/main\\.c$")
if(NOT HAVE_HTTP2)
  list(FILTER APROXY_SOURCES EXCLUDE REGEX "h2_server\\.c$")
endif()
if(NOT HAVE_HTTP3)
  list(FILTER APROXY_SOURCES EXCLUDE REGEX "h3_server\\.c$")
endif()

add_library(aproxy_core STATIC ${APROXY_SOURCES})
target_include_directories(aproxy_core PUBLIC include src)
target_compile_definitions(aproxy_core PUBLIC
  HAVE_HTTP2=${HAVE_HTTP2} HAVE_HTTP3=${HAVE_HTTP3}
  APROXY_VERSION="0.1.0")
target_link_libraries(aproxy_core PUBLIC Threads::Threads OpenSSL::SSL OpenSSL::Crypto)

if(HAVE_HTTP2)
  target_include_directories(aproxy_core PUBLIC ${NGHTTP2_INCLUDE_DIR})
  target_link_libraries(aproxy_core PUBLIC ${NGHTTP2_LIBRARY})
endif()
if(HAVE_HTTP3)
  target_include_directories(aproxy_core PUBLIC ${NGTCP2_INCLUDE_DIR} ${NGHTTP3_INCLUDE_DIR})
  target_link_libraries(aproxy_core PUBLIC ${NGTCP2_OSSL_LIBRARY} ${NGTCP2_LIBRARY} ${NGHTTP3_LIBRARY})
endif()

add_executable(proxy src/main.c)
target_link_libraries(proxy PRIVATE aproxy_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  enable_testing()
  file(GLOB UNIT_TESTS CONFIGURE_DEPENDS tests/test_*.c)
  foreach(src ${UNIT_TESTS})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE aproxy_core)
    add_test(NAME ${name} COMMAND ${name})
  endforeach()
endif()
