#pragma once

#include "NodeBases.h"
#include "Stateful.h"
#include "TypeAlgebra.h"
#include "Errors.h"
#include "Native.h"

namespace K3 {
	namespace Backends {
		class SideEffectTransform;
	};

	namespace Reactive{
		class Node;
	};

	namespace Nodes{
		TYPED_NODE(SizeOfPointer,TypedLeaf,IFixedResultType)
		PUBLIC
			CODEGEN_EMITTER
			static SizeOfPointer* New() {return new SizeOfPointer;}
			Type Result(ResultTypeTransform& rt) const override {return Type::Int64;}
			Type FixedResult() const override {return Type::Int64;}
		END

		/* metadata node for combining high level expression representation with a known pointer traversal path */
		TYPED_NODE(DataSource, TypedBinary)
			DataSource(CTRef accessPath, CTRef dataLayout):TypedBinary(accessPath,dataLayout){}
		PUBLIC
			bool CanTakeReference() const;
			static DataSource* New(CTRef access, CTRef dataLayout);
			static const DataSource* New(CTRef source);
			CTRef GraphFirst() const override;
			CTRef GraphRest() const override;
			const DataSource* Reference() const;
			const DataSource* Dereference(CRRef rx) const;
			const DataSource* Dereference() const;
			const DataSource* First() const;
			const DataSource* Rest() const;
			bool IsReference() const;
			bool HasPairLayout() const;
			Type Result(ResultTypeTransform&rt) const override {return rt(GetDataLayout());}
			CTRef GetAccessor() const {return GetUp(0);}
			CTRef GetDataLayout() const {return GetUp(1);}
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			CTRef SizeOf() const;
			const DataSource* Conform(Backends::SideEffectTransform&, const DataSource* layout, const Reactive::Node* copyWhen = nullptr) const;
		END

		class IAlignmentTrackerNode : REFLECTING_CLASS {
			INHERIT(IAlignmentTrackerNode,Reflecting)
		public:
			virtual int GetAlignment() const = 0;
		};

		TYPED_NODE(SubroutineArgument,DisposableTypedLeaf,IAlignmentTrackerNode)
			Type type;
			size_t ID;
			int pointerAlignment;
			bool isOutput;
			bool isReference;
			const char *label;
			SubroutineArgument(size_t ID, const char *l, bool isOutput, const Type& t, bool isReference, int pointerAlignment)
				:ID(ID),isOutput(isOutput),type(t),isReference(isReference),label(l),pointerAlignment(pointerAlignment) {}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf,ID,isOutput,isReference,pointerAlignment,type);
			CODEGEN_EMITTER
			
			unsigned ComputeLocalHash() const override;
			static CTRef In(size_t ID, CRRef rx, CTRef fromGraph, const char *l = "in") {
				return New(true, ID, fromGraph, rx, l);
			}
			static CTRef Out(size_t ID, CTRef fromGraph, const char *l = "out") {
				return New(false, ID, fromGraph, nullptr, l);
			}
			static CTRef In(size_t ID, CRRef rx, const Type& t, const char *l = "in")
			{
				assert(t.IsNativeType());
				auto sa = new SubroutineArgument(ID+1,l,false,t,false,0);
				sa->SetReactivity(rx);
				return sa;
			}
			static CTRef New(bool isInput, size_t ID, CTRef fromGraph, CRRef rx, const char *l = nullptr);
			static SubroutineArgument* Out(size_t ID, const Type& t, const char *l = "out") {return new SubroutineArgument(ID+1,l,true,t,false,0);}
			static SubroutineArgument* In(size_t ID, CRRef rx, int align, const char *l = "in") {
				auto sa = new SubroutineArgument(ID+1,l,false,Type::Nil,true,align);
				sa->SetReactivity(rx);
				return sa;
			}
			static SubroutineArgument* Out(size_t ID, int align, const char *l = "out") {return new SubroutineArgument(ID+1,l,true,Type::Nil,true,align);}
			static SubroutineArgument* State() {return new SubroutineArgument(1,"state",false,Type::Nil,true,4);}
			static SubroutineArgument* Self() {return new SubroutineArgument(0,"self",false,Type::Nil,true,16);}
			Type Result(ResultTypeTransform&) const override {return type;}
			void Output(std::ostream& strm) const override;
			
