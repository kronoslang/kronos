#include <map>

#include "Graphviz.h"
#include "EnumerableGraph.h"
#include "Evaluate.h"

namespace K3 {
	void ExportGraphviz(std::ostream& dot, const std::string& graphLabel, Nodes::CGRef graph, NodeVisitor<CGRef> customVisitor, EdgeVisitor<CGRef> edgeVisitor) {
		dot << "digraph "<<graphLabel<<" {\nordering=in;\n";
		for (auto n : Qxx::FromGraph(graph)) {
			dot << "n" << n << " [label=\"";
			n->Output(dot);
			dot << "\"];\n";
		}
		for (auto n : Qxx::FromGraph(graph)) {
			for (auto up : n->Upstream( )) {
				dot << "n" << up << " -> n" << n << ";\n";
			}
		}
		dot << "}";
	}

	void ExportGraphviz(std::ostream& dot, const std::string& graphLabel, Nodes::CTRef graph, NodeVisitor<CTRef> customVisitor, EdgeVisitor<CTRef> edgeVisitor)
	{
		dot << "digraph "<<graphLabel<<" {\nordering=in;\n";
		int colorcount = 1;
		std::map<const K3::Reactive::Node*, int> colors;
		for (auto n : Qxx::FromGraph(graph)) {
			if (colors.find(n->GetReactivity( )) == colors.end( )) {
				colors[n->GetReactivity( )] = colorcount;
				if (++colorcount > 12) colorcount = 1;
			}

			if (!customVisitor || customVisitor(dot, n, colors[n->GetReactivity()]) == false) {
				dot << "n" << n << " [label=\"";
				n->Output(dot);
				dot << "\", style=filled, fillcolor=\"/set312/" << colors[n->GetReactivity()] << "\"];\n";
			}
		}
		for (auto n : Qxx::FromGraph(graph)) {
			Nodes::FunctionBase *fb;
			n->Cast(fb);
			for (auto up : n->Upstream( )) {
				if (!edgeVisitor || edgeVisitor(dot, n, up) == false) {
					dot << "n" << up << " -> n" << n;
					if (fb) dot << " [label=\"" << fb->ArgumentType() << "\"]";
					else if (up->Cast(fb)) {
						dot << " [label=\"" << fb->FixedResult() << "\"]";
						fb = nullptr;
					}
					dot << ";\n";
				}
			}
		}
		dot << "}";
	}
}