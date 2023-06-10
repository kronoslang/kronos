project(kronos CXX)
cmake_minimum_required(VERSION 3.1)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Release.cmake)
include(cmake/Platform.cmake)

set(CPACK_PACKAGE_VERSION ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH})

set(KRONOS_WEBSITE "" CACHE PATH "If set, build website from this source directory")

set(PAF_LIBRARY_TARGET_ONLY ON)
set(PAD_LIBRARY_TARGET_ONLY ON)
set(LITHE_LIBRARY_TARGETS_ONLY ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FindCURL)
if(CURL_FOUND)
	include_directories($CURL_INCLUDE_DIRS)
endif()

set(KRONOS_LLVM_BACKEND ON CACHE BOOL "Use LLVM backend")

if (NOT EMSCRIPTEN AND KRONOS_LLVM_BACKEND)
	add_subdirectory(src/paf)
	add_subdirectory(src/pad)
	set_target_properties( pad paf PROPERTIES FOLDER libs) 
	include(cmake/LLVM.cmake)
endif()

include_directories(src src/k3 "${CMAKE_CURRENT_BINARY_DIR}/headers")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set( KRONOS_CORE_SOURCES
	"src/backends/CallGraphAnalysis.cpp"
	"src/backends/CodeMotionPass.cpp"
	"src/backends/CopyElision.cpp"
	"src/backends/DriverSignature.cpp"
	"src/backends/CodeGenModule.cpp"
	"src/backends/CodeGenCompiler.cpp"
	"src/backends/SideEffectCompiler.cpp"
	"src/backends/TargetLowering.cpp"
#	"src/backends/WaveCoreCompiler.cpp"
#	"src/backends/WaveCore.cpp"
	"src/common/DynamicScope.cpp"
	"src/common/Graphviz.cpp"
	"src/common/Ref.cpp"
	"src/common/ssstring.cpp"
	"src/common/CallStack.cpp"
    "src/driver/CmdLineOpts.cpp"
	"src/k3/MemoizeTransform.cpp"
	"src/k3/Conversions.cpp"
	"src/k3/DynamicVariables.cpp"
	"src/k3/Evaluate.cpp"
	"src/k3/FlowControl.cpp"
	"src/k3/FunctionSequence.cpp"
	"src/k3/Generic.cpp"
	"src/k3/ImmutableNode.cpp"
	"src/k3/Invariant.cpp"
	"src/k3/kronos.cpp"
	"src/k3/LibraryRef.cpp"
	"src/k3/ModuleBuilder.cpp"
	"src/k3/Native.cpp"
	"src/k3/NativeVector.cpp"
	"src/k3/Parser.cpp"
	"src/k3/CodeRepository.cpp"
	"src/k3/Reactive.cpp"
	"src/k3/RegionNode.cpp"
	"src/k3/Stateful.cpp"
	"src/k3/TLS.cpp" 
	"src/k3/Type.cpp"
	"src/k3/TypeAlgebra.cpp"
	"src/k3/Typed.cpp"
	"src/k3/TypeRuleGenerator.cpp"
	"src/backends/CallGraphAnalysis.h"
	"src/backends/CodeMotionPass.h"
	"src/backends/DriverSignature.h"
	"src/backends/CodeGenModule.h"
	"src/backends/SideEffectCompiler.h"
	"src/backends/TargetLowering.h"
#	"src/backends/WaveCoreModule.h"
	"src/common/DynamicScope.h"
	"src/common/Enumerable.h"
	"src/common/Graphviz.h"
	"src/common/PlatformUtils.h"
	"src/common/PreprocessorMeta.h"
	"src/common/Ref.h"
	"src/common/Reflect.h"
	"src/common/ssstring.h"
	"src/driver/picojson.h"
	"src/k3/CompilerNodes.h"
	"src/k3/Conversions.h"
	"src/k3/DynamicVariables.h"
	"src/k3/EnumerableGraph.h"
	"src/k3/Errors.h"
	"src/k3/Evaluate.h"
	"src/k3/FlowControl.h"
	"src/k3/Generic.h"
	"src/k3/Graph.h"
	"src/k3/ImmutableNode.h"
	"src/k3/Invariant.h"
	"src/k3/LibraryRef.h"
	"src/k3/ModuleBuilder.h"
	"src/k3/Native.h"
	"src/k3/NativeVector.h"
	"src/k3/NodeBases.h"
	"src/k3/Parser.h"
	"src/k3/Reactive.h"
	"src/k3/ReactivityGraph.h"
	"src/k3/RegionNode.h"
	"src/k3/SmallContainer.h"
	"src/k3/Stateful.h"
	"src/k3/TLS.h"
	"src/k3/Transform.h"
	"src/k3/TupleTypeEnumerator.h"
	"src/k3/Type.h"
	"src/k3/TypeAlgebra.h"
	"src/k3/Typed.h"
	"src/k3/TypeRuleGenerator.h"
	"src/k3/UserErrors.h"
	"src/kronos.h"
	"src/kronos_abi.h"
)

