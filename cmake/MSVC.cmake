add_definitions(
		/GR- /permissive- 
		-D_CRT_SECURE_NO_DEPREATE 
		-D_CRT_NONSTDC_NO_DEPRECATE 
		-D_CRT_SECURE_NO_WARNINGS
		-D_SCL_SECURE_NO_WARNINGS
		-D_ENABLE_EXTENDED_ALIGNED_STORAGE
		-DTTMATH_NOASM
		-DNOMINMAX
			"/wd4458 /wd4457 /wd4456 /wd4624 /wd4250 /wd4141 /wd4291 /wd4018 /wd4503 /wd26495")
	
add_custom_target(debugger_support SOURCES editors/kronos.natvis src/pcoll/pcoll.natvis)

setDefaultPackageGenerator(BINARY_WIX)