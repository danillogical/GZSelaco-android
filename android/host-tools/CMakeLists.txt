# Standalone project that builds only the code-generation tools the main build shells
# out to (re2c, lemon, zipdir). Those must run on the BUILD machine, so an Android
# cross-compile cannot produce them; it imports them from here instead.
#
# The main build consumes the generated ImportExecutables.cmake via -DIMPORT_EXECUTABLES=,
# which is included from the root CMakeLists.txt when CMAKE_CROSSCOMPILING is set.
#
#   cmake -S android/host-tools -B build-host -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-host
#   # -> build-host/ImportExecutables.cmake
cmake_minimum_required( VERSION 3.16 )

project( gzselaco-host-tools C CXX )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

if( NOT CMAKE_BUILD_TYPE )
	set( CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE )
endif()

set( GZ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." )
get_filename_component( GZ_ROOT "${GZ_ROOT}" ABSOLUTE )

list( APPEND CMAKE_MODULE_PATH "${GZ_ROOT}/cmake" )

# The bundled libraries below call make_release_only(), which the root CMakeLists.txt
# normally defines. Reproduce it so they can be added standalone.
macro( make_release_only )
	set( CMAKE_C_FLAGS_MINSIZEREL ${CMAKE_C_FLAGS_RELEASE} )
	set( CMAKE_C_FLAGS_RELWITHDEBINFO ${CMAKE_C_FLAGS_RELEASE} )
	set( CMAKE_CXX_FLAGS_MINSIZEREL ${CMAKE_CXX_FLAGS_RELEASE} )
	set( CMAKE_CXX_FLAGS_RELWITHDEBINFO ${CMAKE_CXX_FLAGS_RELEASE} )
endmacro()

# zipdir links against the bundled compression libraries, so they come along.
set( BZIP2_INCLUDE_DIR "${GZ_ROOT}/libraries/bzip2" )
set( BZIP2_LIBRARIES bz2 )
set( LZMA_INCLUDE_DIR "${GZ_ROOT}/libraries/lzma/C" )

add_subdirectory( "${GZ_ROOT}/libraries/bzip2" bzip2 )
add_subdirectory( "${GZ_ROOT}/libraries/lzma"  lzma )
add_subdirectory( "${GZ_ROOT}/libraries/miniz" miniz )

set( CROSS_EXPORTS "" )
add_subdirectory( "${GZ_ROOT}/tools" tools )

if( NOT CROSS_EXPORTS )
	message( FATAL_ERROR "No host tools were exported - tools/ did not populate CROSS_EXPORTS" )
endif()
message( STATUS "Host tools to export: ${CROSS_EXPORTS}" )

export( TARGETS ${CROSS_EXPORTS} FILE "${CMAKE_BINARY_DIR}/ImportExecutables.cmake" )
