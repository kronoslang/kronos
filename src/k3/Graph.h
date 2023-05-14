#pragma once

#include "RegionNode.h"

template <class N>
class Graph{
	Ref<MemoryRegion> BackingStore;
	const N* root;
public:
	Graph(const N* node = 0, bool canFinalize = true):BackingStore(node?node->GetHostRegion():0),root(node) 
	{
		assert(root == 0 || N::VerifyAllocation(BackingStore.Pointer(),root));
		if (node) {
			node->GetHash(canFinalize);
			node->HasInvisibleConnections( );
		}
	}
	operator const N*() const {return root;}
	operator bool() const {return root!=0;}
	const N* operator->() const {return root;}
	Graph<N>& operator=(const Graph<N>& src) {root = src.root;BackingStore=src.BackingStore;return *this;}
	bool operator<(const Graph<N>& src) const {  return OrdinalCompare(src) < 0;}
	bool operator>(const Graph<N>& src) const {  return OrdinalCompare(src) > 0;}
	bool operator==(const Graph<N>& src) const { return OrdinalCompare(src) == 0;}
	bool operator!=(const Graph<N>& src) const { return OrdinalCompare(src) != 0;}

	int OrdinalCompare(const Graph<N>& src) const {
		if (root == src.root) return 0;
		if (!root && src.root) return -1;
		if (root && !src.root) return 1;
		return root->Compare(*src.root);
	}
};

namespace std
{
	template <typename NODE> struct hash<Graph<NODE>>
	{
		size_t operator()(const Graph<NODE>& g) const {return g->GetHash(true);}
	};
};