# Top-level build, so IDEs (CLion) can open the repo root.
# The sim is the only CMake project; oDFe keeps its own Makefile and is wrapped
# in the `odfe` target below.

cmake_minimum_required(VERSION 3.20)
project(gauntlet C)

enable_testing()
add_subdirectory(sim)

# Build the engine with engine/patches applied (not part of the default build):
#   cmake --build <dir> --target odfe
add_custom_target(odfe
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/engine/build.sh
  USES_TERMINAL
  COMMENT "Building patched oDFe into build/odfe"
)
