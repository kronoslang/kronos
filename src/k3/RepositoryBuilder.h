#pragma once

#include "common/Err.h"

namespace K3 {
	namespace Parser {
		class Repository2;

		struct RepositoryBuilder {
			std::string path;
			Repository2& r;
			void AddMacro(const char*, Nodes::CGRef, bool);
			void AddFunction(const char*, Nodes::CGRef, const char* args="", const char *docs = "", const char *fallback = nullptr);
			RepositoryBuilder AddPackage(const char* name) {
				return RepositoryBuilder{ path + name + ":", r };
			}
			~RepositoryBuilder();
			MemoryRegion* GetMemoryRegion();
		};
	}
}
