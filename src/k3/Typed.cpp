#include "NodeBases.h"
#include "EnumerableGraph.h"
#include "Evaluate.h"

namespace K3 {
	const Reactive::Node* AnyRX = (const Reactive::Node*) -1;

	CTRef InliningTransform::operate(CTRef src) {
		if (IsOfExactType<Argument>(src)) {
			didVisitArg = true; return arg;
		}
		else return src->IdentityTransform(*this);
	}

	namespace Nodes{
		CTRef Typed::IdentityTransform(GraphTransform<const Typed,CTRef> &copy) const
		{
			Typed *tmp = ConstructShallowCopy();
			assert(GetNumCons() == 0 || tmp->upstream != upstream);
			for(unsigned int i(0);i<tmp->GetNumCons();++i) {
				tmp->Reconnect(i,copy(tmp->GetUp(i)));
			}
			tmp->SetReactivity(GetReactivity( ));
			return tmp;
		}


		int TypedBase::LocalCompare(const ImmutableNode& rhs) const
		{
			TypedBase &tb((TypedBase&)rhs);
			if (GetReactivity() != tb.GetReactivity())
			{
				if (GetReactivity() == AnyRX || tb.GetReactivity() == AnyRX) {
					return 0;
				}
				if (GetReactivity() == 0) return -1;
				else if (tb.GetReactivity() == 0) return 1;
				int t(GetReactivity()->Compare(*tb.GetReactivity()));
				if (t) return t;
			}
			return RegionNodeBase::LocalCompare(rhs);
		}
		
		bool TypedBase::VerifyAllocation(MemoryRegion* region, CTRef node)
		{
/*			bool validAllocation = Qxx::FromGraph(node).Where([&](CTRef node)
			{
				if (region->HasDependency(node->GetHostRegion()) == false)
				{
					std::cerr << "["<<node->GetLabel()<<"] is not correctly connected to allocation graph";
					return true;
				}
				else return false;
			}).Any() == false;

			if (validAllocation == false)
			{
				std::cerr << "In " << *node << std::endl;
			}

			return validAllocation;*/
			return true;
		}
	};
};