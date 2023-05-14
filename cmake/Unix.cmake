# detect debian-ish platform
find_program(DPKG dpkg)
if (NOT ${DPKG} STREQUAL "DPKG-NOTFOUND")
	setDefaultPackageGenerator(BINARY_DEB)
endif()