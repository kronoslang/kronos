#pragma once
#include "RegionNode.h"

namespace K3 {
	namespace Nodes {
		class Typed;
	};

	namespace Reactive {
		class Node : public CachedTransformNode<ImmutableNode>, public RegionAllocated, REFLECTING_CLASS {
		public:
			const Node* GetUp(int idx) const { return static_cast<const Node*>(GetCon(idx)); }
			const Node* const* GetConnectionsArray() const { return (const Node*const*)upstream; }
			virtual const Node* First() const { return this; }
			virtual const Node* Rest() const { return this; }
			virtual bool IsFused() const = 0;
			static bool VerifyAllocation(void*, const void*) { return true; }
			unsigned ComputeGraphHash(bool canFinalize) const { return 0; } // disable hash computation for reactive descriptors
		};
	};

	using CRRef = const Reactive::Node*;
};

namespace std {
	template <> struct hash<K3::Reactive::Node>{size_t operator()(const K3::Reactive::Node* n) const {return n->GetHash();}};
};