			size_t GetID() const {return ID;}
			bool IsReference() const {return isReference;}
			bool IsOutput() const {return isOutput;}
			bool IsState() const { return ID == 1; }
			bool IsLocalState() const { return ID == -1; }
			bool IsSelf() const { return ID == 0; }
			int SchedulingPriority() const override {return -1;}
			int GetAlignment() const override { return pointerAlignment; }
			const char *GetLabel() const override {return label;}
			END

		TYPED_NODE(SubroutineMeta, TypedLeaf)
			SubroutineMeta(bool s, bool fx) :HasLocalState(s), HasSideEffects(fx) {}
		PUBLIC
			static SubroutineMeta* New(bool hasState, bool hasEffects) { return new SubroutineMeta(hasState, hasEffects); }
			const bool HasLocalState, HasSideEffects;
			Type Result(ResultTypeTransform&) const override { KRONOS_UNREACHABLE; }
			void Output(std::ostream& strm) const override { strm << (HasLocalState ? "Stateful" : "Stateless") << ", " << (HasSideEffects ? "Impure" : "Pure"); }
		END

		TYPED_NODE(Subroutine,TypedPolyadic)
			static void ConnectUpstream() {return;}
			template <typename... ARGS> void ConnectUpstream(CTRef up, ARGS... upstreamNodes) {Connect(up);ConnectUpstream(upstreamNodes...);}
			std::string label;
			Subroutine(const char *l, CTRef compiledBody, int recursion) :label(l), compiledBody(compiledBody), conditionalRecursionLoopCount(recursion) { }
			Graph<Typed> compiledBody;
			int conditionalRecursionLoopCount;
			unsigned lastArgument;
		PUBLIC
			bool HasNoLocalState = false, HasNoSideEffects = false;
			
			DEFAULT_LOCAL_COMPARE(TypedPolyadic,lastArgument,compiledBody,conditionalRecursionLoopCount);
			CODEGEN_EMITTER

			unsigned ComputeLocalHash() const override {size_t h(TypedPolyadic::ComputeLocalHash());HASHER(h,compiledBody->GetHash());HASHER(h,conditionalRecursionLoopCount);return unsigned(h);}

			template <typename... ARGS> static Subroutine* New(const char *label, CTRef compiledBody, ARGS... upstreamNodes) {
				Subroutine *s = new Subroutine(label,compiledBody,0);s->ConnectUpstream(upstreamNodes...); 
				return s;
			}

			static Subroutine* NewRecursive(const char *label, CTRef compiledBody, int loopCount) {
				auto s = new Subroutine(label,compiledBody,loopCount);
				return s;
			}

			Type Result(ResultTypeTransform&) const override {INTERNAL_ERROR("not implemented");}

			void ArgumentsConnected() {lastArgument=GetNumCons();}
			int GetLastArgument() const { return lastArgument; }
			int SchedulingPriority() const override {return conditionalRecursionLoopCount?-100:-10;}

			void Output(std::ostream& strm) const override { strm << (conditionalRecursionLoopCount ? "Recur" : "Subr") <<"<"<<label<<">";}
			CTRef GetBody() const { return compiledBody; }
			void SetBody(CTRef newBody) { compiledBody = newBody; }
			const char *GetLabel() const override {return label.c_str();}
			Subroutine* MakeMutableCopy() const { return ConstructShallowCopy(); }
			int GetLoopCount() const { return conditionalRecursionLoopCount; }
		END


