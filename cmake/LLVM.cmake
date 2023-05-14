find_package(LLVM 6 REQUIRED CONFIG PATHS "${CMAKE_BINARY_DIR}/../llvm/share/llvm/cmake")

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} targeting ${LLVM_TARGETS_TO_BUILD}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(llvm_libs mcjit passes codegen interpreter objcarcopts object bitwriter
	"${LLVM_TARGETS_TO_BUILD}")

add_library(llvm_backend 
	"src/backends/LLVMCompiler.cpp"
	"src/backends/LLVMModule.cpp"
	"src/backends/LLVMScatterLoad.cpp"
	"src/backends/LLVMAoT.cpp"
    "src/backends/LLVMJiT.cpp"
    "src/backends/LLVMOpt.cpp"
	"src/backends/LLVMCompiler.h"
	"src/backends/LLVMModule.h"
	"src/backends/LLVMSignal.h"
	"src/backends/LLVMUtil.h"
)

target_link_libraries( llvm_backend PRIVATE ${llvm_libs})
set_target_properties( llvm_backend PROPERTIES FOLDER libs/emitters)

set(HAVE_LLVM True)