set(KRONOS_BINARYEN_BACKEND OFF CACHE BOOL "Attempt to use Binaryen")

if (KRONOS_BINARYEN_BACKEND OR EMSCRIPTEN)
	message(STATUS "Using Binaryen backend")
	include(cmake/Binaryen.cmake)
endif()

add_subdirectory(src/lithe)
set_target_properties( lithe PROPERTIES FOLDER libs/lithe )
set_target_properties( grammar_json grammar_kronos grammar_common PROPERTIES FOLDER libs/lithe)

add_library( platform STATIC 
	"src/common/Platform.cpp")


source_group( "Backends" FILES
	"src/k3/ModuleBuilder.cpp"
	"src/backends/CodeMotionPass.cpp"
	"src/Backends/CopyElision.cpp"
	"src/backends/DriverSignature.cpp"
	"src/backends/SideEffectCompiler.cpp"
    "src/backends/CopyElision.cpp"
	"src/backends/TargetLowering.cpp"
	#"src/backends/WaveCoreCompiler.cpp"
	#"src/backends/WaveCore.cpp"
	"src/backends/CodeGenModule.cpp"
	"src/backends/CodeGenCompiler.cpp"
	"src/k3/ModuleBuilder.h"
	"src/backends/CodeMotionPass.h"
	"src/Backends/CopyElision.h"
	"src/backends/DriverSignature.h"
	"src/backends/SideEffectCompiler.h"
	"src/backends/TargetLowering.h"
	#"src/backends/WaveCoreCompiler.h" 
	)

source_group( "Graph" FILES
	"src/k3/EnumerableGraph.cpp"
	"src/k3/Conversions.cpp"
	"src/k3/DynamicVariables.cpp"
	"src/k3/Evaluate.cpp"
	"src/k3/FlowControl.cpp"
	"src/k3/FunctionSequence.cpp"
	"src/k3/ImmutableNode.cpp"
	"src/k3/Invariant.cpp"
	"src/k3/LibraryRef.cpp"
	"src/k3/Native.cpp"
	"src/k3/NativeVector.cpp"
	"src/k3/Stateful.cpp"
	"src/k3/TypeAlgebra.cpp"
    "src/k3/EnumerableGraph.h"
    "src/k3/Graph.h"
    "src/k3/NodeBases.h"
	"src/k3/Conversions.h"
    "src/k3/ReactivityGraph.h"
	"src/k3/DynamicVariables.h"
	"src/k3/Evaluate.h"
	"src/k3/FlowControl.h"
	"src/k3/FunctionSequence.h"
	"src/k3/ImmutableNode.h"
	"src/k3/Invariant.h"
	"src/k3/LibraryRef.h"
	"src/k3/Native.h"
	"src/k3/NativeVector.h"
	"src/k3/Stateful.h"
	"src/k3/TypeAlgebra.h")

source_group( "Repository" FILES
	"src/k3/Parser.cpp"
    "src/k3/Parser.h"
	"src/k3/CodeRepository.cpp"
	"src/k3/SourceFileMap.h")

