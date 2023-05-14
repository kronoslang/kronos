add_library( network 
	"Sxx.cpp"
	"Sxx.h"
	"websocket.cpp"
	"websocket.h"
	"router.cpp"
	"router.h"
	"http.cpp"
	"http.h" )

target_link_libraries( network lithe )