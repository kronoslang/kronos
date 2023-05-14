#pragma once

#include "RegionNode.h"
#include "ImmutableNode.h"
#include "Reactive.h"
#include "CompilerNodes.h"

#include <unordered_set>
#include <unordered_map>

namespace K3 {

	/* Generic call graph simplification: unification and inlining */
	class CallGraphSimplify {
		std::unordered_map<Graph<Typed>, CTRef> simplifiedGraphs;
	public:
		Graph<Typed> operator()(CTRef root, int& weight);
	};


	namespace Backends{

		class CallGraphNode;
		typedef std::unordered_map<const Nodes::Subroutine*,const CallGraphNode*> CallGraphMap;

		class CallGraphNode : public Immutable::DynamicUpstreamNode<DisposableRegionNode<::RegionNodeBase>>
		{
			std::unordered_set<Type> ActiveStatesIn;
			const Nodes::Subroutine* subr;
		public:
			CallGraphNode(const Nodes::Subroutine* associatedSubroutine = 0);
			void AddActiveState(const Type& t) {ActiveStatesIn.insert(t);}
			void Output(std::ostream& stream) const override;
			void* GetTypeIdentifier() const override {static char c;return &c;}
			void Connect(const CallGraphNode *cgn) {DynamicUpstreamNode::Connect(cgn);}
			const CallGraphNode* GetUp(unsigned i) const {return (CallGraphNode*)GetCon(i);}

			const std::unordered_set<Type>& GetActiveStates() const {return ActiveStatesIn;}
			void SetStaticallyActive() { ActiveStatesIn.clear(); }
		};

		/**
		 * Analyze a call graph from a post-side effects pass
		 */
		CallGraphNode *AnalyzeCallGraph(const Nodes::Subroutine*,CTRef,CallGraphMap&);
	};
};

std::ostream& operator<<(std::ostream& strm, const K3::Backends::CallGraphNode&);
