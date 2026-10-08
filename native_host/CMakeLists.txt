# Native host for the WebAssembly build: pd.wasm is translated to C with wasm2c and built together
# with the host code, so a native player runs the exact game the browsers run (and can join the
# same online matches). See README.md.
#
#   cmake -S native_host -B build-native -DWABT_DIR=/path/to/wabt [-DPD_WASM=build-web/pd.wasm]
#   cmake --build build-native -j
cmake_minimum_required(VERSION 3.16)
project(pdhost C)

set(PD_WASM "${CMAKE_CURRENT_SOURCE_DIR}/../build-web/pd.wasm" CACHE FILEPATH "the game's WebAssembly build (web/build.sh)")
set(WABT_DIR "" CACHE PATH "a built wabt checkout (wasm2c and its runtime)")
set(WASM2C "${WABT_DIR}/build/wasm2c" CACHE FILEPATH "the wasm2c tool")
set(PDHOST_PLATFORM "sdl" CACHE STRING "platform layer: sdl, or null (headless: no window, input or audio)")

if(NOT EXISTS "${WABT_DIR}/wasm2c/wasm-rt-impl.c")
  message(FATAL_ERROR "set WABT_DIR to a wabt checkout with wasm2c built (https://github.com/WebAssembly/wabt)")
endif()

# pd.wasm -> C (8 files so the compiler can work in parallel)
set(W2C_DIR "${CMAKE_CURRENT_BINARY_DIR}/w2c")
set(W2C_OUT "${W2C_DIR}/pd.h")
foreach(i RANGE 7)
  list(APPEND W2C_OUT "${W2C_DIR}/pd_${i}.c")
endforeach()
add_custom_command(
  OUTPUT ${W2C_OUT}
  COMMAND ${CMAKE_COMMAND} -E make_directory "${W2C_DIR}"
  COMMAND "${WASM2C}" "${PD_WASM}" -o "${W2C_DIR}/pd.c" --module-name pd --num-outputs 8
  DEPENDS "${PD_WASM}"
  COMMENT "wasm2c: translating pd.wasm"
)
set_source_files_properties(${W2C_OUT} PROPERTIES GENERATED TRUE)

add_executable(pdhost
  src/main.c
  src/fs.c
  src/gl.c
  src/net.c
  src/platform_${PDHOST_PLATFORM}.c
  ${W2C_OUT}
  "${WABT_DIR}/wasm2c/wasm-rt-impl.c"
  "${WABT_DIR}/wasm2c/wasm-rt-mem-impl.c"
)
target_include_directories(pdhost PRIVATE "${W2C_DIR}" "${WABT_DIR}/wasm2c" src)

# The game must compute exactly what the browsers compute: wasm arithmetic is IEEE-754 without
# fused multiply-add, so the C compiler must not contract or reassociate float math.
target_compile_options(pdhost PRIVATE -O2 -ffp-contract=off -fno-fast-math -fno-strict-aliasing
  $<$<C_COMPILER_ID:GNU>:-fexcess-precision=standard>)
set_source_files_properties(${W2C_OUT} PROPERTIES COMPILE_OPTIONS "-w")

if(PDHOST_PLATFORM STREQUAL "sdl")
  find_package(SDL2 REQUIRED)
  target_include_directories(pdhost PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(pdhost PRIVATE ${SDL2_LIBRARIES})
endif()
target_link_libraries(pdhost PRIVATE m)
