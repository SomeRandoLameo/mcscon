cmake_minimum_required(VERSION 3.10)
project(mcscon CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Core library: 2 translation units, no dependencies. Simply copy include/ + src/*.cpp into any build system instead if you like.
add_library(mcscon STATIC src/zlib.cpp src/client.cpp)
# Optional modules (link only what you need): world, entities, inventory, physics, chat, session
add_library(mcscon_modules STATIC src/world.cpp src/entities.cpp src/inventory.cpp src/physics.cpp src/chat.cpp src/session.cpp)
target_link_libraries(mcscon_modules PUBLIC mcscon)
target_compile_options(mcscon_modules PRIVATE -Wall -Wextra -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
target_include_directories(mcscon PUBLIC include)
target_compile_options(mcscon PRIVATE -Wall -Wextra -Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)

# Optional BSD-socket transport
add_library(mcscon_posix STATIC platform/posix_transport.cpp)
target_link_libraries(mcscon_posix PUBLIC mcscon)

option(MCSCON_TESTS "Build tests" ON)
if(MCSCON_TESTS)
  enable_testing()
  foreach(t core client socket packets sb world chat modules)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} mcscon_modules mcscon mcscon_posix)
    set_target_properties(test_${t} PROPERTIES CXX_STANDARD 14)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
endif()

add_executable(ping_bot examples/ping_bot.cpp)
target_link_libraries(ping_bot mcscon mcscon_posix)

add_executable(integ_server tests/integ_server.cpp)   # needs a running local server, see testserver/
target_link_libraries(integ_server mcscon mcscon_posix)

add_executable(integ_session tests/integ_session.cpp)   # run via tools/integ_session.py
target_link_libraries(integ_session mcscon_modules mcscon mcscon_posix)

add_executable(integ_coverage tests/integ_coverage.cpp)   # run via tools/integ_coverage.py
target_link_libraries(integ_coverage mcscon_modules mcscon mcscon_posix)

add_executable(integ_edge tests/integ_edge.cpp)
target_link_libraries(integ_edge mcscon_modules mcscon mcscon_posix)

add_executable(integ_tp tests/integ_tp.cpp)
target_link_libraries(integ_tp mcscon_modules mcscon mcscon_posix)

add_executable(integ_two tests/integ_two.cpp)
target_link_libraries(integ_two mcscon_modules mcscon mcscon_posix)
