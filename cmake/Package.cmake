set(CPACK_PACKAGE_NAME "Kronos")
set(CPACK_PACKAGE_VERSION ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH})

set_target_properties( core
		       PROPERTIES 
		       OUTPUT_NAME kronos
		       PUBLIC_HEADER "src/kronos.h;src/kronos_abi.h;src/kronosrt.h"
		       VERSION ${CPACK_PACKAGE_VERSION}
		       SOVERSION ${CPACK_PACKAGE_VERSION_MAJOR} )

### Installation ###

set(CPACK_PACKAGE_NAME "Kronos ${CPACK_PACKAGE_VERSION}")
set(CPACK_PACKAGE_VENDOR "UniArts Helsinki")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Kronos - A compiler for musical signal processors")

set(CPACK_WIX_PROGRAM_MENU_FOLDER  "Kronos Compiler Suite")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/package/kronos-icon.ico")
set(CPACK_WIX_UPGRADE_GUID d7b262a0-2efc-4ec6-9506-537e3d959507)
set(CPACK_WIX_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/package/msvc14_wix.wxi.in.xml")
set(CPACK_WIX_CMAKE_PACKAGE_REGISTRY Kronos)
set(CPACK_WIX_EXTENSIONS WixUIExtension WixUtilExtension)
set(CPACK_WIX_EXTRA_FLAGS sw)

set(CPACK_PACKAGE_INSTALL_DIRECTORY "Kronos")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Vesa Norilo <vnorilo@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_NAME "kronos-${CPACK_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://bitbucket.org/vnorilo/k3")
set(CPACK_DEBIAN_PACKAGE_PROVIDES "kronos,krepl,kc,krpc,ktests,klangsrv")

#set(CPACK_STRIP_FILES core krepl krpcsrv kc ktests)
# stripping removes all the kvm_* functions that are dynamically linked to

set(CPACK_BUNDLE_NAME "kronos")

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/package/installer_eula.rtf")

set(CPACK_COMPONENT_CORE_DISPLAY_NAME "Core")
set(CPACK_COMPONENT_CXXDEV_DISPLAY_NAME "C++ developer tools")
set(CPACK_COMPONENT_STATIC_COMPILER_DISPLAY_NAME "Static compiler")
set(CPACK_COMPONENT_LANGUAGE_SERVER_NAME "Language server")
set(CPACK_COMPONENT_REPL_DISPLAY_NAME "REPL")
set(CPACK_COMPONENT_RPCSERVER_DISPLAY_NAME "RPC Server")
set(CPACK_COMPONENT_TESTS_DISPLAY_NAME "Self-test suite")

set(CPACK_COMPONENT_CORE_DESCRIPTION "The Kronos compiler core")
set(CPACK_COMPONENT_CXXDEV_DESCRIPTION "C++ headers and import library for developers")
set(CPACK_COMPONENT_LANGUAGE_SERVER_DESCRIPTION "Language Server Protocol implementation for IDE integration")
set(CPACK_COMPONENT_STATIC_COMPILER_DESCRIPTION "Produces dependency-free object files with C ABI")
set(CPACK_COMPONENT_REPL_DESCRIPTION "Read-eval-print-loop with sound and I/O capabilities")
set(CPACK_COMPONENT_RPCSERVER_DESCRIPTION "Websocket JiT compiler server")
set(CPACK_COMPONENT_TESTS_DESCRIPTION "Tests and demonstrates compiler functionality")

include(GNUInstallDirs)
set(_LIBDIR ${CMAKE_INSTALL_LIBDIR}/${CMAKE_LIBRARY_ARCHITECTURE})
set(_BINDIR ${CMAKE_INSTALL_BINDIR})
set(_INCDIR ${CMAKE_INSTALL_INCLUDEDIR})

if (WIN32)
	install( FILES package/install_vcredist.bat DESTINATION bin COMPONENT core )
elseif(APPLE)
	set(CPACK_PACKAGING_INSTALL_PREFIX "/usr/local/")
	set(CMAKE_INSTALL_PREFIX "/usr/local/")
endif()

install( TARGETS core EXPORT KronosTargets
	RUNTIME DESTINATION ${_BINDIR} COMPONENT core
	LIBRARY DESTINATION ${_LIBDIR} COMPONENT core
	ARCHIVE DESTINATION ${_LIBDIR} COMPONENT cxxdev
	PUBLIC_HEADER DESTINATION include COMPONENT cxxdev
    FRAMEWORK DESTINATION "/Library/Frameworks" COMPONENT core )

target_include_directories(core INTERFACE 
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc/k3>
	$<INSTALL_INTERFACE:include>)

install( TARGETS ktests
	RUNTIME DESTINATION ${_BINDIR}
	COMPONENT tests
	OPTIONAL )

install( TARGETS kc
	RUNTIME DESTINATION ${_BINDIR}
	COMPONENT static_compiler 
)

install( TARGETS krepl krpc
	RUNTIME DESTINATION ${_BINDIR}
	COMPONENT repl
)

install( TARGETS krpcsrv
	RUNTIME DESTINATION ${_BINDIR}
	COMPONENT rpcserver
	OPTIONAL )

install( TARGETS klangsrv 
	RUNTIME DESTINATION ${_BINDIR}
	COMPONENT language_server
	OPTIONAL )

include(CMakePackageConfigHelpers)
include(GenerateExportHeader)

write_basic_package_Version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/Kronos/KronosConfigVersion.cmake"
	VERSION "${CPACK_PACKAGE_VERSION}"
	COMPATIBILITY AnyNewerVersion
)

export(EXPORT KronosTargets
	FILE "${CMAKE_CURRENT_BINARY_DIR}/Kronos/KronosTargets.cmake"
	NAMESPACE Kronos::)

configure_file(package/KronosConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/Kronos/KronosConfig.cmake"
	COPYONLY)

set(ConfigPackageLocation share/kronos/cmake)

install(EXPORT KronosTargets
		FILE KronosTargets.cmake
		NAMESPACE Kronos::
		DESTINATION ${ConfigPackageLocation})

install(FILES
	package/KronosConfig.cmake
	"${CMAKE_CURRENT_BINARY_DIR}/Kronos/KronosConfigVersion.cmake"
	DESTINATION ${ConfigPackageLocation}
	COMPONENT cxxdev)

if (WIN32) 
	install( FILES package/kronos_shell.bat 
			 DESTINATION "/"
			 COMPONENT core)
	set(CPACK_WIX_CMAKE_PACKAGE_REGISTRY kronos)
endif()

if (APPLE)
	set(CPACK_OSX_PACKAGE_VERSION ${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()
string(TOLOWER "kronos-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" CPACK_PACKAGE_FILE_NAME)
set(CPACK_PACKAGE_EXECUTABLES "krepl;Kronos REPL")

include(CPack)
