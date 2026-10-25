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

# Engine side: collision code and shared helpers exactly as oDFe builds them,
# plus the shims that stand in for the rest of the engine.
add_library(q3 STATIC
  q3/qcommon/cm_load.c
  q3/qcommon/cm_patch.c
  q3/qcommon/cm_polylib.c
  q3/qcommon/cm_test.c
  q3/qcommon/cm_trace.c
  q3/qcommon/q_math.c
  q3/qcommon/q_shared.c
  src/q3_shim.c
)
target_include_directories(q3 PUBLIC q3/qcommon q3/game src include)
target_compile_definitions(q3 PUBLIC GAUNTLET_SIM)
target_compile_options(q3 PRIVATE ${GS_FLOAT_FLAGS} -w)
set_target_properties(q3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Game side: what runs inside DeFRaG's qagame.qvm, compiled with the QVM's float
# semantics (single-precision constants, float-returning math syscalls, id's
# game-side q_math). See q3/game/bg_vmcompat.h.
add_library(q3game STATIC
  q3/game/bg_pmove.c
  q3/game/bg_slidemove.c
  q3/game/bg_misc.c
  q3/game/q_math_vm.c
  src/game.c
)
target_link_libraries(q3game PUBLIC q3)
target_compile_options(q3game PRIVATE ${GS_FLOAT_FLAGS} -fsingle-precision-constant
  -include ${CMAKE_CURRENT_SOURCE_DIR}/q3/game/bg_vmcompat.h -w)
set_target_properties(q3game PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(gauntlet SHARED
  src/pk3.c
  src/env.c
  src/entities.c
  src/world.c
  src/pool.c
)
target_include_directories(gauntlet PUBLIC include PRIVATE src)
target_compile_options(gauntlet PRIVATE ${GS_FLOAT_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_link_libraries(gauntlet PRIVATE q3game q3 ZLIB::ZLIB Threads::Threads m)

enable_testing()
foreach(t test_map test_strafe test_threads)
  add_executable(${t} tests/${t}.c)
  target_compile_options(${t} PRIVATE ${GS_FLOAT_FLAGS})
  target_link_libraries(${t} PRIVATE gauntlet m)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(bench tests/bench.c)
target_compile_options(bench PRIVATE ${GS_FLOAT_FLAGS})
target_link_libraries(bench PRIVATE gauntlet m)
