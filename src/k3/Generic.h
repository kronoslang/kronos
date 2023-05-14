#pragma once
#include "common/Reflect.h"
#include "RegionNode.h"
#include "Transform.h"
#include "Type.h"
#include "Graph.h"

#define GENERIC_NODE_INLINE(CLASS, ...) \
class CLASS : META_MAP(CXX_INHERIT_PUBLIC,__VA_ARGS__) mu {\
REGION_ALLOC(CLASS) INHERIT_RENAME(CLASS, __VA_ARGS__) CLASS* ConstructShallowCopy() const override {return new CLASS(*this);}

#define GENERIC_NODE(CLASS, ...) \
GENERIC_NODE_INLINE(CLASS,__VA_ARGS__)\
const char *PrettyName() const override {return #CLASS;}\
public: Specialization Specialize(SpecializationState &t) const override; private:
#define SPECIALIZE(TRANSFORM,VAR,SOURCENODE) auto VAR = TRANSFORM(SOURCENODE); if (VAR.node == nullptr) return VAR;

#define SPECIALIZE_ARGS_BUILDER(SOURCE) SPECIALIZE(_t,A##SOURCE,GetUp(SOURCE));
#define SPECIALIZE_ARGS(TRANSFORM,...) auto& _t(TRANSFORM); META_MAP(SPECIALIZE_ARGS_BUILDER,__VA_ARGS__)

#define PUBLIC public:
#define PRIVATE private:
#define PROTECTED protected:

//namespace std{class stringstream;};

namespace K3 {
	namespace Nodes{
		class Typed;
		typedef const Typed* CTRef;

		struct Specialization{
			Specialization():result(false),node(nullptr) {}
			Specialization(CTRef n, const Type &t) :node(n), result(t) { }
			Specialization(CTRef n, Type&& t) :node(n), result(std::move(t)) { }
			
			CTRef node;
			Type result;
		};

		class SpecializationTransform;
		typedef SpecializationTransform SpecializationState;

		/* Generic graph node that represents a node of the user program AST */
		class GenericBase : public RegionNodeBase {
			friend class SpecializationTransform;
			const char* pos;
		protected:
			virtual Specialization Specialize(SpecializationState &t) const = 0;
			GenericBase();
		public:
			static bool VerifyAllocation(MemoryRegion* region, CGRef node);
			const char* GetRepositoryAddress() const { return pos; }
		};

		class Generic : public CachedTransformNode<GenericBase>, REFLECTING_CLASS{
		protected:
			static Specialization TypeError(TypeDescriptor *desc);
			static Specialization TypeError(TypeDescriptor *desc, const Type& content);
			static Specialization SpecializationFailure();
			virtual void *GetTypeIdentifier() const {return (void*)TypeID();}
			CGRef& _GetUp(unsigned int idx) {assert(idx<GetNumCons());return (CGRef&)upstream[idx];}
		public:
			virtual const char *GetLabel()const {return PrettyName();}
			CGRef GetUp(unsigned int idx) const {return (CGRef)GetCon(idx);}
			CGRef Reconnect(unsigned int idx, CGRef newCon) {_GetUp(idx)=newCon;newCon->globalDownstreamCount++;return newCon;}
			void Connect(const Generic *up) {up->globalDownstreamCount++;CachedTransformNode<GenericBase>::Connect(up);}
			unsigned ComputeLocalHash() const { auto h(GenericBase::ComputeLocalHash());HASHER(h,(uintptr_t)TypeID()); return h;}
			virtual CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& copyTable) const;
			virtual Generic* ConstructShallowCopy() const = 0;
			const CGRef* GetConnectionArray() const {return (const CGRef*)upstream;}
			class UpstreamCollection {
				const CGRef *beg;
				const CGRef *e;
			public:
				UpstreamCollection(const CGRef* b, const CGRef* e):beg(b),e(e){}
				const CGRef* begin() const {return beg;}
				const CGRef* end() const {return e;}
			};

			UpstreamCollection Upstream() const {return UpstreamCollection(GetConnectionArray(),GetConnectionArray()+GetNumCons());}
		};

		typedef const Generic* CGRef;
		 
		/* A transform that performs specialization of generic graphs into type-inferred ones */		
		enum Verbosity{
			LogEverything,
			LogTrace,
			LogWarnings,
			LogErrors,
			LogAlways
		};

		class SpecializationDiagnostic {
			std::ostream *report;
			int indent;
			class DiagnosticBlock{
				SpecializationDiagnostic &diag;
				const char *b;
			public:
				DiagnosticBlock(SpecializationDiagnostic &d,const char *block, const char *attr);
				~DiagnosticBlock();				
			};
			void DoIndent(); 
		public:

			SpecializationDiagnostic(std::ostream *r,Verbosity verbosity = LogTrace, int tabSize = 1);
			~SpecializationDiagnostic();

			DiagnosticBlock Block(Verbosity loglevel, const char *b, const char *fmt = 0,...);// {return DiagnosticBlock(*this,b);}

			void Diagnostic(Verbosity loglevel, CGRef source, int code, const Type& received, const Type& expected, const char *fmt, ...);
			void Diagnostic(Verbosity loglevel, CGRef source, int code, const Type& received, const char *fmt, ...);
			void Diagnostic(Verbosity loglevel, CGRef source, int code, const char *fmt, ...);
			void SuccessForm(Verbosity loglevel, const char *funcname, const Type& argument, const Type& result);
			Specialization TypeError(const char *node, const Type& arg, const Type& nested = Type::Nil);
			bool IsActive( ) const {
				return report != nullptr;
			}

		private:
			Verbosity verbosity;
		};

		class SpecializationTransform : public CachedTransform<Generic,Specialization> {
			Type argumentType;
			SpecializationDiagnostic rep;
		protected:
			Specialization operate(CGRef src) {return src->Specialize(*this);} 
		public:
			const enum Mode {
				Normal,
				Configuration
			} mode;

			friend class GenericArgument;
			
			SpecializationTransform(CGRef root, Type argument, SpecializationDiagnostic diags, Mode m) 
				:CachedTransform(root) 
				,argumentType(std::move(argument))
				,rep(diags)
				,mode(m) { }

			SpecializationDiagnostic& GetRep() {return rep;}

			static Type Infer(CGRef root, const Type& argument);
			
			static std::pair<Type, Graph<Typed>> Process(CGRef root, const Type& argument, SpecializationState::Mode m);

			const Type& GetArgumentType() const {
				return argumentType;
			}
		};

		class DisposableGenericTernary : public Immutable::StaticUpstreamNode<2,DisposableRegionNode<Generic>>
		{protected:DisposableGenericTernary(CGRef a, CGRef b, CGRef c) {Connect(a);Connect(b);Connect(c);} };

		class DisposableGenericBinary : public Immutable::StaticUpstreamNode<2,DisposableRegionNode<Generic>>
		{protected:DisposableGenericBinary(CGRef a, CGRef b) {Connect(a);Connect(b);} };

		class DisposableGenericUnary : public Immutable::StaticUpstreamNode<1,DisposableRegionNode<Generic>>
		{protected:DisposableGenericUnary(CGRef a) {Connect(a);} };

		class DisposableGenericLeaf : public DisposableRegionNode<Generic> {};

		class GenericTernary : public Immutable::StaticUpstreamNode<3,RegionNode<Generic>>
		{protected:GenericTernary(CGRef a, CGRef b, CGRef c) {Connect(a);Connect(b);Connect(c);} };

		class GenericBinary : public Immutable::StaticUpstreamNode<2, RegionNode<Generic>>
		{protected:GenericBinary(CGRef a, CGRef b) {Connect(a);Connect(b);} };

		class GenericUnary : public Immutable::StaticUpstreamNode<1, RegionNode<Generic>>
		{protected:GenericUnary(CGRef a) {Connect(a);} };

		class GenericLeaf : public DisposableRegionNode<Generic> { };

		class GenericPolyadic : public Immutable::DynamicUpstreamNode<DisposableRegionNode<Generic>> {};

		class IInversible : REFLECTING_CLASS
		{
			INHERIT(IInversible,Reflecting)
		public:
			virtual bool InverseFunction(int forBranch, const Type& down, Type& out, 
										 SpecializationTransform& forwardTransform) const = 0;
		};
	};

	namespace Transform{
		template <class NODETYPE>
		class Identity : public CachedTransform<NODETYPE,NODETYPE*>{
		public:
			Identity(const NODETYPE *root):CachedTransform<NODETYPE,NODETYPE*>(root){}
			virtual NODETYPE* operate(const NODETYPE* src)
			{
				return src->IdentityTransform(*this);
			}
		};

		template <class TYPE> class GraphCopyDictionary : public PartialTransform<Identity<TYPE>> 
		{
		public:
			GraphCopyDictionary(const TYPE* root) : PartialTransform<Identity<TYPE>> (root) { }
		};
	};
};
