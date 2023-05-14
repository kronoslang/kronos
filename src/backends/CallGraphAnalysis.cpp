#include "CallGraphAnalysis.h"
#include "Evaluate.h"
#include "FlowControl.h"

#include <iostream>

namespace K3 {
	namespace Backends{
		CallGraphNode::CallGraphNode(const Nodes::Subroutine* associatedSubroutine):subr(associatedSubroutine) { }

		void CallGraphNode::Output(std::ostream& stream) const
		{
			static unsigned _indent = 0;
			stream << std::string(_indent,' ') << (subr?subr->GetLabel():"root") << "[" << subr << "] :";
			for(auto id : ActiveStatesIn) stream << " " << id;
			stream << "\n";
			_indent++;
			for(unsigned i(0);i<GetNumCons();++i) GetUp(i)->Output(stream);
			_indent--;
		}

		CallGraphNode *AnalyzeCallGraph(const Nodes::Subroutine* subr,CTRef graph, CallGraphMap& cgmap)
		{
			CallGraphNode *current(new CallGraphNode(subr));
			bool staticallyActive = false;
			for(auto node : Qxx::FromGraph(graph))
			{
				Nodes::Subroutine* sub;
				if (node->Cast(sub))
				{
					auto up = cgmap[sub] = AnalyzeCallGraph(sub,sub->GetBody(),cgmap);
					current->Connect(up);

					if (!staticallyActive) {
						for (auto sst : up->GetActiveStates()) current->AddActiveState(sst);
					}
				}
				else if (!staticallyActive)
				{
					if (node->GetReactivity()) {
						for (auto rn : Qxx::FromGraph(node->GetReactivity()).OfType<Reactive::DriverNode>())
							current->AddActiveState(rn->GetID());
					} else {
						staticallyActive = true;
						current->SetStaticallyActive();
					}

					Nodes::MultiDispatch *md;
					if (node->Cast(md)) {
						for (auto &d : md->GetDispatchees()) {
							if (d.first) AnalyzeCallGraph(d.first, d.first->GetBody(), cgmap);
						}
					}
				}
			}
			return current;
		}
	};

	//class ModifyGraph : public Transform::GraphCopyDictionary<const Nodes::Typed> {
	//public:
	//	ModifyGraph(CTRef root):GraphCopyDictionary(root) { }

	//	Typed* FinalizeSpecifiedNode(const Typed *src) {
	//		if (IsOfCompatibleType<FunctionBase>(src))
	//		for (int i(0); i < src->GetNumCons(); ++i) {
	//			src->Reconnect(i, const_cast<Nodes::Typed*>((*this)(src->GetUp(i))));
	//		}
	//		return src;
	//	}
	//};

	static const int inlineTreshold = 7;

	class SubgraphSimplify : public CachedTransform <const Typed, CTRef, true> {
		CallGraphSimplify& cgs;
		int myWeight = 0;
	public:
		SubgraphSimplify(CallGraphSimplify& c, CTRef root) :CachedTransform(root), cgs(c) { }
		CTRef operate(CTRef node) {
			Nodes::FunctionSequence *fseq;
			Nodes::FunctionCall *fc;
			Nodes::Switch *sw;
			int weight(0);
			if (node->Cast(fseq)) {
				myWeight += 2;
				auto newIter = cgs(fseq->GetIterator(), weight);
				auto newGene = cgs(fseq->GetGenerator(), weight);
//				std::cout << "\n[" << fseq->GetLabel() << "]\n" << *fseq->GetIterator() << "\n" << *newIter<< "\n" << *fseq->GetGenerator() << "\n" <<*newGene;
				auto newSeq = fseq->MakeMutableCopy();
				newSeq->SetIterator(newIter);
				newSeq->SetGenerator(newGene);
				for (unsigned i(0); i < newSeq->GetNumCons(); ++i) {
					newSeq->Reconnect(i, (*this)(newSeq->GetUp(i)));
				}
				return newSeq;
			} else if (node->Cast(fc)) {
				auto newBody = cgs(fc->GetBody(), weight);
//				std::cout << "\n[" << fc->GetLabel() << "]\n" << * fc->GetBody() << "\n" << *newBody << "\n";
				auto newFc = FunctionCall::New(
					fc->GetLabel(),
					newBody,
					fc->ArgumentType(),
					fc->FixedResult(),
					(*this)(fc->GetUp(0)));
				if (weight > inlineTreshold) {
					myWeight += 2;
					return newFc;
				} else if (newFc->Cast(fc)) {
					myWeight += weight;
					return InliningTransform(fc->GetBody(), fc->GetUp(0)).Go();
				} else {
					assert(0 && "Unreachable");
				}
			} else if (node->Cast(sw)) {
				// do not inline switch sub-branches because they're a bit special (scoping)!
				auto cpy = sw->MakeMutableCopy();
				for (unsigned i(0);i < cpy->GetNumCons();++i) {
					cpy->Reconnect(i, cpy->GetUp(i)->IdentityTransform(*this));
				}
				myWeight += 6;
				return cpy;
			}
			myWeight++;
			return node->IdentityTransform(*this);
		}

	};

	Graph<Typed> CallGraphSimplify::operator()(CTRef root, int& myWeight) {
		myWeight = 0;
		auto f = simplifiedGraphs.find(root);
		if (f == simplifiedGraphs.end()) {
			f = simplifiedGraphs.insert(std::make_pair(root, SubgraphSimplify(*this, root).Go())).first;
		} 
		return f->second;
	}
};

std::ostream& operator<<(std::ostream& strm, const K3::Backends::CallGraphNode& node) {node.Output(strm);return strm;}
