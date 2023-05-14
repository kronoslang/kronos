# clear packaging settings
function(setDefaultPackageGenerator GEN)
	set(generators
			SOURCE_ZIP 
			SOURCE_TBZ2
			SOURCE_TGZ
			SOURCE_TXZ
			SOURCE_TZ
			BINARY_NSIS 
			BINARY_WIX 
			BINARY_PRODUCTBUILD 
			BINARY_BUNDLE
			BINARY_DEB
			BINARY_IFW
			BINARY_RPM
			BINARY_STGZ
			BINARY_TBZ2
			BINARY_TGZ
			BINARY_TXZ
			BINARY_TZ)

	list(REMOVE_ITEM generators ${GEN})	

	set(CPACK_${GEN} ON CACHE BOOL "" FORCE)

	foreach(_gen ${generators}) 
		set("CPACK_${_gen}" OFF CACHE BOOL "" FORCE)
	endforeach()
endfunction(setDefaultPackageGenerator)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
include(cmake/MSVC.cmake)
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
include(cmake/Clang.cmake)
else()
include(cmake/Gcc.cmake)
endif()

include(CheckTypeSize)
include(CheckCXXSymbolExists)
include(CheckIncludeFiles)

if (APPLE)
	include(cmake/Apple.cmake)
elseif(UNIX)
	include(cmake/Unix.cmake)
endif()

set(CMAKE_EXTRA_INCLUDE_FILES ucontext.h)
check_type_size(int128_t HAS_INT128 LANGUAGE CXX)
check_cxx_symbol_exists(makecontext ucontext.h HAVE_UCONTEXT_T)
check_cxx_symbol_exists(readlink unistd.h HAVE_READLINK)
check_cxx_symbol_exists(getpid libproc.h HAVE_GETPID)
check_cxx_symbol_exists(proc_pidpath libproc.h HAVE_PROC_PIDPATH)
check_cxx_symbol_exists(_NSGetExecutablePath mach-o/dyld.h HAVE__NSGETEXECUTABLEPATH)
check_cxx_symbol_exists(strnlen string.h HAVE_STRNLEN)
check_include_file_cxx(alloca.h HAVE_ALLOCA_H)

if (WIN32)
	check_cxx_symbol_exists(htonll WinSock2.h HAVE_HTONLL)
	check_cxx_symbol_exists(ntohll WinSock2.h HAVE_NTOHLL)
	check_cxx_symbol_exists(CreateFiberEx Windows.h HAVE_CREATEFIBEREX)
else()
	check_cxx_symbol_exists(htonll netinet/in.h HAVE_HTONLL)
	check_cxx_symbol_exists(ntohll netinet/in.h HAVE_NTOHLL)
endif(WIN32)

find_library(LIB_READLINE readline PATHS /usr/lib/x86_64-linux-gnu)
if (NOT ${LIB_READLINE} STREQUAL "LIB_READLINE-NOTFOUND")
	set(HAVE_READLINE 1)
	set(EXTRA_LIBS ${EXTRA_LIBS} ${LIB_READLINE})
endif()

message(STATUS "Readline: ${LIB_READLINE}")
