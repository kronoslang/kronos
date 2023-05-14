set(CMAKE_FIND_ROOT_PATH "")
set(CMAKE_POSITION_INDEPENDENT_CODE False)
set(BINARYEN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../binaryen/src" CACHE PATH "Binaryen include files")

set(EXT_BASE "${CMAKE_BINARY_DIR}/external")
set_property(DIRECTORY PROPERTY EP_BASE ${EXT_BASE})
set(BINARYEN_BUILD_DIR "${EXT_BASE}/Build/binaryen")

include(ExternalProject)

set(BINARYEN_VERSION "version_84")

if (EMSCRIPTEN) 
	set(CMAKE_CMD "emcmake")
	set(CMAKE_ARGS "cmake" "-DENABLE_WERROR=OFF" "-DBUILD_STATIC_LIB=ON")
else()
	set(CMAKE_CMD ${CMAKE_COMMAND})
	set(CMAKE_ARGS "-DENABLE_WERROR=OFF" "-DBUILD_STATIC_LIB=ON")
endif()

if (WIN32)
	ExternalProject_Add(
		binaryen 
		SVN_REPOSITORY "https://github.com/WebAssembly/binaryen.git/tags/${BINARYEN_VERSION}"
		CMAKE_COMMAND "${CMAKE_CMD}"
		CMAKE_ARGS ${CMAKE_ARGS}
		BUILD_COMMAND cmake --build . --target binaryen
		INSTALL_COMMAND "")
else()
	ExternalProject_Add(
		binaryen 
		GIT_REPOSITORY https://github.com/WebAssembly/binaryen.git
		GIT_TAG ${BINARYEN_VERSION}
		CMAKE_COMMAND "${CMAKE_CMD}"
		CMAKE_ARGS ${CMAKE_ARGS}
		BUILD_COMMAND cmake --build . --target binaryen -- -j4
		INSTALL_COMMAND "")
endif()

if (EMSCRIPTEN)
	set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O2 -s STACK_OVERFLOW_CHECK=2 -s WARN_UNALIGNED=1 -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0 -s DISABLE_EXCEPTION_THROWING=0 -s ASSERTIONS=2")
	set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -s DISABLE_EXCEPTION_CATCHING=1 -s USE_CLOSURE_COMPILER=1")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wcast-align -Wover-aligned -DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0 -s ALLOW_MEMORY_GROWTH=1 -s WASM=1 -s MODULARIZE=1 --bind")
endif()

foreach(dep binaryen passes wasm asmjs emscripten-optimizer ir cfg support)
	set(PATH "${BINARYEN_BUILD_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}${dep}${CMAKE_STATIC_LIBRARY_SUFFIX}")
	list(APPEND BINARYEN_LIBRARIES "${PATH}")
endforeach()

message(STATUS "Linking Binaryen ${BINARYEN_VERSION}")
add_library(binaryen_backend
	"src/backends/BinaryenInterface.cpp"
	"src/backends/BinaryenEmitter.cpp"
	"src/backends/BinaryenEmitter.h"
	"src/backends/BinaryenModule.cpp"
	"src/backends/BinaryenCompiler.cpp"
	"src/backends/BinaryenModule.h"
	"src/backends/BinaryenCompiler.h"
	"src/backends/GenericModule.h"
	"src/backends/CodeGenModule.h")
	
target_link_libraries(binaryen_backend PUBLIC ${BINARYEN_LIBRARIES})
set_target_properties( binaryen_backend PROPERTIES FOLDER libs/emitters )
target_include_directories( binaryen_backend PRIVATE "${EXT_BASE}/Source/binaryen/src" )
add_dependencies(binaryen_backend binaryen)

set(HAVE_BINARYEN True)

if(EMSCRIPTEN)
	file(COPY ${CMAKE_SOURCE_DIR}/library DESTINATION ${CMAKE_BINARY_DIR} FILES_MATCHING PATTERN *.k)
	file(COPY ${CMAKE_SOURCE_DIR}/library DESTINATION ${CMAKE_BINARY_DIR} FILES_MATCHING PATTERN *.json)

	# veneer runtime
	add_executable(veneer
		src/lithe/lithe.cpp
		src/lithe/ast.cpp
		src/driver/veneer.cpp
		src/lithe/grammar/kronos.cpp
		src/lithe/grammar/common.cpp)


	# compiler
    add_executable(kronos src/driver/kwasm.cpp ${KRONOS_CORE_SOURCES})
	target_link_libraries(kronos PRIVATE binaryen_backend tinyxml grammar_kronos grammar_json platform)
	set_property(TARGET veneer APPEND_STRING PROPERTY COMPILE_FLAGS "-s NO_FILESYSTEM=1 -fno-exceptions")

	# this is CMake at its best.
	target_link_libraries(kronos PUBLIC "--preload-file ${CMAKE_BINARY_DIR}/library@library" "-s TOTAL_STACK=64MB -s TOTAL_MEMORY=128MB -s 'EXPORT_NAME=\"NativeCompiler\"'")
	target_link_libraries(veneer PUBLIC "-fno-exceptions -s ALLOW_MEMORY_GROWTH=1 -s TOTAL_STACK=16MB -s TOTAL_MEMORY=32MB" "-s NO_FILESYSTEM=1" "-s EXPORTED_FUNCTIONS='[\"_vnr_malloc\",\"_vnr_free\",\"_vnr_memset\"]'" "-s 'EXPORT_NAME=\"NativeParser\"'")

	if ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
		target_link_libraries(kronos PUBLIC "-g3")
	endif()
endif()