source_group( "Transforms" FILES
	"src/backends/CallGraphAnalysis.cpp"
	"src/k3/Errors.cpp"
	"src/k3/SequenceCodeMotion.cpp" 
	"src/common/Graphviz.cpp"
	"src/k3/Reactive.cpp"
	"src/k3/TypeRuleGenerator.cpp"
	"src/k3/MemoizeTransform.cpp"
	"src/backends/CallGraphAnalysis.h"
	"src/k3/Errors.h"
	"src/k3/SequenceCodeMotion.h" 
	"src/common/Graphviz.h"
	"src/k3/Reactive.h"
	"src/k3/TypeRuleGenerator.h"
	"src/k3/MemoizeTransform.h")

source_group( "Core" FILES
	"src/k3/Type.cpp"
	"src/k3/TLS.cpp" 
	"src/k3/RegionNode.cpp"
	"src/k3/Generic.cpp"
	"src/k3/Typed.cpp" 
	"src/k3/kronos.cpp"
    "src/k3/Type.h"
	"src/k3/TLS.h" 
	"src/k3/RegionNode.h"
	"src/k3/Generic.h"
	"src/k3/Typed.h" 
	"src/kronos.h")

source_group( "Common" FILES
	"src/common/DynamicScope.cpp"
	"src/common/Enumerable.cpp"
	"src/common/Platform.cpp"
	"src/common/Ref.cpp"
	"src/common/ssstring.cpp"
	"src/common/CallStack.cpp"
	"src/k3/SmallContainer.cpp"
    "src/driver/CmdLineOpts.cpp"
	"src/common/DynamicScope.h"
	"src/common/Enumerable.h"
    "src/common/PlatformUtils.h"
	"src/common/Ref.h"
	"src/common/ssstring.h"
	"src/common/CallStack.h"
	"src/k3/SmallContainer.h")

add_library( cli STATIC
	"src/driver/CmdLineOpts.cpp"
	"src/driver/clierror.cpp")

set( IO_LIBS ${PLATFORM_IO_LIBS} pad paf )

function(add_o2)
	
	set(TESTS_BUILD OFF)
	set(BUILD_WITH_DEBUG_SUPPORT OFF)
	set(BUILD_WITH_WEBSOCKET_SUPPORT OFF)
	set(BUILD_WITH_SHAREDMEM_SUPPORT OFF)
    set(BUILD_WITH_BRIDGES OFF)
    set(BUILD_WITH_MTQQ OFF)
    set(BUILD_WITH_MESSAGE_PRINT OFF)
    set(BUILD_WITH_O2LITE_DISCOVERY OFF)
    set(BUILD_WITH_O2LITE_CLOCKSYNC OFF)
	add_definitions("-DO2_EXPORT=extern" -DO2L_NO_BROADCAST -DO2_NO_ZEROCONF)

	add_subdirectory(${O2_DIR} o2)
	set_target_properties(o2_static o2lite_static PROPERTIES FOLDER libs)

endfunction()

if (NOT EMSCSRIPTEN)
	# check for o2
	if(O2_DIR)
	include_directories(${O2_DIR}/src)

	add_o2()
	add_definitions(-DHAS_O2)
	set(IO_LIBS ${IO_LIBS} o2_static)

	endif()

    add_library(core SHARED ${KRONOS_CORE_SOURCES})

	add_library( jsonrpc 
		"src/driver/JsonRPCRepl.cpp"
		"src/driver/JsonRPCEndpoint.h"
		"src/driver/JsonRPCEndpoint.cpp")

	add_executable( kc 
		"src/driver/kc.cpp" )

	add_library( ksubrepl 
		"src/driver/krepl.cpp"
		"src/driver/ext.cpp" 
		"src/driver/TestInstrumentation.cpp")

	add_executable( krepl
		"src/driver/krepldrv.cpp")

	add_executable( krpc
		"src/driver/krpc.cpp")

	add_executable( krpcsrv 
		"src/driver/krpcsrv.cpp")

	add_executable( klangsrv 
		"src/driver/klangsrv.cpp"
		"src/driver/LanguageServer.h" 
		"src/driver/LanguageServer.cpp")

	add_executable( ktests 
		"src/driver/ktests.cpp"
		"src/driver/CompareTestResultJSON.cpp"
		"src/driver/TestInstrumentation.cpp"
		"src/driver/hgrev.cpp"
		"src/driver/hgrev.h")

	add_dependencies( ktests krepl )

	add_library( kiss_fft 
		"src/common/kiss_fft.cpp"
		"src/common/kiss_fftr.cpp")

	add_library( repl 
		"src/driver/ReplEntryBuffer.h"
		"src/driver/ReplEnvironment.h"
		"src/driver/ReplEntryBuffer.cpp"
		"src/driver/ReplEnvironment.cpp")


	add_subdirectory(src/network)

	add_subdirectory(src/runtime)

	add_library( package_manager 
		"src/driver/package.cpp" 
		"src/driver/package.h")
