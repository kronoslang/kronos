#pragma once
#include "NodeBases.h"
#include "CompilerNodes.h"
#include "Reactive.h"
#include "DynamicVariables.h"
#include "MemoizeTransform.h"
#include "FlowControl.h"

namespace K3 {
	namespace Nodes{
		class FunctionCall;
		class FunctionSequence;
		class FunctionBase;
		class Argument;
	};
	using namespace Nodes;
	namespace Backends{

		class CopyElisionTransform //: public GraphTransform<const Typed,void>
		{
			friend class ::K3::Nodes::FunctionCall;
			friend class ::K3::Nodes::FunctionSequence;
			friend class ::K3::Nodes::RecursionBranch;
			friend class ::K3::Nodes::Argument;
			friend class ::K3::Nodes::Switch;
			CTRef elision;
		public:
			typedef Sml::Map<CTRef,CTRef> ElisionMap;
		private:
			ElisionMap &sfx;
		public:
			CopyElisionTransform(CTRef elision, ElisionMap& map):elision(elision),sfx(map) {}
			CopyElisionTransform Pass(CTRef newElision) {return CopyElisionTransform(newElision,sfx);}
			CTRef& operator[](CTRef node);
			void operate(CTRef t);
			void operator()(CTRef t) {return operate(t);}
			CTRef GetElision() {return elision;}
		};

		using IInstanceMemoizeKey = std::tuple<Graph<Typed>, Graph<Typed>, Graph<Typed>>;
		using IInstanceMemoizeValue = Graph<Typed>;
		using IInstanceMemoize = Transform::IMemoized<IInstanceMemoizeKey, IInstanceMemoizeValue>;
		using InstanceMemoize = Transform::Memoized<IInstanceMemoizeKey, IInstanceMemoizeValue>;
		class IInstanceSymbolTable : public virtual IInstanceMemoize {
		public:
			virtual void RegisterExternalVariable(const Type& key, const Type& type, const void* uid, GlobalVarType varType, std::pair<int,int> sampleRateMultiplier, const Type& clock) = 0;
			virtual unsigned GetIndex(const void *uid) = 0;
			virtual const Reactive::Node* GetInitializerReactivity() const = 0;
		};

		class SideEffectTransform : public CachedTransformBase<const Typed,const Typed*>{			
			struct SideEffect {
				CTRef WritePointer;
				CTRef WriteValue;
				CTRef ReadPointer;
				CRRef Reactivity;
				std::int64_t size;
			};

			CTRef arguments, results, statePointer, localStatePointer;
			CopyElisionTransform& elision;
			std::vector<SideEffect> sfx;
			IInstanceSymbolTable& symbols;
			Subroutine* recursiveBranch = nullptr;
			bool allocatesState = false, mutatesGVars = false;
		#ifndef NDEBUG
			std::unordered_set<const Nodes::FunctionBase*> visitedFunctions;
		#endif
		public:
			void AllocatesState() { allocatesState = true; }
			void MutatesGVars() { mutatesGVars = true; }
			void SetRecursiveBranch(Subroutine* recurBranch) { assert(recursiveBranch == nullptr && "Sequence must have a single recursive branch"); recursiveBranch = recurBranch; }

			IInstanceSymbolTable& GetSymbolTable() {return symbols;}

			SideEffectTransform(IInstanceSymbolTable& tbl, CTRef root,CTRef arguments, CTRef results, map_t& map, CopyElisionTransform& elision)
				:CachedTransformBase(root, map), arguments(arguments), results(results), elision(elision), symbols(tbl) {
				// make these distinct objects to prevent data hazard detection (which depends on node identity)
				// that is ok because local state and state will never alias, because the 
				// initial state pointer will be patched to be the end of the local state chain
				// as the final compilation step
				localStatePointer = SubroutineArgument::State();
				statePointer = SubroutineArgument::In(-2, nullptr, 4, "local_state");
			}

			static CTRef GetDataLayout(CTRef graph);
			static CTRef GetDataAccessor(CTRef graph);
			static CTRef GetDereferencedAccessor(CTRef graph);
			static CTRef GetDereferencedAccessor(CTRef graph, CRRef rx);

			CTRef GetLocalStatePointer() { return localStatePointer; }
			void SetLocalStatePointer(CTRef newLocalState) { localStatePointer = newLocalState; AllocatesState(); }
			CTRef GetStatePointer() { return statePointer; }
			void SetStatePointer(CTRef newStatePointer) { statePointer=newStatePointer; }

			CTRef CopyData(CTRef dst, CTRef src, const Reactive::Node* reactivity, bool byValue, bool mutatesState, bool doesInit);
			const Typed* operate(CTRef src);

			const Typed* ComputeResultSize(const Typed* graph);

			CTRef Process(CTRef body, const Reactive::Node* rootReactivity);

			void AddSideEffect(CTRef WritePtr, CTRef WriteVal, CTRef ReadPtr, CRRef Reactivity, std::int64_t sizeIfKnown = 0);
			
			static Graph<Typed> Compile(IInstanceSymbolTable& symbols, const CTRef body, const CTRef args, const CTRef results, const char *l, const Type& argTy, const Type& resTy);
			static Graph<Typed> Compile(IInstanceSymbolTable& symbols, CTRef body, const Type& args, const Type& results); // unity-mapped, for C-callability

			CTRef AllocateBuffer(CTRef size);

			CopyElisionTransform& GetElision() { return elision; }
			CTRef GetArgument() { return arguments; }

			void CompileSubroutineAsync(const char* l, const Type&, const Type&, Subroutine*, CTRef, CTRef, CTRef, bool synchronous = false);		

		};
	};
};