		TYPED_NODE(SubroutineStateAllocation, TypedUnary)
			const Subroutine* subr;
			SubroutineStateAllocation(const Subroutine *subr, CTRef stateThread);
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedUnary, subr);
			CODEGEN_EMITTER
			static SubroutineStateAllocation* New(const Subroutine *subr, CTRef state) { return new SubroutineStateAllocation(subr, state); }
			Type Result(ResultTypeTransform&) const override { INTERNAL_ERROR("not implemented"); }
			CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const override;
			const Subroutine* GetSubroutine() { return subr; }
		END
			
		/* represents a temporarily allocated buffer */
		class Buffer : public TypedUnary, public IAlignmentTrackerNode, public virtual mu {
			const char *PrettyName() const override { return "Buffer"; }
			REGION_ALLOC(Buffer) INHERIT_RENAME(Buffer, TypedUnary, IAlignmentTrackerNode)
				PUBLIC
				enum Allocation {
				Stack,
				StackZeroed,
				Module,
				Empty
			};
		PRIVATE
			int64_t GUID;
			int alignment;
			Allocation alloc;
			Buffer(int64_t ID, CTRef sz, Allocation a, int alignment) :TypedUnary(sz), GUID(ID), alignment(alignment), alloc(a) {
			}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedUnary, alignment, GUID, alloc);
			CODEGEN_EMITTER
			unsigned ComputeLocalHash() const override { size_t h(TypedUnary::ComputeLocalHash());HASHER(h, (size_t)GUID);HASHER(h, alignment);HASHER(h, alloc);return unsigned(h); }
			static CTRef New(Backends::SideEffectTransform&, const Type& forType, Allocation);
			static CTRef New(Backends::SideEffectTransform&, size_t size, Allocation);
			static CTRef New(Backends::SideEffectTransform&, CTRef size, Allocation, size_t alignment);
			static CTRef NewEmpty();
			Type Result(ResultTypeTransform& rt) const override { INTERNAL_ERROR("Bad type query"); }

			void Output(std::ostream& strm) const override;
			int SchedulingPriority() const override { return -15; }
			Allocation GetAllocation() { return alloc; }
			int GetAlignment() const override { return alignment; }
			int64_t GetUID() const { return GUID; }
			Buffer* ConstructShallowCopy() const override;
		};

		TYPED_NODE(ReleaseBuffer,TypedBinary)
			ReleaseBuffer(CTRef buf, CTRef sz) :TypedBinary(buf, sz) {}
		PUBLIC
			CODEGEN_EMITTER
			static ReleaseBuffer* New(CTRef buffer, CTRef size) { return new ReleaseBuffer(buffer, size); }
			int SchedulingPriority() const override { return 11; }
			Type Result(ResultTypeTransform& rt) const override { INTERNAL_ERROR("Bad type query"); }
		END

		/* represents a boundary from clock regimen to another which may involve caching the signal passing through */
		TYPED_NODE(BoundaryBuffer,TypedBinary)
			BoundaryBuffer(CTRef signal, CTRef buffer, const Reactive::Node *r):TypedBinary(signal,buffer){SetReactivity(r);}
		PUBLIC
			static BoundaryBuffer* New(CTRef signal, CTRef buffer, const Reactive::Node* r) {return new BoundaryBuffer(signal,buffer,r);}
			Type Result(ResultTypeTransform& rt) const override {return rt(GetUp(0));}

			CODEGEN_EMITTER
		END


		/* represents a varying lenght tuple in function sequences */
		TYPED_NODE(VariantTuple,TypedTernary)
			int recurrenceDelta;
			VariantTuple(CTRef elementType, CTRef elementCount, CTRef tail, int recurrenceDelta):TypedTernary(elementType,elementCount,tail),recurrenceDelta(recurrenceDelta){}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedTernary,recurrenceDelta);
			static VariantTuple* New(CTRef element, CTRef count, CTRef tail, int delta) {
				return new VariantTuple(element,count,tail,delta);
			}
			Type Result(ResultTypeTransform&) const override {INTERNAL_ERROR("not applicable");}
			CTRef GraphFirst() const override;
			CTRef GraphRest() const override;
			int GetRecurrenceDelta() const { return recurrenceDelta; }
		END

		/* represents an offset sub-pointer */
		TYPED_NODE(Offset,TypedBinary, IAlignmentTrackerNode)
			Offset(CTRef buf, CTRef offset):TypedBinary(buf,offset){}
		PUBLIC
			CODEGEN_EMITTER
			static CTRef New(CTRef buffer, CTRef offset);
			Type Result(ResultTypeTransform&) const override { INTERNAL_ERROR("not implemented"); }
			int GetAlignment() const override;
			int SchedulingPriority() const override { return 5; }
		END

		/* represents a referencing operation.
			also appears in data layout metadata to denote data by reference
		*/
		TYPED_NODE(Reference, TypedUnary, IAlignmentTrackerNode)
			int alignment;
			Reference(int a, CTRef up):TypedUnary(up),alignment(a) {}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedUnary, alignment);
			CODEGEN_EMITTER
			Type Result(ResultTypeTransform& rt) const override {return rt(GetUp(0));}
			static CTRef New(CTRef up);

			int GetAlignment() const override { return alignment; }
		END

		/* represents a dereferencing operation. */
		TYPED_NODE(Dereference,DisposableTypedUnary)
			Type loadType;
			int alignment;
			bool loadPtr;
			Dereference(int a, CTRef up, const Type& loadType, bool isPtr):DisposableTypedUnary(up),loadType(loadType),loadPtr(isPtr),alignment(a) {}
