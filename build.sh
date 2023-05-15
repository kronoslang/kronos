VER="6.0.1"
CORES=$(cat /proc/cpuinfo | grep "processor" | wc -l)
DIR=$(dirname "$0")

# Download and build LLVM
if [ ! -d $DIR/llvm ]; then
	cd $DIR
	wget https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-$VER.zip
	unzip llvmorg-$VER.zip
	rm llvmorg-$VER.zip
	mv llvm-project-llvmorg-$VER llvm
	cd llvm/llvm
	mkdir build && cd build
	cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_BUILD_EXAMPLES=False -DLLVM_BUILD_TOOLS=False -DLLVM_TARGETS_TO_BUILD=host ..
	make -j$CORES
fi

# Build Kronos
cd $DIR
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=./llvm/llvm/build/lib/cmake/llvm/ ..
make -j$CORES
