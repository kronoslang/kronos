project(lithe CXX)
cmake_minimum_required(VERSION 3.1)

set(CMAKE_CXX_STANDARD 14)

include_directories(/)

if(CMAKE_COMPILER_IS_GNUCXX)
	add_definitions(-fPIC)	
endif()

add_library(lithe 
	lithe.cpp 
	lithe.h 
	ast.cpp 
	ast.h 
	adapter.h
	variant.h)

add_library(grammar_common
	grammar/common.cpp
	grammar/common.h)

add_library(grammar_json
	grammar/json.cpp
	grammar/json.h)

add_library(grammar_kronos 
	grammar/kronos.cpp
	grammar/kronos.h)

target_link_libraries(grammar_kronos lithe grammar_common)
target_link_libraries(grammar_json lithe grammar_common)

if(NOT LITHE_LIBRARY_TARGETS_ONLY)
	add_executable(testxml test/xml.cpp)
	add_executable(testjson test/json.cpp)
	add_executable(testkronos test/kronos.cpp)

	target_link_libraries(testxml lithe)
	target_link_libraries(testjson grammar_json)
	target_link_libraries(testkronos grammar_kronos)
endif()
