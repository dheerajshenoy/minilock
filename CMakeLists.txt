cmake_minimum_required(VERSION 3.16)
project(minilock C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
find_library(PAM_LIB pam REQUIRED)

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

add_executable(${PROJECT_NAME}
  main.c
  ${GEN_DIR}/ext-session-lock-v1-client-protocol.h
  ${GEN_DIR}/ext-session-lock-v1-protocol.c
)

target_compile_definitions(${PROJECT_NAME} PRIVATE PROJECT_NAME="${PROJECT_NAME}")

target_include_directories(${PROJECT_NAME} PRIVATE
  ${GEN_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS} ${XKBCOMMON_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE
  ${WAYLAND_CLIENT_LIBRARIES} ${XKBCOMMON_LIBRARIES} ${PAM_LIB})
