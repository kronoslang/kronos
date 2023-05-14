#include <cstdlib>
#include <cstring>
#include <ostream>
#include <cstdint>
#include "common/DynamicScope.h"
#include "ImmutableNode.h"
#include "SmallContainer.h"

ImmutableNode::ImmutableNode():numCons(0),hash(0),finalized(false)
{
}

ImmutableNode::ImmutableNode(const ImmutableNode& src):numCons(src.numCons),hash(0),finalized(false)
{
}

unsigned ImmutableNode::ComputeGraphHash(bool canFinalize) const
{
	if (!finalized)
	{
		if (canFinalize)
		{
			((ImmutableNode*)this)->finalized = true;
			((ImmutableNode*)this)->hash = 1;
			for(unsigned i(0);i<numCons;++i) HASHER(((ImmutableNode*)this)->hash,GetCon(i)->ComputeGraphHash(true));
			HASHER(((ImmutableNode*)this)->hash,ComputeLocalHash());
		}
		else return 0;
	}
	return hash;
}

void ImmutableNode::Output(std::ostream& stream) const
{
	stream << GetLabel();// << "{"<<std::hex<<(uint32_t)this<<"}";
}

bool ImmutableNode::operator==(const ImmutableNode& rhs) const
{
	return Compare(rhs) == 0;
}

typedef std::unordered_map<std::pair<const ImmutableNode*,const ImmutableNode*>, const ImmutableNode *> EdgeMap; 

namespace std
{
	template <class A, class B> struct hash<std::pair<A,B>>
	{
		size_t operator()(const std::pair<A,B> &p) const
		{
			return std::hash<A>()(p.first) + std::hash<B>()(p.second);
		}
	};
}

static uint64_t hasher(uint64_t v) {
	return v * 0x9e3779b97f4a7c13;
}

class VisitedEdges {
	static const int bloom_size = 4;
	std::uint64_t key_bloom[bloom_size];
	const ImmutableNode *l, *r;
	const VisitedEdges *predecessor;
	VisitedEdges(const VisitedEdges* ve, const ImmutableNode* lhs, const ImmutableNode* rhs)
		:l(lhs),r(rhs),predecessor(ve) {
		uint64_t h = (uintptr_t)lhs;
		for (int i = 0; i < bloom_size; ++i) {
			h = hasher(h);
			key_bloom[i] = predecessor->key_bloom[i] | h;
		}
	}
public:
	VisitedEdges():predecessor(0),l(0),r(0) { 
		for (int i = 0; i < bloom_size; ++i) key_bloom[i] = 0;
	}

	bool HaveVisited(const ImmutableNode& left, const ImmutableNode& right) const {
		uint64_t keyhash = (uintptr_t)&left;
		for (int i = 0; i < bloom_size; ++i) {
			keyhash = hasher(keyhash);
			if ((key_bloom[i] & keyhash) != 0) {
				for (auto ve = predecessor; ve; ve = ve->predecessor) {
					if (ve->l == &left && ve->r == &right) return true;
				}
			}
		}
		return false;
	}

	VisitedEdges Edge(const ImmutableNode& lhs, const ImmutableNode& rhs) const {
		return VisitedEdges(this,&lhs,&rhs);
	}
};


static int CompareGraph(const ImmutableNode& lhs, const ImmutableNode& rhs, const VisitedEdges& edges)
{
	if (&lhs == &rhs) return 0;
#ifndef NDEBUG
#else
	if (lhs.IsHashFinalized() && rhs.IsHashFinalized()) {
		if (lhs.GetHash()<rhs.GetHash()) return -1;
		else if (lhs.GetHash()>rhs.GetHash()) return 1;
	}
#endif
	auto lt(lhs.GetTypeIdentifier()),rt(rhs.GetTypeIdentifier());
	if (lt > rt) return 1;
	if (lt < rt) return -1;

	if (edges.HaveVisited(lhs,rhs)) {
		return 0;
	}

	auto successorEdges = edges.Edge(lhs,rhs);

	int r(lhs.LocalCompare(rhs));
	if (r) return r;
	if (lhs.GetNumCons()>rhs.GetNumCons()) return 1;
	else if (lhs.GetNumCons()<rhs.GetNumCons()) return -1;
	else {		
		for(unsigned int i(0);i<lhs.GetNumCons();++i) {
			int r(CompareGraph(*lhs.GetCon(i),*rhs.GetCon(i),successorEdges));
			if (r) {
#ifndef NDEBUG
				if (lhs.GetCon(i)->GetHash() == rhs.GetCon(i)->GetHash() && lhs.GetCon(i)->IsHashFinalized()) {
						//puts("** Hash collision detected **");
						//puts(lhs.GetLabel());
				}
#endif
				return r;
			}
		}
	}
	return 0; // equal
}

int ImmutableNode::Compare(const ImmutableNode& rhs) const
{
	if (this == &rhs) return 0;
	int result = CompareGraph(*this,rhs,VisitedEdges());
#ifndef NDEBUG
	if (result == 0 && finalized && rhs.finalized) assert(GetHash() == rhs.GetHash() && "Bad hash detected");
#endif
	return result;
}
