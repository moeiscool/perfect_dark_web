# Native part of the Android app: SDL2 (shared, loaded by SDLActivity) and libmain.so, the native
# host (native_host/) running pd.wasm translated to C, with touch controls and OpenGL ES.
cmake_minimum_required(VERSION 3.19)
project(perfectdark_android C)

set(SDL_SOURCE "" CACHE PATH "SDL2 release source (android/build.sh downloads it)")
set(PD_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
if(NOT EXISTS "${SDL_SOURCE}/include/SDL.h")
  message(FATAL_ERROR "set SDL_SOURCE to an SDL2 source tree (use android/build.sh)")
endif()

# SDL2 as libSDL2.so
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory("${SDL_SOURCE}" SDL2)

# the native host, as a static library
set(PDHOST_LIBRARY ON CACHE BOOL "" FORCE)
set(PDHOST_GLES ON CACHE BOOL "" FORCE)
set(PDHOST_TOUCH ON CACHE BOOL "" FORCE)
# explicit bounds checks: no guard pages, so no SIGSEGV handler next to ART's
set(PDHOST_BOUNDS_CHECK ON CACHE BOOL "" FORCE)
set(PDHOST_PLATFORM_NAME "android" CACHE STRING "" FORCE)
set(PDHOST_SDL2_INCLUDE_DIR "${SDL_SOURCE}/include" CACHE PATH "" FORCE)
add_subdirectory("${PD_REPO}/native_host" native_host)

add_library(main SHARED android_main.c)
target_include_directories(main PRIVATE "${SDL_SOURCE}/include")
target_link_libraries(main PRIVATE pdhost mbedtls SDL2 GLESv3 EGL android log z m)
