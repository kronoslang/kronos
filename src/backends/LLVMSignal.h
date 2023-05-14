#pragma once

#include "common/Ref.h"
#include "SmallContainer.h"

namespace llvm{
	class Value;
};

namespace K3 {
	namespace Nodes{
		class Deps;
	};

	namespace Backends{
		class LLVMSignal : public RefCounting
		{
			llvm::Value* val;
			Ref<LLVMSignal> reference;
		public:
			LLVMSignal(llvm::Value *v=0);

			operator llvm::Value*() {return val;}
			operator const llvm::Value*() const {return val;}
#ifndef NDEBUG
            int offsetCounter = 0;
#endif
		};

		using LLVMValue = llvm::Value*;
		static inline LLVMValue Val(llvm::Value* val) {
			return val;
		}
	};
};
