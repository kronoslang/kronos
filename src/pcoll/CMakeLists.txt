project(pcoll CXX)
cmake_minimum_required(VERSION 3.4)

add_library(pcoll 
	hamt.cpp
	hamt.h
	util.h
	stm.h
	treap.h
	pcoll.natvis)

include_directories(.)

if(PCOLL_TEST)
	set_property(GLOBAL PROPERTY USE_FOLDERS ON)
	
	set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER cmake)

	enable_testing()

	add_library(test_core tests/test.cpp)
	set_target_properties(test_core PROPERTIES FOLDER tests)

	set(PCOLL_TESTS 
		hamt_concurrent_fuzz 
		hamt_dissoc
		hamt_integrity 
		treap_concurrent_fuzz
		treap_integrity
		stm_switcharoo)

	foreach(test ${PCOLL_TESTS})
		add_executable(${test} tests/${test}.cpp)
		target_link_libraries(${test} pcoll test_core)
		add_test(NAME ${test} COMMAND ${test})		
	endforeach()
	set_target_properties(${PCOLL_TESTS} PROPERTIES FOLDER tests)
endif()