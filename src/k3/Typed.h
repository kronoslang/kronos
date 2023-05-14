#pragma once
#include "RegionNode.h"
#include "Transform.h"
#include "Type.h"
#include "ReactivityGraph.h"

#include "common/Reflect.h"
#include "common/Ref.h"
#include "backends/LLVMSignal.h"
#include "backends/Binaryen.h"

#include "config/system.h"

#include "Errors.h"

#define TYPED_NODE_INLINE(CLASS, ...) \
class CLASS : META_MAP(CXX_INHERIT_PUBLIC,__VA_ARGS__) virtual public mu {\
REGION_ALLOC(CLASS) INHERIT_RENAME(CLASS, __VA_ARGS__) virtual CLASS* ConstructShallowCopy() const override {return new CLASS(*this);}

#define TYPED_NODE(CLASS,...) \
TYPED_NODE_INLINE(CLASS,__VA_ARGS__)\
const char *PrettyName() const override {return #CLASS;}\

#ifdef HAVE_LLVM
#define LLVM_EMITTER Backends::LLVMValue Compile(Backends::LLVMTransform&, Backends::ActivityMaskVector*) const override; 
#define LLVM_EMIT(CLASS)
#else 
#define LLVM_EMITTER
#define LLVM_EMIT(CLASS)
#endif

#ifdef HAVE_BINARYEN
#define BINARYEN_EMITTER void* Compile(Backends::BinaryenTransform&, Backends::ActivityMaskVector*) const override;
#define BINARYEN_EMIT(CLASS) void* CLASS::Compile(Backends::BinaryenTransform& xfm, Backends::ActivityMaskVector* avm) const { return GenericCompile(xfm, avm); } 
#else
#define BINARYEN_EMITTER
#define BINARYEN_EMIT(CLASS)
#endif

#define CODEGEN_EMITTER LLVM_EMITTER BINARYEN_EMITTER \
template <typename TXfm> auto GenericCompile(TXfm& xfm, Backends::ActivityMaskVector* avm) const -> decltype(xfm(this, avm));

#define CODEGEN_EMIT(CLASS) LLVM_EMIT(CLASS) BINARYEN_EMIT(CLASS) \
template <typename TXfm> auto CLASS::GenericCompile(TXfm& xfm, Backends::ActivityMaskVector* avm) const -> decltype(xfm(this, avm))

namespace K3 {
	//class Reactivity;

	namespace Reactive {
		class Analysis;
		enum Active{
			No,
			Maybe,
			Yes
		};
	};

	namespace Backends {
		class SideEffectTransform;
		class CopyElisionTransform;

		class LLVMTransform;
		class ActivityMaskVector;
	};


	namespace Nodes{
        class SpecializationTransform;        
		class ResultTypeTransform;
		/* Generic graph node that represents a node of the user program AST */
		class TypedBase : public RegionNodeBase {
			friend class SpecializationTransform;
		protected:
			const Reactive::Node* reactivity;
		public:
			const Reactive::Node* GetReactivity() const {return reactivity;}
			void SetReactivity(const Reactive::Node* r) {assert(r!=(const Reactive::Node*)0xcccccccccccccccc);reactivity = r;}
			TypedBase():reactivity(0){}

			virtual Type Result(ResultTypeTransform& rtt) const = 0;

			static bool VerifyAllocation(MemoryRegion* hostRegion, const Typed *node);

			virtual int LocalCompare(const ImmutableNode& rhs) const;
		};

		class Typed;
		typedef const Typed* CTRef;

		class Typed : public CachedTransformNode<TypedBase>, REFLECTING_CLASS{
			virtual void *GetTypeIdentifier() const {return (void*)TypeID();}
			CTRef& _GetUp(unsigned int idx) {assert(idx<GetNumCons());return (CTRef&)upstream[idx];}
		public:
			virtual const char *GetLabel() const {return PrettyName();}
			CTRef GetUp(unsigned int idx) const {return (CTRef)GetCon(idx);}
			CTRef Reconnect(unsigned int idx, CTRef newCon) {_GetUp(idx)=newCon;newCon->globalDownstreamCount++;return newCon;}
			void Connect(CTRef up) {up->globalDownstreamCount++;CachedTransformNode::Connect(up);}
			unsigned ComputeLocalHash() const { auto h(TypedBase::ComputeLocalHash());HASHER(h,(uintptr_t)TypeID()); return h;}
			static CTRef Nil();
			static bool IsNil(CTRef);

			/* transform dispatchers */	
			virtual CTRef IdentityTransform(GraphTransform<const Typed,CTRef>& copyTable) const;
			virtual const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const;
			virtual CTRef ReactiveReconstruct(Reactive::Analysis&) const;
			virtual CTRef SideEffects(Backends::SideEffectTransform&) const;
			virtual void CopyElision(Backends::CopyElisionTransform&) const;
			virtual Typed* ConstructShallowCopy() const = 0; // should be private?
		
			virtual Backends::LLVMValue Compile(Backends::LLVMTransform&, Backends::ActivityMaskVector*) const {return Backends::LLVMValue();}
			virtual void* Compile(Backends::BinaryenTransform& xfm, Backends::ActivityMaskVector* avm) const { return nullptr; };

			virtual int SchedulingPriority() const {return 0;}
			virtual int GetWeight() const { return 0; }

			virtual CTRef GraphFirst() const;
			virtual CTRef GraphRest() const;

			const CTRef* GetConnectionArray() const {return (const CTRef*)upstream;}
			class UpstreamCollection
			{
				const CTRef *beg;
				const CTRef *e;
			public:
				typedef const CTRef* const_iterator;
				UpstreamCollection(const_iterator b, const_iterator e):beg(b),e(e){}
				const_iterator begin() const {return beg;}
				const_iterator end() const {return e;}
			};
			UpstreamCollection Upstream() const {return UpstreamCollection(GetConnectionArray(),GetConnectionArray()+GetNumCons());}
		};

		class IFixedResultType : REFLECTING_CLASS {
			INHERIT(IFixedResultType,Reflecting);
		public:
			virtual Type FixedResult() const = 0;
		};

		class ResultTypeTransform : public CachedTransform<Typed,Type> {
		public:
			ResultTypeTransform(CTRef root):CachedTransform(root) {}
			Type operate(CTRef n) {return n->Result(*this);}
			virtual Type GetArgumentType() = 0;
		};

		class ResultTypeWithConstantArgument : public ResultTypeTransform
		{
			const Type& argType;
		public:
			ResultTypeWithConstantArgument(CTRef root, const Type& fixedArgumentType):argType(fixedArgumentType),ResultTypeTransform(root) {}
			Type GetArgumentType() {return argType;}
		};

		//class ResultTypeWithArgumentGraph : public ResultTypeTransform
		//{
		//	CTRef argGraph;
		//public:
		//	ResultTypeWithArgumentGraph(CTRef root, CTRef argumentGraph):argGraph(argumentGraph),ResultTypeTransform(root) {}
		//	Type GetArgumentType() { ResultTypeWithConstantArgument rt(initial,Type::Nil); return argGraph->Result(rt); }
		//};

		class ResultTypeWithNoArgument : public ResultTypeTransform {
		public:
			ResultTypeWithNoArgument(CTRef root):ResultTypeTransform(root) {}
			Type GetArgumentType() {assert(0 && "Bad argument node found"); KRONOS_UNREACHABLE;}
		};

		class TypedTernary : public Immutable::StaticUpstreamNode<3,RegionNode<Typed>>
		{protected:TypedTernary(CTRef a, CTRef b, CTRef c) {Connect(a);Connect(b);Connect(c);}};

		class TypedBinary : public Immutable::StaticUpstreamNode<2,RegionNode<Typed>>
		{protected:TypedBinary(CTRef a, CTRef b) {Connect(a);Connect(b);}};

		class TypedUnary : public Immutable::StaticUpstreamNode<1,RegionNode<Typed>>
		{protected:TypedUnary(CTRef a) { Connect(a); } };

		class TypedLeaf : public RegionNode<Typed>  { };

		class TypedPolyadic : public Immutable::DynamicUpstreamNode<DisposableRegionNode<Typed>> { };

		class DisposableTypedTernary : public Immutable::StaticUpstreamNode<3,DisposableRegionNode<Typed>>
		{protected:DisposableTypedTernary(CTRef a, CTRef b,CTRef c) { Connect(a);Connect(b);Connect(c); } };

		class DisposableTypedBinary : public Immutable::StaticUpstreamNode<2,DisposableRegionNode<Typed>>
		{protected:DisposableTypedBinary(CTRef a, CTRef b) { Connect(a); Connect(b); } };

		class DisposableTypedUnary : public Immutable::StaticUpstreamNode<1,DisposableRegionNode<Typed>>
		{protected:DisposableTypedUnary(CTRef a) { Connect(a); } };

		class DisposableTypedLeaf : public DisposableRegionNode<Typed> {};

		class InliningTransform : public CachedTransform<const Typed, CTRef, true> {
			CTRef arg;
			bool didVisitArg = false;
		public:
			bool DidVisitArgument() const {
				return didVisitArg;
			}
			InliningTransform(CTRef root, CTRef arg) :CachedTransform(root), arg(arg) {}
			CTRef operate(CTRef src);
		};
	};
};