#pragma once
#include "common/ssstring.h"
#include "common/Enumerable.h"
#include "MemoizeTransform.h"
#include "NodeBases.h"
#include "Transform.h"
#include "EnumerableGraph.h"
#include "ReactivityGraph.h"
#include "Errors.h"
#include "Stateful.h"

#include <memory>

namespace K3 {
	namespace Nodes {
		class Boundary;
	}

	namespace Reactive{

		extern TypeDescriptor ReactiveDriver;
		extern TypeDescriptor AllDrivers;
		extern TypeDescriptor ArgumentDriver;
		extern TypeDescriptor InitializationDriver;
		extern TypeDescriptor SizingDriver;
		extern TypeDescriptor NullDriver;
		extern TypeDescriptor RecursiveDriver;

		using namespace K3::Nodes;

		using IDelegateKey = std::tuple<Graph<Typed>, CRRef>;
		using IDelegateValue = std::tuple<Graph<Typed>, CRRef>;
		using IDelegateMemoize = Transform::IMemoized<IDelegateKey,IDelegateValue>;
		using DelegateMemoize = Transform::Memoized<IDelegateKey, IDelegateValue>;
		class IDelegate : public virtual IDelegateMemoize {
		public:
			virtual int OrdinalCompare(const Type& clockId1, const Type& clockId2) const = 0;
			virtual void RegisterInput(const Type& inputId, const Type& dataType, size_t index) = 0;
			virtual void RegisterDriver(const Type& clockId, double clockMultiplier, double clockDivider) = 0;
			virtual void RegisterSignalMaskSlot(int id) = 0;
			virtual const void* CanonicalizeMaskUID(const void *uid) = 0;
			virtual const Reactive::Node* GetGlobalVariableReactivity(const void *uid) = 0;
			virtual void SetGlobalVariableReactivity(const void* uid, const Reactive::Node*) = 0;
		};

		class DriverSet : public Sml::Set<Type>, public RefCounting {
		public:
			void Merge(const IDelegate& d, const Type& driverId);
			DriverSet& operator=(const Sml::Set<Type>& rhs) {Sml::Set<Type>::operator=(rhs); return *this;}
		};

		class DriverNode : public Immutable::StaticUpstreamNode<0,DisposableRegionNode<Node>> {
			typedef Immutable::StaticUpstreamNode<0,DisposableRegionNode<Node>> _super;
			INHERIT(DriverNode,Node);
			Type DriverId;
		public:
			REGION_ALLOC(DriverNode);
			DEFAULT_LOCAL_COMPARE(_super,DriverId);
			unsigned int ComputeLocalHash() const  override
			{
				size_t h(_super::ComputeLocalHash());
				HASHER(h,DriverId.GetHash());
				return (unsigned int)h;
			}
			const char *GetLabel() const  override {return "Driver";}
			void Output(std::ostream& strm) const override;
			DriverNode(const Type& driver);
			bool IsFused() const  override {return true;}
			void *GetTypeIdentifier() const override {static const char tmp = 0; return (void*)&tmp;}
			const Type& GetID() const {return DriverId;}
			void SetID(const Type& id) { DriverId = id; }
		};

		class LazyPair : public Immutable::StaticUpstreamNode<2,RegionNode<Node>> {
			INHERIT(LazyPair,Node);
		public:
			REGION_ALLOC(LazyPair);
			LazyPair(const Node *a, const Node *b) {Connect(a);Connect(b);}
			const char *GetLabel() const  override {return "LazyPair";}
			virtual const Node* First() const override {return static_cast<const Node*>(GetCon(0));}
			virtual const Node* Rest() const override {return static_cast<const Node*>(GetCon(1));}
			bool IsFused() const override {return false;}
			void *GetTypeIdentifier() const override {static const char tmp = 0; return (void*)&tmp;}
		};

