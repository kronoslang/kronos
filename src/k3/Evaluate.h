#pragma once
#include "NodeBases.h"
#include "Graph.h"
#include "Stateful.h"

namespace K3 {
	namespace Nodes{

		GENERIC_NODE(GenericArgument,GenericLeaf)
		PUBLIC
			static GenericArgument* New() {return new GenericArgument;}
		END

		TYPED_NODE(Argument,TypedLeaf)
		PUBLIC
			static Argument* New() {return new Argument();}
			Type Result(ResultTypeTransform& withArgument) const override {return withArgument.GetArgumentType();}
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			CTRef SideEffects(Backends::SideEffectTransform&) const override;
			void CopyElision(Backends::CopyElisionTransform&) const override;
			END

		GENERIC_NODE(Evaluate, GenericBinary)
			char label[32];
			Evaluate(const char *l, CGRef graph, CGRef arg, const char *le = nullptr);
			Specialization SpecializeRecursive(SpecializationState& t,const Specialization& argument,const Qxx::IEnumerable<const Type&>& Forms,const std::vector<Nodes::CGRef >* recurPt) const;
			Specialization SpecializeCore(SpecializationTransform&) const;
		PUBLIC
			std::pair<Graph<Typed>, Type> SpecializeBody(SpecializationState& t, const std::vector<Nodes::CGRef >* recurPt, const Qxx::IEnumerable<const Type&>& Forms, const Specialization& A1, bool& outUsedFallback) const;
			const char *GetLabel() const override {return label;}
			static Evaluate* New(const char *label, CGRef graph, CGRef arg, const char *label_end = nullptr);
			static Evaluate* CallLib(const char *qn, CGRef arg);
		END

		class FunctionBase : public DisposableTypedUnary, public IFixedResultType {
			INHERIT(FunctionBase,DisposableTypedUnary,IFixedResultType);
		protected:
			const Reactive::Node* argumentReactivity;
			int LocalCompare(const ImmutableNode& rhs) const override;
		public:
			void SetArgumentReactivity(const Reactive::Node* argReact) { argumentReactivity = argReact; }
			FunctionBase(CTRef arg):DisposableTypedUnary(arg) { }
			virtual Type ArgumentType() const = 0;
			virtual CTRef GetArgumentNode() const { return GetUp(0); };
			CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			int GetWeight() const  override { return 5; }
		};

		TYPED_NODE(FunctionCall,FunctionBase)
			Graph<Typed> body;
			std::string label;
			FunctionCall(const char *l, Graph<Typed> body, const Type& argument, const Type& result, CTRef arg);
			int LocalCompare(const ImmutableNode& rhs) const override;
			unsigned ComputeLocalHash() const override;
			Type argumentType, resultType;
		PUBLIC
			FunctionCall *MakeMutableCopy() { return ConstructShallowCopy(); }
			static CTRef New(const char *l, Graph<Typed> body, const Type& argument, const Type& result, CTRef arg) {
				if (result.GetSize() < 1) return Typed::Nil();
				return new FunctionCall(l,body,argument.Fix(Type::GenerateRulesForSizedTypes),result.Fix(Type::GenerateRulesForSizedTypes),arg);
			}
			const char *GetLabel() const override { return label.c_str(); }
			Type Result(ResultTypeTransform& withArg) const override {return resultType;}
			Type FixedResult() const override { return resultType; }
			Type ArgumentType() const override { return argumentType; }
			void CopyElision(Backends::CopyElisionTransform&) const override;
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			void Output(std::ostream& strm) const override { strm << "Call<" << label << ">"; }
			CTRef GetBody() const { return body; }
			void SetBody(CTRef b) { body = b; }
			void SetResultType(const Type& rty) { resultType = rty; }
			CTRef IdentityTransform(GraphTransform<const Typed, CTRef> &copy) const override;
		END

		TYPED_NODE(FunctionSequence,FunctionBase)
			friend class RecursionBranch;
			Graph<Typed> iterator;
			Graph<Typed> generator;
			Graph<Generic> closedArgument;
			Graph<Generic> closedResult;
			Graph<Typed> typedArgument;
			Graph<Typed> typedResult;
			Graph<Typed> tailContinuation;
			size_t num;
			const char *label;
			unsigned ComputeLocalHash() const override;
			FunctionSequence(const char* l,
							 CGRef argumentFormula, CGRef resultFormula,
							 CTRef iterator, CTRef generator, 
							 CTRef tailCall, size_t repeatCount, 
							 CTRef arg);
		PUBLIC
			FunctionSequence* MakeMutableCopy() {return ConstructShallowCopy();}
			const char *GetLabel() const  override {return label;}
			CTRef GetIterator() const {return iterator;}
			CTRef GetGenerator() const {return generator;}
			void SetIterator(CTRef i) {iterator = i;}
			void SetGenerator(CTRef g) {generator = g;}
			CGRef GetArgumentFormula() const {return closedArgument;}
			CGRef GetResultFormula() const {return closedResult;}
			CTRef GetTailCall() const {return tailContinuation;}
			void SetTailCall(CTRef t) {tailContinuation = t;}
			size_t GetRepeatCount() const {return num;}
			void SetRepeatCount(size_t count) { num = count; }
			DEFAULT_LOCAL_COMPARE(FunctionBase,iterator,generator,closedArgument,closedResult,tailContinuation,num);
			static FunctionSequence* New(const char* l,
										 CGRef argumentFormula, CGRef resultFormula,
										 CTRef iterator, CTRef generator, 
										 CTRef tailCall, size_t repeatCount, 
										 CTRef arg, CTRef state = Typed::Nil())
			{
				return new FunctionSequence(l,argumentFormula,resultFormula,iterator,generator,tailCall,repeatCount,arg);
			};

			static CTRef NewWithCodeMotion(const char *l,
				CGRef argumentFormula, CGRef resultFormula,
				CTRef iterator, CTRef generator,
				CTRef tailCall, size_t repeatCount,
				CTRef arg);

			//Reactivity ReactiveAnalysis(Reactive::Analysis&) const;
			Type Result(ResultTypeTransform& withArg) const override;
			Type ArgumentType() const override {return SpecializationTransform::Infer(closedArgument,Type::InvariantLD(0));}

			Type FixedResult() const override { return SpecializationTransform::Infer(closedResult,Type::InvariantU64(num)); }

			void CopyElision(Backends::CopyElisionTransform&) const override;
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			void Output(std::ostream& strm) const override { strm << "Seq<" << label << ">"; }

			FunctionBase *RemoveIterationsFront(unsigned count) const;
			void SplitSequenceAt(unsigned count);
		END
	};
};