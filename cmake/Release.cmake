# read current version and make release targets
file(READ ${CMAKE_SOURCE_DIR}/version.txt VERSION_FILE)
string(REPLACE "." ";" VERSION_NUMBERS ${VERSION_FILE})

function(write_release_files ID MAJOR MINOR PATCH)
	file(WRITE ${CMAKE_BINARY_DIR}/${ID}.txt "${MAJOR}.${MINOR}.${PATCH}")
	add_custom_target("${ID}"
		${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${ID}.txt ${CMAKE_SOURCE_DIR}/version.txt
		COMMAND
		${CMAKE_COMMAND} ${CMAKE_BINARY_DIR}
		COMMAND
		hg commit -R ${CMAKE_SOURCE_DIR} -m "Release ${MAJOR}.${MINOR}.${PATCH}"
		COMMAND
		hg tag -R ${CMAKE_SOURCE_DIR} "release-${MAJOR}.${MINOR}.${PATCH}"
		COMMAND
		hg push -R ${CMAKE_SOURCE_DIR}
		DEPENDS 
			${CMAKE_SOURCE_DIR}/version.txt
		SOURCES 
			${CMAKE_SOURCE_DIR}/version.txt
			${CMAKE_BINARY_DIR}/${ID}.txt
		)
	set_target_properties("${ID}" PROPERTIES FOLDER release)
endfunction(write_release_files)

list(GET VERSION_NUMBERS 0 CUR_MAJOR)
list(GET VERSION_NUMBERS 1 CUR_MINOR)
list(GET VERSION_NUMBERS 2 CUR_PATCH)

math(EXPR NEXT_MAJOR "${CUR_MAJOR} + 1")
math(EXPR NEXT_MINOR "${CUR_MINOR} + 1")
math(EXPR NEXT_PATCH "${CUR_PATCH} + 1")

set(CPACK_PACKAGE_VERSION_MAJOR ${CUR_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${CUR_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${CUR_PATCH})

write_release_files("release_${CUR_MAJOR}.${CUR_MINOR}.${NEXT_PATCH}" ${CUR_MAJOR} ${CUR_MINOR} ${NEXT_PATCH})
write_release_files("release_${CUR_MAJOR}.${NEXT_MINOR}.0" ${CUR_MAJOR} ${NEXT_MINOR} 0)
write_release_files("release_${NEXT_MAJOR}.0.0" ${NEXT_MAJOR} 0 0)

file(WRITE ${CMAKE_BINARY_DIR}/recent_changes.txt "please run the recent_changes target to populate this file.")

add_custom_target(recent_changes 
		COMMAND 
		hg log -R ${CMAKE_SOURCE_DIR} --template "{node|short} | {author|user}: {join(splitlines(desc), ' ')}\\n" -r "tip:release-${VERSION_FILE}" > "${CMAKE_BINARY_DIR}/recent_changes.txt"
		SOURCES 
		${CMAKE_BINARY_DIR}/recent_changes.txt
		${CMAKE_SOURCE_DIR}/changelog.md
		DEPENDS 
		${CMAKE_SOURCE_DIR}/version.txt)

set_target_properties( recent_changes PROPERTIES FOLDER release)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/version.txt)