//			static CTRef New(CTRef up, const Type& loadType, bool isPtr);
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedUnary,loadType,loadPtr,alignment);
			CODEGEN_EMITTER
			unsigned ComputeLocalHash() const override {size_t h(DisposableTypedUnary::ComputeLocalHash());HASHER(h,loadType.GetHash());HASHER(h,loadPtr?1:0);return unsigned(h);}
			Type Result(ResultTypeTransform& rt) const override { return loadType.IsNil() ? rt(GetUp(0)) : loadType; }
			static CTRef New(CTRef up); 
			static CTRef New(CTRef up, const Type& load);
			static CTRef New(CTRef up, CRRef rx);
			static CTRef New(CTRef up, CRRef rx, const Type& load);

			void Output(std::ostream& strm) const override;
		END

		TYPED_NODE(SequenceCounter,TypedLeaf,IFixedResultType)
			int64_t counter_offset;
			SequenceCounter(int64_t c):counter_offset(c) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedLeaf,counter_offset)
			CODEGEN_EMITTER
			static SequenceCounter* New(int64_t offset) {return new SequenceCounter(offset);}
			Type Result(ResultTypeTransform&) const override {return Type::Int64;}
			Type FixedResult() const override {return Type::Int64;}
			int64_t GetCounterOffset() const { return counter_offset; }
			int SchedulingPriority() const override { return 5; }
		END
	
		TYPED_NODE(Copy,TypedPolyadic)
		PUBLIC
			enum Mode{
				MemCpy,
				Store
			};
		PRIVATE
			Mode mode;
			int dstAlign = 4, srcAlign = 4;
			bool mutatesState;
			bool doesInit;
			Copy(CTRef dst, CTRef src, CTRef size, CTRef repeat, Mode m, const Reactive::Node *r, int sa, int da, bool mutate, bool init)
				:mode(m), mutatesState(mutate),srcAlign(sa),dstAlign(da),doesInit(init) {
				Connect(dst); Connect(src); Connect(size); Connect(repeat);
				SetReactivity(r);
			}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedPolyadic,mode,mutatesState,doesInit);
			CODEGEN_EMITTER
			static Copy* New(CTRef dst, CTRef src, CTRef sizeInBytes, Mode mode, const Reactive::Node* reactivity, CTRef repeat, bool mutateState, bool doesInitialization) {
				int sa = 0, da = 0;
				IAlignmentTrackerNode *at;
				if (src->Cast(at)) sa = at->GetAlignment();
				if (dst->Cast(at)) da = at->GetAlignment();
				return new Copy(dst,src,sizeInBytes,repeat,mode,reactivity,sa,da,mutateState,doesInitialization); 
			}
			static Copy* New(CTRef dst, CTRef src, CTRef sizeInBytes, Mode mode, const Reactive::Node* reactivity, size_t repeat, bool mutateState, bool doesInitialization) {
				return New(dst, src, sizeInBytes, mode, reactivity, Native::Constant::New((int32_t)repeat), mutateState, doesInitialization);
			}

			Type Result(ResultTypeTransform&) const override { return Type::Nil; }

			void Output(std::ostream&) const override;

			bool DoesMutateState() const { return mutatesState; }
			bool DoesInitialization() const { return doesInit; }
			void SetAlignment(int src, int dst) { srcAlign = src; dstAlign = dst; }
			int SchedulingPriority() const override { return 5; }
		END

		TYPED_NODE(OnInit, TypedBinary)
			OnInit(CTRef init, CTRef stream) :TypedBinary(init, stream) {}
		PUBLIC
			static OnInit* New(CTRef onInit, CTRef onStream) { return new OnInit(onInit, onStream); }
			Type Result(ResultTypeTransform& rt) const override { return GetUp(0)->Result(rt); }
			CODEGEN_EMITTER
		END

		TYPED_NODE(Configuration, DisposableTypedLeaf, IFixedResultType)
			int slotIndex;
			Type ty;
			Configuration(Type t, int si) :slotIndex(si), ty(t) {}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf, slotIndex, ty);
			static Configuration* New(Type ty, int slotIndex) { return new Configuration{ ty, slotIndex }; }
			Type Result(ResultTypeTransform&) const override { return ty; }
			Type FixedResult() const override { return ty; }
			int GetSlotIndex() const { return slotIndex; }
			CODEGEN_EMITTER
		END

		TYPED_NODE(DerivedConfiguration, TypedLeaf)
			CTRef cfg;
			DerivedConfiguration(CTRef up) : cfg(up) { 
				cfg->HasInvisibleConnections();
			}
		PUBLIC
			static DerivedConfiguration* New(CTRef up) { 
				return new DerivedConfiguration(up); 
			}
			Type Result(ResultTypeTransform& rt) const override { return cfg->Result(rt); }
			CODEGEN_EMITTER
		END

		TYPED_NODE(Deps,TypedPolyadic)
			void ConnectUpstream() {};
			template <typename... ARGS> void ConnectUpstream(CTRef up, ARGS... upstream) {
				Connect(up);ConnectUpstream(upstream...);
			}
			bool isDataProtector;
			Deps(bool dp = false):isDataProtector(dp) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedPolyadic,isDataProtector);
			CODEGEN_EMITTER
			void Connect(CTRef upstream);
			static Deps* New(bool dataProtector = false) {
				return new Deps(dataProtector);
			}
			static CTRef Transfer(CTRef upstream, const Deps *dependencies);

			template <typename... ARGS> static CTRef New(CTRef initial, ARGS... upstream) {
				DataSource *ds;
				if (initial->Cast(ds)) return DataSource::New(New(ds->GetAccessor(),upstream...),ds->GetDataLayout());
				Pair *p;
				if (initial->Cast(p)) {
					auto np = const_cast<Typed*>(Pair::New(New(p->GetUp(0), upstream...), 
														   New(p->GetUp(1), upstream...)));
					np->SetReactivity(initial->GetReactivity( ));
					return np;
				}
				auto m(new Deps);
				m->ConnectUpstream(initial,upstream...);
				m->SetReactivity(initial->GetReactivity( ));
				return m;
			}

			/* if first argument is mutable, safe to assume returned node is mutable */
			template <typename... ARGS> static Typed* New(Typed *initial, ARGS... upstream) {
				return const_cast<Typed*>(New(const_cast<CTRef>(initial),upstream...));
			}

			Type Result(ResultTypeTransform&t) const override {assert(GetNumCons() && "Incomplete Deps");return GetUp(0)->Result(t);}
			CTRef GraphFirst() const override;
			CTRef GraphRest() const override;

			void CopyElision(Backends::CopyElisionTransform&) const override;
			virtual const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			virtual CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			CTRef SideEffects(Backends::SideEffectTransform&) const override;
			int SchedulingPriority() const override {return 5;}
			CTRef IdentityTransform(GraphTransform<const Typed,CTRef>& transform) const override;
			bool IsDataProtector() const { return isDataProtector; }
			void SetDataProtector(bool f) { isDataProtector = f; }
		END

		class FunctionSequence;
		TYPED_NODE(RecursionBranch,DisposableTypedBinary)
			const FunctionSequence *parentSeq;
			int32_t* sequenceLength;
			CTRef SequenceArgument;
			CTRef SequenceResult;

			Graph<Typed> tailContinuation;
			Graph<Generic> closedArgument;
			Graph<Generic> closedResult;
			const Reactive::Node *argumentReactivity;
			RecursionBranch(CTRef counter, CTRef up, int32_t* seq, Graph<Typed> t, Graph<Generic> a, Graph<Generic> r, CTRef calleeArgs, CTRef calleeRes, const FunctionSequence* parent);
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedBinary,parentSeq);
			static RecursionBranch* New(CTRef counter, CTRef upstream, int32_t* sequenceLength, Graph<Typed> tail,
			                            Graph<Generic> argumentFormula,
										Graph<Generic> resultFormula,
										CTRef CalleeParamGraph,
										CTRef CalleeResultGraph,
										const FunctionSequence* parent) 
			{ 
				return new RecursionBranch(counter,upstream,sequenceLength,tail,argumentFormula,resultFormula,CalleeParamGraph,CalleeResultGraph,parent); 
			}
			Type Result(ResultTypeTransform&) const override {KRONOS_UNREACHABLE;}

			void CopyElision(Backends::CopyElisionTransform&) const override;
