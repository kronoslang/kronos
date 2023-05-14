#pragma once
#include "NodeBases.h"

namespace K3 {
	namespace Nodes{
		GENERIC_NODE(GenericPair,GenericBinary,IInversible)
			GenericPair(CGRef lhs, CGRef rhs);
			public:static GenericPair* New(CGRef lhs, CGRef rhs);
		    static GenericPair NewTemporary(CGRef lhs, CGRef rhs) {return GenericPair(lhs,rhs);}
			bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
			void Output(std::ostream& stream) const override;
		END

		GENERIC_NODE(GenericFirst,GenericUnary,IInversible)
			GenericFirst(CGRef up);
			public:static CGRef New(CGRef up);
			bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
			CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& copyTable) const override {return New(copyTable(GetUp(0)));}
			void Output(std::ostream& stream) const override;
		END

		GENERIC_NODE(GenericRest,GenericUnary,IInversible)
			GenericRest(CGRef up);
			public:static CGRef New(CGRef up);
			bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
			CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& copyTable) const  override {return New(copyTable(GetUp(0)));}
			void Output(std::ostream& stream) const override;
		END

		GENERIC_NODE(GenericTypeTag,GenericLeaf)
			GenericTypeTag(const std::string& fullPath);
			GenericTypeTag(TypeDescriptor *desc);
			TypeDescriptor *typeDesc;
		public:
			static CGRef New(const std::string& fullPath) {return new GenericTypeTag(fullPath);}
			static CGRef New(TypeDescriptor *fromDesc) {return new GenericTypeTag(fromDesc);}
		END

		GENERIC_NODE(GenericMake,GenericBinary,IInversible)
			GenericMake(CGRef typetag, CGRef content, bool ai):GenericBinary(typetag,content),allowInternalType(ai) {}
			DEFAULT_LOCAL_COMPARE(GenericBinary,allowInternalType);
			const bool allowInternalType;
		PUBLIC
			static Generic* New(CGRef tag, CGRef content, bool allowInternalType = false) {return new GenericMake(tag,content,allowInternalType);}
			bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
		END

		GENERIC_NODE(GenericBreak,GenericBinary,IInversible)
			GenericBreak(CGRef typetag, CGRef content,bool ai):GenericBinary(typetag,content),allowInternalType(ai){}
			DEFAULT_LOCAL_COMPARE(GenericBinary,allowInternalType);
			const bool allowInternalType;
		PUBLIC
			static Generic* New(CGRef tag, CGRef content, bool allowInternalType = false) {return new GenericBreak(tag,content,allowInternalType);}
			bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
		END


		class PairSimplify {
		public:
			static CTRef MakeFullPair(CTRef, CTRef);
		};

		TYPED_NODE(Pair, TypedBinary)
#ifndef NDEBUG
			static const bool PairIdentifier = true;
#endif
			friend class PairSimplify;
		Pair(CTRef lhs, CTRef rhs);
			public:static CTRef New(CTRef lhs, CTRef rhs);
			Type Result(ResultTypeTransform& withArgument) const override;
			// non-default reactivity
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			void CopyElision(Backends::CopyElisionTransform&) const override;
			CTRef SideEffects(Backends::SideEffectTransform&) const override;
			// first-chance graph simplification
			CTRef GraphFirst() const override;
			CTRef GraphRest() const override;
			CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& copyTable) const override;
		END

		class IPairSimplifyFirst : public PairSimplify, REFLECTING_CLASS {
			INHERIT(IPairSimplifyFirst, Reflecting)
		public:
			virtual CTRef PairWithRest(CTRef) const = 0;
		};

		class IPairSimplifyRest : public PairSimplify, REFLECTING_CLASS {
			INHERIT(IPairSimplifyRest, Reflecting)
		public:
			virtual CTRef PairWithFirst(CTRef) const = 0;
		};

		TYPED_NODE(First,TypedUnary,IPairSimplifyFirst)
			friend class Typed;
			First(CTRef up);
			public:static CTRef New(CTRef up);
			 Type Result(ResultTypeTransform& withArgument) const override;
			// non-default reactivity
			 const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			 CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			 void CopyElision(Backends::CopyElisionTransform&) const override;
			CTRef SideEffects(Backends::SideEffectTransform&) const override;
			CTRef IdentityTransform(GraphTransform<const Typed,CTRef>& st) const override {return New(st(GetUp(0)));}
			CTRef PairWithRest(CTRef) const override;
			virtual int GetWeight() const override { return 1; }
		END

		TYPED_NODE(Rest,TypedUnary,IPairSimplifyRest)
			friend class Typed;
			Rest(CTRef up);
			public:static CTRef New(CTRef up);
			Type Result(ResultTypeTransform& withArgument) const override;
			// non-default reactivity
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			void CopyElision(Backends::CopyElisionTransform&) const override;
			CTRef SideEffects(Backends::SideEffectTransform&) const override;
			CTRef IdentityTransform(GraphTransform<const Typed,CTRef>& st) const override {return New(st(GetUp(0)));}
			CTRef PairWithFirst(CTRef) const override;
			virtual int GetWeight() const override { return 1; }
		END
	};
};