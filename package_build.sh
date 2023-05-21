DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

cd $DIR
mkdir -p kronos
mkdir -p kronos/bin
mkdir -p kronos/lib
mkdir -p kronos/include
cp src/*.h kronos/include
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
elif [[ "$OSTYPE" == "linux-gnu"* ]] || [[ "$OSTYPE" == "darwin"* ]]; then
	cp build/bin/kc kronos/bin
	cp build/bin/krepl kronos/bin
	cp build/bin/ktests kronos/bin
	cp build/bin/klangsrv kronos/bin
	cp build/bin/krpc kronos/bin
	cp build/bin/krpcsrv kronos/bin
	cp build/*.a kronos/lib
	cp -a build/libkronos* kronos/bin
	cp -a build/libkronos* kronos/lib
fi
