set(REF_FILE "${TEST}.txt")

message(STATUS "Static ${TEST} ${SRC} (${EXPR})")


execute_process(
	COMMAND "${REPL}"
	-i "${SRC}"
	"Actions:Output-JSON(String:Concat() ${EXPR})"
	OUTPUT_FILE "${REF_FILE}")

execute_process(
	COMMAND ${TEST} 
	INPUT_FILE "${REF_FILE}" 
	OUTPUT_VARIABLE STATIC_OUT 
	RESULT_VARIABLE EXIT_CODE)

if (EXIT_CODE)
	message(FATAL_ERROR "${EXIT_CODE}:${STATIC_OUT}")
else()
	FILE(REMOVE "${REF_FILE}")
endif()