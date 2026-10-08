# Cross-compiling native_host/ for PS5 native folder titles with the
# ps5-native-app-boilerplate compiler wrapper (see ps5/build.sh, which sets these variables).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(PS5_TEMPLATE "$ENV{PS5_TEMPLATE}" CACHE PATH "ps5-native-app-boilerplate checkout")
set(CMAKE_C_COMPILER "${PS5_TEMPLATE}/tooling/prospero-clang18")
set(CMAKE_CXX_COMPILER "${PS5_TEMPLATE}/tooling/prospero-clang18")
find_program(CMAKE_AR NAMES llvm-ar-18 llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib-18 llvm-ranlib REQUIRED)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-D__PROSPERO__ -fPIC -ffunction-sections -fdata-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# no threads library to find: pthreads are part of the console's libc
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(Threads_FOUND TRUE)
