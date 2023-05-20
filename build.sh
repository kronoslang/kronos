LLVM_VER="6.0.1"
DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

# Download and build LLVM
if [ ! -d $DIR/llvm ]; then
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
cmake -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR=$DIR/llvm/llvm/build/lib/cmake/llvm/ ..
cmake --build . --config "Release" -j $(nproc)

# Move all relevant files to the kronos folder
cd $DIR
mkdir -p kronos
mkdir -p kronos/bin
mkdir -p kronos/lib
mkdir -p kronos/include
if [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "msys" ]]; then
	cp build/bin/Release/kronos.dll kronos/bin
	cp build/bin/Release/kronos.dll kronos/lib
	cp build/bin/Release/kc.exe kronos/bin
	cp build/bin/Release/krepl.exe kronos/bin
	cp build/bin/Release/ktests.exe kronos/bin
	cp build/bin/Release/klangsrv.exe kronos/bin
	cp build/bin/Release/krpc.exe kronos/bin
	cp build/bin/Release/krpcsrv.exe kronos/bin
	cp build/Release/*.lib kronos/lib
	cp src/*.h kronos/include
elif [[ "$OSTYPE" == "linux-gnu"* ]] || [[ "$OSTYPE" == "darwin"* ]]; then
	echo "TODO"
fi