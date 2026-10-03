cmake_minimum_required(VERSION 3.16)
project(minilock VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type: default to Release when none is given (-DCMAKE_BUILD_TYPE=Debug).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
endif()

option(MINILOCK_SANITIZE "Build with AddressSanitizer and UBSan (Debug only)" OFF)

# Debug: no optimization, full debug info, many warnings, DEBUG defined.
set(CMAKE_C_FLAGS_DEBUG "-O0 -g3 -Wall -Wextra -DDEBUG")
# Release: optimized, no asserts, stack protector and fortify as hardening.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG -fstack-protector-strong -D_FORTIFY_SOURCE=2")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

set(FORMATS JPEG PNG WEBP GIF TIFF SVG AVIF HEIF)

pkg_check_modules(IMG_JPEG libjpeg)
pkg_check_modules(IMG_PNG  libpng)
pkg_check_modules(IMG_WEBP libwebp)
pkg_check_modules(IMG_WEBPDEMUX libwebpdemux) # animated WebP

pkg_check_modules(IMG_GIF giflib)
if(NOT IMG_GIF_FOUND)
  find_path(GIF_INCLUDE_DIR gif_lib.h)
  find_library(GIF_LIBRARY NAMES gif giflib)
  if(GIF_INCLUDE_DIR AND GIF_LIBRARY)
    set(IMG_GIF_FOUND TRUE)
    set(IMG_GIF_INCLUDE_DIRS ${GIF_INCLUDE_DIR})
    set(IMG_GIF_LIBRARIES ${GIF_LIBRARY})
  endif()
endif()

pkg_check_modules(IMG_TIFF libtiff-4)
pkg_check_modules(IMG_SVG  librsvg-2.0)
pkg_check_modules(IMG_AVIF libavif)
pkg_check_modules(IMG_HEIF libheif)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_library(PAM_LIB pam REQUIRED)
find_package(Threads REQUIRED)

pkg_get_variable(PROTO_DIR wayland-protocols pkgdatadir)
set(LOCK_XML ${PROTO_DIR}/staging/ext-session-lock/ext-session-lock-v1.xml)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR})

add_custom_command(
  OUTPUT ${GEN_DIR}/ext-session-lock-v1-client-protocol.h
  COMMAND ${WAYLAND_SCANNER} client-header ${LOCK_XML}
          ${GEN_DIR}/ext-session-lock-v1-client-protocol.h
  DEPENDS ${LOCK_XML}
)

add_custom_command(
  OUTPUT ${GEN_DIR}/ext-session-lock-v1-protocol.c
  COMMAND ${WAYLAND_SCANNER} private-code ${LOCK_XML}
          ${GEN_DIR}/ext-session-lock-v1-protocol.c
  DEPENDS ${LOCK_XML}
)

# Include include/
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_executable(${PROJECT_NAME}
  src/main.c
  src/minilock.c
  src/decoder.c
  src/blur.c
  src/cache.c
  src/tomlc17.c
  ${GEN_DIR}/ext-session-lock-v1-client-protocol.h
  ${GEN_DIR}/ext-session-lock-v1-protocol.c
)

foreach(fmt IN LISTS FORMATS)
  if(IMG_${fmt}_FOUND)
    target_compile_definitions(minilock PRIVATE HAVE_${fmt})
    target_include_directories(minilock PRIVATE ${IMG_${fmt}_INCLUDE_DIRS})
    target_link_libraries(minilock PRIVATE ${IMG_${fmt}_LIBRARIES})
    message(STATUS "Image format enabled: ${fmt}")
  endif()
endforeach()

if(IMG_WEBP_FOUND AND IMG_WEBPDEMUX_FOUND)
  target_compile_definitions(minilock PRIVATE HAVE_WEBP_ANIM)
  target_include_directories(minilock PRIVATE ${IMG_WEBPDEMUX_INCLUDE_DIRS})
  target_link_libraries(minilock PRIVATE ${IMG_WEBPDEMUX_LIBRARIES})
  message(STATUS "Animated WebP enabled")
endif()

if(MINILOCK_SANITIZE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${PROJECT_NAME} PRIVATE -fsanitize=address,undefined)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

target_compile_definitions(${PROJECT_NAME} PRIVATE PROJECT_NAME="${PROJECT_NAME}")
target_compile_definitions(${PROJECT_NAME} PRIVATE PROJECT_VERSION="${PROJECT_VERSION}")

target_include_directories(${PROJECT_NAME} PRIVATE
  ${GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE
  ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES} ${PAM_LIB} m
  Threads::Threads)