		class FusedSet : public Immutable::DynamicUpstreamNode<DisposableRegionNode<Node>> {
			INHERIT(FusedSet,Node);
		public:
			FusedSet( ) { } FusedSet(const Node* connect) { Connect(connect); }
			static FusedSet* Memoize(IDelegate&, const DriverSet&);
			REGION_ALLOC(FusedSet);
			const char *GetLabel() const override { return ""; }
			bool IsFused() const override { return true; }
			void *GetTypeIdentifier() const override { static const char tmp = 0; return (void*)&tmp; }
			void Reconnect(int idx, const Node* newCon) { DynamicUpstreamNode::Reconnect(idx,newCon); }
			void Canonicalize( );
			void Canonicalize(IDelegate& withDelegate);
		};

		typedef vector<pair<CTRef,int>> SignalMaskMapTy;

		template <typename KEY, typename VAL> class RecursiveGuard {
			const RecursiveGuard* pred;
			KEY key;
			mutable VAL val;
			RecursiveGuard(const RecursiveGuard* pred, KEY key, VAL val) :pred(pred), key(key), val(val) { }
		public:
			VAL* find(KEY k) const {
				if (k == key) return &val;
				else if (pred) return pred->find(k);
				else return nullptr;
			}

			RecursiveGuard insert(KEY k, VAL v) const {
				return RecursiveGuard(this, k, v);
			}

			static const RecursiveGuard* empty( ) {
				return nullptr;
			}
		};

		struct RecursiveClockCompletion : public std::vector<FusedSet*> {
			DriverSet LoopDriverSet;
			~RecursiveClockCompletion( ) {
				assert(empty());
			}
			void Add(FusedSet*);
			void Commit(IDelegate& d, const Node* newRx, void *tagId);
		};

		class Analysis : public CachedTransform<const Typed, CTRef, true> {
			struct dshash{size_t operator()(const DriverSet& ds) const {
				size_t h(0x1337);
				ds.for_each([&](const Type& t){HASHER(h,t.GetHash());});
				return h;
			}};

			Sml::Map<DriverSet,Graph<FusedSet>,dshash> memoizedReactivity;

			SignalMaskMapTy signalMaskIndices;

			IDelegate &del;
			const Node* leaf;
			const Node* arg;
			const Node* noReact;

			using SiblingMap = std::unordered_map<CTRef, CRRef>;
			using FunctionMap = std::unordered_map<CTRef, Graph<Typed>>;
			FunctionMap FunctionTranslation;
			SiblingMap Siblings;

			using CycleMap = RecursiveGuard<CTRef, RecursiveClockCompletion**>;
			const Node * DataflowPass(CTRef node, const CycleMap* cycles, SiblingMap & siblings);
			using CachedTransform::Go;
			std::unordered_multimap<CTRef, const Nodes::Boundary*> generatedBoundaries;
			std::list<RecursiveClockCompletion> clockCycles;
		public:
			Analysis(CTRef root, Analysis& parent, IDelegate& reactiveDelegate, const Node* argumentReactivity, const Node* leafReactivity, const Node* nullReact = new FusedSet());
			Analysis(CTRef root, IDelegate& reactiveDelegate, const Node* argumentReactivity, const Node* leafReactivity, const Node* nullReact = new FusedSet());
			int GetSignalMask(CTRef expression);
			void ReserveSignalMask(int slot);
			Graph<Typed> Process(CTRef root);
			CTRef operate(CTRef source) override;
			const Node* GetLeafReactivity() const {return leaf;}
			const Node* GetArgumentReactivity() const {return arg;}
			const Node* GetNullReactivity() const {return noReact;}
			void SetArgumentReactivity(const Node* a) {arg = a;}
			IDelegate& GetDelegate() {return del;}
			const FusedSet* Memoize(const DriverSet& set);
			
			virtual CTRef Boundary(CTRef up, const Node* downRx, const Node* upRx);
			CTRef Go(CRRef& root);
			void Rebase(CTRef b) { initial = b; }
			void AddDataflowNode(CTRef oldNode, Typed* newNode);
			Typed* GetDataflowNode(CTRef oldNode);
			void InvalidateDataflowNode(CTRef oldNode);
			const Node* ReactivityOf(CTRef node) const;
		};
	};

	class Package;
	void BuildReactivePrimitiveOps(Package pack);

