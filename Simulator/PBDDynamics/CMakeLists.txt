cmake_minimum_required(VERSION 3.5 FATAL_ERROR)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE $ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake CACHE STRING "")
	message(STATUS  "VCPKG is enable: CMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}") 
endif()

project(pbd-dynamics LANGUAGES CXX CUDA)

## Use C++11
set (CMAKE_CXX_STANDARD 11)
set (CMAKE_CUDA_STANDARD 11)   

set(BUILD_PBD ON)
include(../cmake/GAIA-config.cmake)

include_directories(
	${GAIA_INCLUDE_DIRS}
	
)

file(GLOB SRC
    "*.h"
    "*.cpp"
	"*.c"
	)

add_executable(pbd-dynamics 
	${SRC}
	${GAIA_SRCS}
)


target_compile_options(pbd-dynamics PUBLIC $<$<COMPILE_LANGUAGE:CUDA>:
                       --extended-lambda
					   --default-stream per-thread
                       >)

target_link_libraries(pbd-dynamics ${GAIA_LIBRARY})