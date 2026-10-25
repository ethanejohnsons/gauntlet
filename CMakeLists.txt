# Top-level build, so IDEs (CLion) can open the repo root.
# The sim is the only CMake project; oDFe keeps its own Makefile and is wrapped
# in the `odfe` target below.

cmake_minimum_required(VERSION 3.20)
project(gauntlet C)

enable_testing()
add_subdirectory(sim)

# Build the engine out-of-tree (not part of the default build): cmake --build <dir> --target odfe
add_custom_target(odfe
  COMMAND make -C ${CMAKE_CURRENT_SOURCE_DIR}/engine/oDFe -j release
          BUILD_DIR=${CMAKE_CURRENT_SOURCE_DIR}/build/odfe
  USES_TERMINAL
  COMMENT "Building oDFe into build/odfe"
)