	namespace Nodes {
		namespace ReactiveOperators {
			GENERIC_NODE(GenericTick,GenericUnary)
				GenericTick(CGRef identifier):GenericUnary(identifier) {}
			PUBLIC
				static GenericTick* New(CGRef id) {return new GenericTick(id);}
			END

			TYPED_NODE(Tick,DisposableTypedLeaf,IFixedResultType)
				Type Identifier;
				Tick(Type id):Identifier(id) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf,Identifier)
				unsigned ComputeLocalHash() const override { size_t h(DisposableTypedLeaf::ComputeLocalHash()); HASHER(h,Identifier.GetHash()); return (unsigned)h; }
				static Tick* New(Type id) {return new Tick(id);}
				void Output(std::ostream& strm) const override;
				Type Result(ResultTypeTransform&) const override { return Type::Float32; }
				Type FixedResult() const override { return Type::Float32; }
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			END

			GENERIC_NODE(GenericImpose,GenericBinary)
				GenericImpose(CGRef clock, CGRef signal):GenericBinary(clock,signal) {}
			PUBLIC
				static GenericImpose* New(CGRef clock, CGRef signal) {return new GenericImpose(clock,signal);}
			END

			class IRequirePartialUpstreamOnly : REFLECTING_CLASS {
				INHERIT(IRequirePartialUpstreamOnly,Reflecting)
			public:
				virtual int GetNumReactiveCons() const = 0;
			};

			TYPED_NODE(Impose,TypedBinary,IRequirePartialUpstreamOnly)
				Impose(CTRef clockSource, CTRef sig):TypedBinary(clockSource,sig) {}
			PUBLIC
				Type Result(ResultTypeTransform& arg) const override {return GetUp(1)->Result(arg);}
				static Impose* New(CTRef clock, CTRef signal) {return new Impose(clock,signal);}
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				CTRef GraphFirst( ) const override;
				CTRef GraphRest() const override;
				int GetNumReactiveCons() const override { return 1; }
			END

			GENERIC_NODE(GenericRateChange,GenericBinary)
				GenericRateChange(CGRef ratio, CGRef signal):GenericBinary(ratio,signal) {}
			PUBLIC
				static GenericRateChange* New(CGRef ratio, CGRef signal) {return new GenericRateChange(ratio,signal);}
			END

			TYPED_NODE(RateChange,TypedUnary)
				double factor;
				RateChange(double factor, CTRef sig) :TypedUnary(sig), factor(factor) { }
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedUnary,factor);
				unsigned ComputeLocalHash() const override { size_t h(TypedUnary::ComputeLocalHash()); HASHER(h,*(int64_t*)&factor); return (unsigned)h; }
				static RateChange* New(double factor, CTRef s) {return new RateChange(factor,s);}
				Type Result(ResultTypeTransform& arg) const override {return GetUp(0)->Result(arg);}
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			END

			GENERIC_NODE(GenericMerge,GenericUnary)
				GenericMerge(CGRef up):GenericUnary(up) {}
			PUBLIC
				static GenericMerge* New(CGRef up) {return new GenericMerge(up);}			
			END

			GENERIC_NODE(GenericAdjustPriority, GenericBinary)
				int relative;
				GenericAdjustPriority(CGRef sig, CGRef pFrom, int relative) : GenericBinary(sig, pFrom), relative(relative) { }
			PUBLIC
				DEFAULT_LOCAL_COMPARE(GenericBinary, relative)
				enum Opcode {
					Abdicate,
					Share,
					Cohabit,
					Supercede
				};
				static GenericAdjustPriority* New(CGRef sig, CGRef priorityFrom, int priorityDelta) { return new GenericAdjustPriority(sig, priorityFrom, priorityDelta); }
			END

			GENERIC_NODE(GenericClockEdge, GenericUnary)
				GenericClockEdge(CGRef clock) :GenericUnary(clock) {}
			PUBLIC
				static GenericClockEdge* New(CGRef clock) { return new GenericClockEdge(clock); }
			END

