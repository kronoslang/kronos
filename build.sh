LLVM_VER="6.0.1"
DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
LLVM_DIR="$DIR/llvm/llvm/build/lib/cmake/llvm"

# Download and build LLVM
if [ ! -d $LLVM_DIR ]; then
	cd $DIR
	wget https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-$LLVM_VER.zip
	unzip llvmorg-$LLVM_VER.zip
	rm llvmorg-$LLVM_VER.zip
	mv llvm-project-llvmorg-$LLVM_VER llvm
	cd llvm/llvm
	mkdir build && cd build
	cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_BUILD_EXAMPLES=False -DLLVM_BUILD_TOOLS=False -DLLVM_TARGETS_TO_BUILD=host ..
	cmake --build . --config "Release" -j $(nproc)
fi

# Build Kronos
cd $DIR
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=$LLVM_DIR ..
cmake --build . --config "Release" -j $(nproc)

# Move all relevant files to the kronos folder
cd $DIR
sh ./package_build.sh
