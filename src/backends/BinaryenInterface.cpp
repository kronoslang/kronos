#include "binaryen-c.h"
#include "wasm-printing.h"

void BinaryenEmitWAST(BinaryenModuleRef M, std::ostream& writeToStream) {
	wasm::WasmPrinter wp;
	wp.printModule((wasm::Module*)M, writeToStream);
}

void BinaryenEmitAsmJS(BinaryenModuleRef M, std::ostream& writeToStream) {
	abort();
}