			TYPED_NODE(ClockEdge, TypedUnary, IFixedResultType)
				CRRef clock;
				ClockEdge(CTRef c) :clock(nullptr), TypedUnary(c) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedUnary, clock)
				CODEGEN_EMITTER
				static ClockEdge* New(CTRef c) { return new ClockEdge(c); }
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				Type Result(ResultTypeTransform& arg) const override { return Type::Float32; }
				Type FixedResult() const override { return Type::Float32; }
				void SetClock(CRRef c) { clock = c; }
				CRRef GetClock() const { return clock; }
			END

			GENERIC_NODE(GenericGate,GenericBinary)
				GenericGate(CGRef gate, CGRef signal):GenericBinary(gate,signal){}
			PUBLIC
				static GenericGate* New(CGRef gate, CGRef sig) {return new GenericGate(gate,sig);}
			END

			GENERIC_NODE(GenericRate,GenericUnary)
				GenericRate(CGRef sig):GenericUnary(sig) {}
			PUBLIC
				static GenericRate* New(CGRef sig) { return new GenericRate(sig); }
			END

			TYPED_NODE(Merge,TypedPolyadic,IFixedResultType)
				Type result;
				Merge(const Type& r):result(r) {}
				std::vector<CRRef> upRx;
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedPolyadic, result)
				Type Result(ResultTypeTransform& arg) const  override { return result; }
				Type FixedResult() const override { return result; }
				static Merge* New(const Type& r) { return new Merge(r); } 
//				virtual const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			END

			TYPED_NODE(RelativePriority, TypedBinary)
				int opcode;
				RelativePriority(CTRef sig, CTRef from, int opcode): TypedBinary(sig, from),opcode(opcode) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedBinary, opcode);
				Type Result(ResultTypeTransform& a) const override { return a(GetUp(0)); }
				static RelativePriority* New(CTRef sig, CTRef priorityFrom, int opcode) { return new RelativePriority(sig, priorityFrom, opcode); }
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
            END

			TYPED_NODE(Gate,TypedBinary)
				Gate(CTRef gate, CTRef sig):TypedBinary(gate,sig) {}
			PUBLIC
				static Gate* New(CTRef gate, CTRef sig) {return new Gate(gate,sig);}
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				Type Result(ResultTypeTransform& rt) const override { return GetUp(1)->Result(rt); }
			END

			TYPED_NODE(BaseRate,TypedUnary,IFixedResultType)				
				BaseRate(CTRef sig):TypedUnary(sig) {}
			PUBLIC
				Type Result(ResultTypeTransform&) const override { return FixedResult(); }
				Type FixedResult() const override { return Type::Float32; }
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				static BaseRate* New(CTRef sig) { return new BaseRate(sig); }
			END
		};

		TYPED_NODE(Boundary,DisposableTypedBinary)
            Boundary(CTRef up, CTRef state, const Reactive::Node* upstream, const Reactive::Node* downstream, bool canOptimizeAway)
				:DisposableTypedBinary(up,state),upstreamReactivity(upstream),canOptimizeAway(canOptimizeAway) {
				SetReactivity(downstream);
			}
			const Reactive::Node *upstreamReactivity;
			const bool canOptimizeAway;
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedBinary,upstreamReactivity,canOptimizeAway);
			void SetUpstreamReactivity(const Reactive::Node* u) { upstreamReactivity=u; }
			const Reactive::Node* GetUpstreamReactivity() const { return upstreamReactivity; }
            Type Result(ResultTypeTransform& arg) const override { return GetUp(0)->Result(arg); }
            static Boundary* New(bool canOptimizeAway,CTRef up, const Reactive::Node* upstream,const Reactive::Node* downstream, CTRef state = Typed::Nil()) { 
				return new Boundary(up,state,upstream,downstream,!canOptimizeAway); 
			}
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override { KRONOS_UNREACHABLE; }
			CTRef ReactiveReconstruct(Reactive::Analysis&) const override { KRONOS_UNREACHABLE; };
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			void Output(std::ostream&) const override;
        END

		
	};
};

namespace Qxx {
	static Enumerable<K3::GraphEnumerator<K3::Reactive::Node>> FromGraph(const K3::Reactive::Node *n) {
		return K3::GraphEnumerator<K3::Reactive::Node>(n);
	}
}
