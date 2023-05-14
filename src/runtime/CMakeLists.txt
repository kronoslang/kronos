add_library( kronosio 
	"inout.cpp"
	"inout.h"
	"audio.cpp"
	"audio.h"
	"midi.cpp"
	"midi.h"
	"o2driver.cpp" 
	"o2driver.h"
	"timercallback.cpp")

add_library( kronosmrt 
	"kronosrt.cpp"
	"scheduler.cpp"
	"interop.cpp"
	"kronosrtxx.h"
	"scheduler.h"
	"../kronosrt.h" )

target_link_libraries( kronosio PUBLIC ${IO_LIBS} )
target_link_libraries( kronosmrt paf )
set_target_properties( kronosio kronosmrt PROPERTIES FOLDER runtime)
