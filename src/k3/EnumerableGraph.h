#pragma once
#include "NodeBases.h"
#include "common/Enumerable.h"

namespace K3 {
	using namespace Nodes;

	template <class NODE>
	LAZY_ENUMERATOR(const NODE*,GraphEnumerator)
	{
		Sml::Set<const NODE*> Visited;
		std::vector<const NODE*> _nodestack;
		const NODE *cur;
		const NODE *root;
		GraphEnumerator(){}
		GraphEnumerator(const NODE *start):cur(start),root(start) {}
		LAZY_BEGIN
			_nodestack.clear();
			if (Visited.size()) Visited = Sml::Set<const NODE*>();
			cur = root;
			do
			{
				if (cur == root || cur->MayHaveMultipleDownstreamConnections())
				{
					while(Visited.find(cur))
					{
						if (_nodestack.empty()) goto end;
						cur = _nodestack.back();
						_nodestack.pop_back();
					}

					Visited.insert(cur);
				}

				LAZY_YIELD(cur);

				if (cur->GetNumCons())
				{
					for(unsigned int i(1);i<cur->GetNumCons();++i)
						_nodestack.push_back(cur->GetUp(i));
					cur = cur->GetUp(0);
				}
				else if (_nodestack.size())
				{
					cur = _nodestack.back();
					_nodestack.pop_back();
				}
				else break;
			}
			while (true);
			end:
		LAZY_END
	};
};

namespace Qxx {
	static Enumerable<K3::GraphEnumerator<K3::Nodes::Generic>> FromGraph(const K3::Nodes::Generic *n)
	{
		return K3::GraphEnumerator<K3::Nodes::Generic>(n);
	}

	static Enumerable<K3::GraphEnumerator<K3::Nodes::Typed>> FromGraph(const K3::Nodes::Typed *n)
	{
		return K3::GraphEnumerator<K3::Nodes::Typed>(n);
	}
};