set(WAVECORE_EIGEN_INCLUDE_DIR CACHE PATH "")
option(BUILD_WAVECORE_DRIVER "Build the experimental Heterogeneous Computation Driver for WaveCore" OFF)

if (BUILD_WAVECORE_DRIVER)
	add_executable( kwc 
		"src/driver/kwc.cpp" "src/driver/ext.cpp" )

	target_include_directories( kwc   PUBLIC ${WAVECORE_EIGEN_INCLUDE_DIR} )
	target_include_directories( krepl PUBLIC ${WAVECORE_EIGEN_INCLUDE_DIR} )
	set_target_properties( kwc PROPERTIES FOLDER Drivers)
	target_link_libraries( kwc kronos cli kiss_fft )
	set_target_properties( kwc PROPERTIES COMPILE_FLAGS "-D_CONSOLE")
endif (BUILD_WAVECORE_DRIVER)
