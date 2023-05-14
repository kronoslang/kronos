#pragma once

#include "NodeBases.h"
#include <functional>

namespace K3 {
	template <typename N> using NodeVisitor = std::function<bool(std::ostream&, N, int)>;
	template <typename N> using EdgeVisitor = std::function<bool(std::ostream&, N,N)>;
	using Nodes::CTRef;
	using Nodes::CGRef;
	void ExportGraphviz(std::ostream& dot, const std::string& label, CTRef graph, NodeVisitor<CTRef> customNodeVisitor = NodeVisitor<CTRef>(), EdgeVisitor<CTRef> customEdgeVisitor = EdgeVisitor<CTRef>());
	void ExportGraphviz(std::ostream& dot, const std::string& label, CGRef graph, NodeVisitor<CGRef> customNodeVisitor = NodeVisitor<CGRef>(), EdgeVisitor<CGRef> customEdgeVisitor = EdgeVisitor<CGRef>());
}

