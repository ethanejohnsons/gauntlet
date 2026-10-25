cmake_minimum_required(VERSION 3.20)
project(gauntlet_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Float behavior must match the engine build (oDFe on Linux: -O2, no fast-math, no FMA).
# Never add -march=native or -ffast-math here: FMA contraction changes results.
set(GS_FLOAT_FLAGS -ffp-contract=off -fno-fast-math -msse2 -mfpmath=sse)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# The vendored Q3 code is compiled as-is with warnings off; our own files get warnings.
set(Q3_ENGINE_SOURCES
  q3/qcommon/cm_load.c q3/qcommon/cm_patch.c q3/qcommon/cm_polylib.c
  q3/qcommon/cm_test.c q3/qcommon/cm_trace.c q3/qcommon/q_math.c q3/qcommon/q_shared.c)
set(Q3_GAME_SOURCES
  q3/game/bg_pmove.c q3/game/bg_slidemove.c q3/game/bg_misc.c q3/game/q_math_vm.c)
set_source_files_properties(${Q3_ENGINE_SOURCES} ${Q3_GAME_SOURCES} PROPERTIES COMPILE_OPTIONS -w)
set(GS_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare)

# Engine side: collision and shared math exactly as oDFe builds them, the shims that
# stand in for the rest of the engine, and our map and player code.
add_library(gauntlet SHARED
  ${Q3_ENGINE_SOURCES}
  src/q3_shim.c
  src/pk3.c
  src/world.c
  src/route.c
  src/env.c
  src/observe.c
)
# Game side: what runs inside DeFRaG's qagame.qvm, compiled with the QVM's float
# semantics (single-precision constants, float-returning math syscalls, id's
# game-side q_math). See q3/game/bg_vmcompat.h.
add_library(q3game OBJECT
  ${Q3_GAME_SOURCES}
  src/game.c
)
foreach(lib gauntlet q3game)
  target_include_directories(${lib} PRIVATE q3/qcommon q3/game src)
  target_include_directories(${lib} PUBLIC include)
  target_compile_definitions(${lib} PRIVATE GAUNTLET_SIM)
  target_compile_options(${lib} PRIVATE ${GS_FLOAT_FLAGS})
endforeach()
target_compile_options(q3game PRIVATE -fsingle-precision-constant
  -include ${CMAKE_CURRENT_SOURCE_DIR}/q3/game/bg_vmcompat.h)
set_source_files_properties(src/game.c src/q3_shim.c src/pk3.c src/world.c src/route.c src/env.c src/observe.c
  PROPERTIES COMPILE_OPTIONS "${GS_WARNINGS}")
set_target_properties(q3game PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_sources(gauntlet PRIVATE $<TARGET_OBJECTS:q3game>)
target_link_libraries(gauntlet PRIVATE ZLIB::ZLIB Threads::Threads m)

enable_testing()
foreach(t test_map test_strafe test_threads test_items test_route test_observe)
  add_executable(${t} tests/${t}.c)
  target_compile_options(${t} PRIVATE ${GS_FLOAT_FLAGS})
  target_link_libraries(${t} PRIVATE gauntlet m)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(bench tests/bench.c)
target_compile_options(bench PRIVATE ${GS_FLOAT_FLAGS})
target_link_libraries(bench PRIVATE gauntlet m)