endif()

# embed tinyxml
message(STATUS "Embedding TinyXML")
add_library( tinyxml 
  "src/tinyxml/tinyxml.cpp" 
  "src/tinyxml/tinyxmlerror.cpp" 
  "src/tinyxml/tinyxmlparser.cpp")
include_directories(src/tinyxml)
set_target_properties(tinyxml PROPERTIES FOLDER libs)
set_property(TARGET tinyxml PROPERTY POSITION_INDEPENDENT_CODE ON)
target_link_libraries(cli tinyxml)
if (TARGET core)
  target_link_libraries(core PRIVATE tinyxml)
endif()
if (TARGET kwasm)
  target_link_libraries(kwasm PRIVATE tinyxml)
endif()

# krpc is not marked as a cli app because there's no utf8 handling on windows
set(KRONOS_CLI_APPS kc krepl krpcsrv klangsrv ktests cli)

set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER cmake)
set_property(GLOBAL PROPERTY CTEST_DASHBOARD_TARGETS_FOLDER static_tests)

if(NOT EMSCRIPTEN)
	set_property(TARGET core PROPERTY POSITION_INDEPENDENT_CODE ON)
	set_target_properties( core PROPERTIES COMPILE_DEFINITIONS "K3_EXPORTS" )
	target_link_libraries( core PRIVATE grammar_kronos grammar_json platform )
	if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
		add_library(wconwrapper STATIC src/driver/W32Console.cpp)
		set_target_properties( wconwrapper PROPERTIES FOLDER libs)
		foreach(APP ${KRONOS_CLI_APPS})
			target_link_libraries(${APP} wconwrapper)
		endforeach()
		set_property( TARGET ${KRONOS_CLI_APPS} APPEND_STRING PROPERTY LINK_FLAGS " /ENTRY:wmainCRTStartup")
		set_property( TARGET ${KRONOS_CLI_APPS} APPEND_STRING PROPERTY COMPILE_DEFINITIONS " _UNICODE")
	endif()
	set_target_properties( kc krpc krpcsrv ksubrepl PROPERTIES COMPILE_DEFINITIONS "K3_IMPORTS" )
	set_target_properties( kc krepl ktests klangsrv krpc krpcsrv PROPERTIES FOLDER apps)
	set_target_properties( platform cli package_manager kiss_fft repl network jsonrpc ksubrepl PROPERTIES FOLDER libs)
