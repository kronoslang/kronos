#include "binaryen-c.h"
#include <ostream>

void BinaryenEmitWAST(BinaryenModuleRef M, std::ostream& writeToStream) {
	char *text = BinaryenModuleAllocateAndWriteText(M);
	writeToStream << text;
	free(text);
}

void BinaryenEmitAsmJS(BinaryenModuleRef M, std::ostream& writeToStream) {
}
