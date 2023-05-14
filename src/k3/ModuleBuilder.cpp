#include "backends/SideEffectCompiler.h"
#include "backends/CodeMotionPass.h"
#include "backends/DriverSignature.h"
#include "backends/CallGraphAnalysis.h"
#include "common/PlatformUtils.h"
#include "ModuleBuilder.h"
#include "RegionNode.h"
#include "Parser.h"
#include "NodeBases.h"
#include "TLS.h"
#include "LibraryRef.h"
#include "kronos.h"

#include "backends/LLVMCmdLine.h"

#include <sstream>
#include <tuple>

#include <iostream>

namespace CL {
	CmdLine::Option<int> OptLevel(2, "--opt-level", "-O", "<level>", "Invoke backend optimizers for level 0 (none) to 3 (aggressive)");
}

namespace K3 {
	Type GetResultReactivity(CRRef node) {
		if (node->IsFused()) {
			Reactive::DriverSet ds;
			for (auto dn : Qxx::FromGraph(node).OfType<Reactive::DriverNode>()) {
				ds.insert(dn->GetID());
			}
			Type drivers(false);
			ds.for_each([&drivers](DriverSignature sig) {
				drivers = Type::Pair(sig.GetMetadata(), drivers);
			});
			static TypeDescriptor rx("drivers");
			return Type::User(&rx, drivers);
		} else {
			return Type::Pair(
				GetResultReactivity(node->First()),
				GetResultReactivity(node->Rest())
				);
		}
	}

	void Module::SetGlobalVariableReactivity(const void* uid, const Reactive::Node *r) { 
		if (globalReactivityTable[uid] && globalReactivityTable[uid] != r) 
		globalReactivityTable[uid] = r; 
	}

	Module::Module(const Type& argument, const Type& res):arg(argument), result(res), buildMemory(new MemoryRegion), freeSymbolIndex(0), numSignalMaskBits(0) {
		if (arg.GetSize()) {
			RegisterExternalVariable(Type("arg"), arg, this, Internal, std::make_pair(0,0), Type::Nil);
			GetIndex(this);
		}
	}
        
    void Module::StandardBuild(CTRef AST, const Type& argument, const Type& result) {
		using namespace Nodes;
		RegionAllocator buildAllocator(buildMemory);

        intermediateAST = AST;
			/* global code motion */
//			puts("[Global Code Motion]\n");

			//Backends::EquivalenceClassMap equivalentExpressions; {
			//	RegionAllocator codeMotionCollector;
			//	Backends::CodeMotionAnalysis(intermediateAST,equivalentExpressions,Backends::EquivalentExpression(),0,0).Go();

			//	for(auto i(equivalentExpressions.begin());i!=equivalentExpressions.end();) {
			//		if (i->second.count<2) equivalentExpressions.erase(i++);
			//		else {
			//			i++;
			//		}
			//	}
			//}
//			intermediateAST = Backends::CodeMotionPass(intermediateAST,equivalentExpressions,true).Go();

			//puts("[Reactive Analysis]\n");			
        Reactive::FusedSet *nullReactivity(new Reactive::FusedSet);


        initializer = new Reactive::DriverNode(
            DriverSignature(Type(&Reactive::InitializationDriver)));

        const Reactive::DriverNode *argReactivity(
            new Reactive::DriverNode(DriverSignature(Type(&Reactive::ArgumentDriver))));

        CRRef outRx;
        /* reactive analysis and boundaries */
        intermediateAST = Graph<Typed>(
            Reactive::Analysis(
                BeforeReactiveAnalysis(intermediateAST),*this,argReactivity,nullReactivity).Go(outRx));

        outputReactivity = GetResultReactivity(outRx);
	}

	void Module::RegisterExternalVariable(const Type& key, const Type& data, const void* uid, GlobalVarType varType, std::pair<int,int> rate, const Type& clock) {
		std::lock_guard<std::mutex> guard(symbolTableLock);
		auto f(globalKeyTable.find(key));
		if (f == globalKeyTable.end()) {
			GlobalVarData varData { uid, data, varType, rate, clock };
			globalKeyTable.insert(std::make_pair(key,varData));
		} else {
			assert (globalKeyTable[key].uid == uid);
		}
	}

	CRRef Module::GetGlobalVariableReactivity(const void *uid) {
		auto f(globalReactivityTable.find(uid));
		if (f == globalReactivityTable.end()) return 0;
		else return f->second;
	}

	int Module::OrdinalCompare(const Type& driver1, const Type& driver2) const {
		return DriverSignature(driver1).OrdinalCompare(driver2);
	}

	unsigned Module::GetIndex(const void *uid) {
		std::lock_guard<std::mutex> guard(symbolTableLock);		
		auto f(globalSymbolTable.find(uid));
		if (f!=globalSymbolTable.end()) return f->second;
		else return globalSymbolTable.insert(std::make_pair(uid,freeSymbolIndex++)).first->second;
	}

	unsigned Module::GetIndex() {
		return freeSymbolIndex++;
	}
}