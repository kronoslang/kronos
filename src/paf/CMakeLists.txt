project(paf CXX)
cmake_minimum_required(VERSION 3.2)			

set(CPACK_PACKAGE_VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED 11)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(
			-D_CRT_SECURE_NO_DEPREATE 
			-D_CRT_NONSTDC_NO_DEPRECATE 
			-D_CRT_SECURE_NO_WARNINGS
			-D_SCL_SECURE_NO_WARNINGS
			-DNOMINMAX
			-D_UNICODE -DUNICODE
			 "/wd4458" "/wd4457" "/wd4456")
else ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
	add_definitions(-msse2 -fno-rtti -Wno-reorder -fPIC -Wno-unused-function)
endif()

if(APPLE)
	set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Build architecture")
endif()

if(CMAKE_GENERATOR STREQUAL Xcode)
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++11")
    set(CMAKE_XCODE_ATTRIBUTE_CLANG_CXX_LIBRARY "libc++")	
endif()

if (WIN32)
	add_library(paf STATIC mmf.cpp mmfspackle.cpp)
elseif (APPLE)
	add_library(paf STATIC audiotoolbox.cpp)
	find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    target_link_libraries( paf ${COREAUDIO_FRAMEWORK} ${AUDIOTOOLBOX_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
else (APPLE)
	include(FindLibSndFile.cmake)
	if (LIBSNDFILE_FOUND)
		add_library(paf STATIC sndfile.cpp)
		target_link_libraries( paf ${LIBSNDFILE_LIBRARY})
	else(${FFMPEG_FOUND})
		message(FATAL_ERROR "Couldn't find libsndfile")
	endif()
endif()

set_target_properties( paf 
		       PROPERTIES 
		       PUBLIC_HEADER PAF.h )

set(PAF_TARGET_LIBRARY_ONLY ON CACHE BOOL "Do not build test driver, export or install targets")

if(NOT PAF_LIBRARY_TARGET_ONLY)

add_executable(paf_test test.cpp)
target_link_libraries( paf_test paf )

target_include_directories(paf INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	$<INSTALL_INTERFACE:include>
	PUBLIC 
	${CMAKE_CURRENT_SOURCE_DIR)

install( TARGETS paf EXPORT PAFTargets
	LIBRARY DESTINATION lib 
	ARCHIVE DESTINATION lib 
	PUBLIC_HEADER DESTINATION include 
    FRAMEWORK DESTINATION "/Library/Frameworks" )

include(CMakePackageConfigHelpers)
write_basic_package_Version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/paf/PAFConfigVersion.cmake"
	VERSION "${CPACK_PACKAGE_VERSION}"
	COMPATIBILITY AnyNewerVersion
)

export(EXPORT PAFTargets
	FILE "${CMAKE_CURRENT_BINARY_DIR}/paf/PAFTargets.cmake"
	NAMESPACE PAF::)

configure_file(PAFConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/paf/PAFConfig.cmake"
	COPYONLY)

set(ConfigPackageLocation lib/cmake/paf)
install(EXPORT PAFTargets
		FILE PAFTargets.cmake
		NAMESPACE PAF::
		DESTINATION ${ConfigPackageLocation})

install(FILES
	PAFConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/paf/PAFConfigVersion.cmake"
	DESTINATION ${ConfigPackageLocation})

export(EXPORT PAFTargets FILE paf-config.cmake  NAMESPACE PAF::)
export(PACKAGE paf)
endif()