#	set_target_properties( kronosmrt kronosio PROPERTIES FOLDER runtime)

	set_property( TARGET klangsrv APPEND PROPERTY COMPILE_DEFINITIONS "DISABLE_WIN32_UTF8")
	find_package(Threads REQUIRED)
	message(STATUS "Using Threads ${CMAKE_THREAD_LIBS_INIT}")

	if (NOT ${LIB_READLINE} STREQUAL "LIB_READLINE-NOTFOUND")
		target_link_libraries( repl ${LIB_READLINE})
	endif()

	# libraries 
	target_link_libraries( cli platform )
	target_link_libraries( package_manager platform )
	target_link_libraries( core PRIVATE paf )
	target_link_libraries( package_manager network lithe grammar_json)
	target_link_libraries( kc core cli package_manager)
	target_link_libraries( ksubrepl core cli repl kronosmrt kronosio kiss_fft jsonrpc package_manager Threads::Threads )
	target_include_directories( ksubrepl INTERFACE src/driver)
	target_link_libraries( krepl ksubrepl )
	target_link_libraries( krpc core cli repl kronosmrt kronosio jsonrpc package_manager Threads::Threads )
	target_link_libraries( krpcsrv core cli repl kronosmrt kronosio kiss_fft network jsonrpc package_manager Threads::Threads )
	target_link_libraries( klangsrv jsonrpc grammar_kronos package_manager )
	target_link_libraries( ktests paf ksubrepl )

	# Add the rpath of libraries in same directory of the executables (like Windows' .dll)
	if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
		set_target_properties( kc PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
		set_target_properties( krepl PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
		set_target_properties( krpc PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
		set_target_properties( krpcsrv PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
		set_target_properties( klangsrv PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
		set_target_properties( ktests PROPERTIES LINK_FLAGS "-Wl,-rpath,@loader_path" )
    elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
		set_target_properties( kc PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
		set_target_properties( krepl PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
		set_target_properties( krpc PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
		set_target_properties( krpcsrv PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
		set_target_properties( klangsrv PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
		set_target_properties( ktests PROPERTIES LINK_FLAGS "-Wl,-rpath,@ORIGIN" )
	endif()

	if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
		target_link_libraries( platform atomic )
		target_link_libraries( ksubrepl atomic )
	endif()

	if(CURL_FOUND)
		target_link_libraries( package_manager ${CURL_LIBRARIES})
	endif()

	include(cmake/Test.cmake)
	include(cmake/Package.cmake)
endif()

if (TARGET llvm_backend AND TARGET core)  
	MESSAGE(STATUS "Including LLVM backend")
	target_link_libraries( core PRIVATE llvm_backend)
endif()

if (TARGET binaryen_backend AND TARGET core) 
	MESSAGE(STATUS "Including Binaryen backend")
	target_link_libraries( core PRIVATE binaryen_backend )
endif()

file(STRINGS "version_stdlib.txt" KRONOS_CORE_LIBRARY_VERSION)

set(KRONOS_CORE_LIBRARY_REPOSITORY "kronoslang/core" CACHE STRING "Package for the runtime library")

message(STATUS "Core Library ${CMAKE_SOURCE_DIR}/library/ [${KRONOS_CORE_LIBRARY_REPOSITORY} ${KRONOS_CORE_LIBRARY_VERSION}] (${LIBRARY_VERSION_RESULT})")

configure_file(config/corelib.h.in "${CMAKE_CURRENT_BINARY_DIR}/headers/config/corelib.h")

function(KDCC_SUBCOMMAND subcmd)
	set(subtarget "kdcc_${subcmd}")
	string(TOUPPER ${subtarget} subdef)

	add_executable(${subtarget} src/website/kdcc.cpp ${ARGN})

#	target_link_libraries(${subtarget} package_manager grammar_kronos)

	set_target_properties( ${subtarget} PROPERTIES FOLDER website COMPILE_DEFINITIONS "${subdef}")
#	target_link_libraries( ${subtarget} ksubrepl )
endfunction()

if(NOT EMSCRIPTEN)
	if (KRONOS_WEBSITE)
		message(STATUS "Building Website from ${KRONOS_WEBSITE}")
		kdcc_subcommand(module src/website/mod2json.cpp)
		kdcc_subcommand(page src/website/md2json.cpp src/website/subrepl.cpp src/website/attachment.cpp)
		kdcc_subcommand(link src/website/makehtml.cpp src/website/attachment.cpp)
		kdcc_subcommand(upload src/website/makehtml.cpp src/website/attachment.cpp)

		target_link_libraries(kdcc_module package_manager grammar_kronos)
		target_link_libraries(kdcc_page grammar_kronos ksubrepl)
		target_link_libraries(kdcc_link package_manager)
		target_link_libraries(kdcc_upload package_manager)

		add_subdirectory(${KRONOS_WEBSITE} web)
	endif()
endif()

string(TIMESTAMP CURRENT_YEAR "%Y")
configure_file(config/config.h.in "${CMAKE_CURRENT_BINARY_DIR}/headers/config/system.h")

get_property(BUILD_TARGETS GLOBAL PROPERTY BUILDSYSTEM_TARGETS)
message(STATUS Building ${BUILD_TARGETS})
