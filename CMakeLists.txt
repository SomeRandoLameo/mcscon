# Use from C/C++ (e.g. vendored in vendor/mcscon):
#   add_subdirectory(vendor/mcscon)
#   target_link_libraries(my_app PRIVATE mcscon::mcscon)
# Needs cargo/rustc. Headers are in include/ (mcscon.h, mcscon.hpp).
cmake_minimum_required(VERSION 3.15)
project(mcscon LANGUAGES C CXX)

find_program(MCSCON_CARGO cargo REQUIRED)

set(MCSCON_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(MCSCON_LIB ${MCSCON_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}mcscon${CMAKE_STATIC_LIBRARY_SUFFIX})
file(GLOB_RECURSE MCSCON_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.rs)

add_custom_command(
  OUTPUT ${MCSCON_LIB}
  COMMAND ${MCSCON_CARGO} build --release --lib --features capi
          --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml --target-dir ${MCSCON_TARGET_DIR}
  DEPENDS ${MCSCON_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml
  COMMENT "cargo build mcscon (staticlib)"
  VERBATIM)
add_custom_target(mcscon_cargo DEPENDS ${MCSCON_LIB})

find_package(Threads REQUIRED)
add_library(mcscon STATIC IMPORTED GLOBAL)
set_target_properties(mcscon PROPERTIES
  IMPORTED_LOCATION ${MCSCON_LIB}
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include
  INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS};$<$<NOT:$<PLATFORM_ID:Windows>>:m>")
add_dependencies(mcscon mcscon_cargo)
add_library(mcscon::mcscon ALIAS mcscon)
