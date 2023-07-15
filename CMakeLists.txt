cmake_minimum_required(VERSION 3.1)
project(lipid)

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "/usr/local/lib/pkgconfig")
pkg_check_modules(ASYNC REQUIRED async)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "stage/native/build/components/lipid/bin")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_BUILD_TYPE Debug)
add_executable(lipid src/lipid.cpp src/socketaddress.cpp)
target_link_libraries(lipid ${ASYNC_STATIC_LIBRARIES})
target_include_directories(lipid PUBLIC ${ASYNC_INCLUDE_DIRS})
target_compile_options(lipid PUBLIC ${ASYNC_CFLAGS_OTHER})