//			Backends::LLVMValue Compile(Backends::LLVMTransform&) const;
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			size_t GetSequenceLength() const { return *sequenceLength; }
		END

		TYPED_NODE(TableIndex,TypedBinary)
			TableIndex(CTRef table, CTRef index):TypedBinary(table,index) {}
		PUBLIC
			CODEGEN_EMITTER
			static TableIndex* New(CTRef table, CTRef index) {return new TableIndex(table,index);}
			Type Result(ResultTypeTransform& rt) const override {auto buf(rt(GetUp(0)));return buf.IsPair()?buf.First():buf;}
		END

		TYPED_NODE(SignalMaskSetter,TypedUnary)
			unsigned bitIdx;
			SignalMaskSetter(CTRef maskState, unsigned bitIdx):TypedUnary(maskState),bitIdx(bitIdx) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(TypedUnary,bitIdx);
			CODEGEN_EMITTER
			static SignalMaskSetter* New(CTRef maskState, unsigned bit) { return new SignalMaskSetter(maskState,bit); }
			Type Result(ResultTypeTransform& rt) const override { assert(0 && "Invalid type query in the backend (SignalMaskSetter)"); return Type::Nil; }
		END

		TYPED_NODE(CStringLiteral, DisposableTypedLeaf)
			Type str;
			CStringLiteral(const Type& t) :str(t) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf, str);
			CODEGEN_EMITTER
			static CStringLiteral* New(const Type& str) { return new CStringLiteral(str); }
			Type Result(ResultTypeTransform& rt) const override { assert(0 && "Invalid type query in the backend (SignalMaskSetter)"); return Type::Nil; }
		END
	};
};
