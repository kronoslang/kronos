message(STATUS "Configuring for Apple")

add_definitions(
# -D_XOPEN_SOURCE=600
		-Wno-deprecated-declarations
		-Wno-inconsistent-missing-override)

set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -D_XOPEN_SOURCE=600)
set(CMAKE_REQUIRED_LIBRARIES pthread)

find_library(COREMIDI_FRAMEWORK CoreMidi)
set(PLATFORM_IO_LIBS ${COREMIDI_FRAMEWORK})

setDefaultPackageGenerator(BINARY_PRODUCTBUILD)
