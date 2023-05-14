enable_testing()

set(BUILDNAME "${CMAKE_SYSTEM_NAME}${CMAKE_SYSTEM_VERSION}-${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_PROCESSOR}" 
	CACHE STRING "build name variable for CDash" FORCE)

set(ktest_driver $<TARGET_FILE:ktests>)
set(just_built_kc $<TARGET_FILE:kc>)
set(just_built_krepl $<TARGET_FILE:krepl>)

add_library(static_test_core STATIC 
	"src/driver/StaticTestCore.cpp"
	"src/driver/CompareTestResultJSON.cpp")

set_target_properties(static_test_core PROPERTIES FOLDER libs COMPILE_FLAGS "-D_CONSOLE")

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/kc_obj)

find_package(PythonInterp 2.7 REQUIRED)

execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/configure_tests.py" "${CMAKE_SOURCE_DIR}/library/tests.json"
				OUTPUT_VARIABLE STATIC_TESTS)

execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/partition_dynamic_tests.py" "${CMAKE_SOURCE_DIR}/library/tests.json"
				OUTPUT_VARIABLE TEST_MODULES)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS 
	"${CMAKE_SOURCE_DIR}/configure_tests.py"
	"${CMAKE_SOURCE_DIR}/partition_dynamic_tests.py"
	"${CMAKE_SOURCE_DIR}/library/tests.json" )

set(KRONOS_SUBMIT_TEST_AUTH "" CACHE STRING "Submit test run to database")

if (KRONOS_SUBMIT_TEST_AUTH) 
	
	foreach(TEST_MODULE ${TEST_MODULES})
		add_test(NAME "dynamic.${TEST_MODULE}" COMMAND ktests --submit -dba ${KRONOS_SUBMIT_TEST_AUTH} --submodule ${TEST_MODULE})
	endforeach()

	add_custom_target(
		submit_test_reference
		COMMAND ktests --submit -dba ${KRONOS_SUBMIT_TEST_AUTH} --bless
		DEPENDS ktests)
else()
	foreach(TEST_MODULE ${TEST_MODULES})
		add_test(NAME "dynamic.${TEST_MODULE}" COMMAND ktests --submodule ${TEST_MODULE})
	endforeach()
	add_custom_target(
		submit_test_reference
		COMMAND ktests --submit --bless
		DEPENDS ktests)
endif()

add_custom_target(
	run_dynamic_tests 
	COMMAND ktests
	DEPENDS ktests)

add_custom_target(
	run_demo
	COMMAND ktests -D
	DEPENDS ktests)

set_target_properties(submit_test_reference run_dynamic_tests run_demo PROPERTIES FOLDER dynamic_tests)

foreach(var ${STATIC_TESTS})
	string(REPLACE "\n" ";" FILE_CASE ${var})
	list(GET FILE_CASE 0 test_file)
	list(GET FILE_CASE 1 test_case)
	list(GET FILE_CASE 2 test_expr)

	set(src_file "${CMAKE_SOURCE_DIR}/library/tests/${test_file}.k")
	set(obj_file "${CMAKE_BINARY_DIR}/kc_obj/${test_file}_${test_case}.o")
	set(repl_cmd "krepl -i ${src_file} ${test_expr}")
	set(test_name "${test_file}.${test_case}")

	set_source_files_properties("${ref_file}" PROPERTIES GENERATED 1)

	add_custom_command(
		OUTPUT 	${obj_file} 
		COMMAND kc -q -m "${test_expr}" -o ${obj_file} ${src_file} 
		DEPENDS ${src_file} ${just_built_kc} $<TARGET_FILE:core>
		COMMENT "kc ${src_file} -> ${obj_file}"
		VERBATIM)

	add_executable(${test_name} ${obj_file} src/driver/StaticTestStub.cpp ${ref_file})

	target_link_libraries(${test_name} static_test_core)			

	set_target_properties(${test_name} PROPERTIES FOLDER "static_tests/${test_file}" COMPILE_FLAGS -D_CONSOLE)

	add_test( NAME "static.${test_name}" 
		COMMAND 
		${CMAKE_COMMAND}
		"-DREPL=$<TARGET_FILE:krepl>"
		"-DSRC=${src_file}"
		"-DEXPR=${test_expr}"
		"-DTEST=$<TARGET_FILE:${test_name}>"
		-P "${CMAKE_SOURCE_DIR}/cmake/StaticTestDriver.cmake")

endforeach()
