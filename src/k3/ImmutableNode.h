#pragma once
#include <cassert>
#include <ostream>
#include <cstring>
#include "SmallContainer.h"
#include "common/Hasher.h"


class ImmutableNode{
	/* comparison support */
	unsigned hash;
	bool finalized;

protected:
	/* connections */
	const ImmutableNode **upstream = nullptr;
	unsigned int numCons = 0;

	/* non-const Mutators */
	void Connect(const ImmutableNode *up) {upstream[numCons++]=up;/*assert(finalized==false);*/}
	void Reconnect(unsigned idx, const ImmutableNode *up) {upstream[idx]=up;/*assert(finalized==false);*/}

	/* graph hash computation */
	virtual unsigned ComputeGraphHash(bool canFinalize) const;
	virtual unsigned ComputeLocalHash() const {return 0;};

	/* construction and destruction */
	ImmutableNode& operator=(const ImmutableNode& src);
protected:
public:
	bool IsHashFinalized() const {return finalized;}

	virtual void Output(std::ostream& stream) const;
	virtual const char *GetLabel() const {return "??";}
	virtual void *GetTypeIdentifier() const = 0;
	virtual bool ShouldCheckRecursion() const {return true;}

	ImmutableNode();
	ImmutableNode(const ImmutableNode& src);

	/* Graph comparison hooks */
	virtual int LocalCompare(const ImmutableNode& rhs) const {return 0;};
	virtual int Compare(const ImmutableNode& rhs) const;

	/* Accessors for traversal */
	unsigned GetNumCons() const {return numCons;}
	const ImmutableNode* GetCon(unsigned int idx) const {assert(idx<numCons); return upstream[idx];}
	const ImmutableNode*& GetCon(unsigned int idx) {assert(idx<numCons);return upstream[idx];}

	bool operator==(const ImmutableNode& rhs) const;
	bool operator!=(const ImmutableNode& rhs) const {return !operator==(rhs);}

	size_t GetHash(bool canFinalize = false) const {return ComputeGraphHash(canFinalize);}
};

namespace std {
	struct ImmutableNodeHasher
	{
		size_t operator()(const ImmutableNode* n) const {return n->GetHash();}
		size_t operator()(const ImmutableNode*& n) const {return n->GetHash();}
	};

	struct ImmutableNodeEq
	{
		bool operator()(const ImmutableNode*a,const ImmutableNode*b) const {return *a == *b;}
		bool operator()(const ImmutableNode*& a,const ImmutableNode*& b) const {return *a == *b;}
	};
}

namespace Immutable{

	template <int N, class BASE>
	class StaticUpstreamNode : public BASE{
		typedef typename BASE::node_t node_t;
		const node_t* ptrs[N];
	protected:
		void Connect(const node_t *up) {assert(this->numCons<N);BASE::Connect(up);}
	public:
		StaticUpstreamNode() { this->upstream = reinterpret_cast<const ImmutableNode**>(ptrs); }
		StaticUpstreamNode(const StaticUpstreamNode& src):BASE(src) { std::memcpy(ptrs,src.ptrs,sizeof(const node_t*)*N); this->upstream = reinterpret_cast<const ImmutableNode**>(ptrs);}
	};

	template <class BASE>
	class StaticUpstreamNode<0,BASE> : public BASE{
	protected:
		void Connect(const ImmutableNode *up) {assert(0 && "Can't connect to leaf node");}
	};

	template <class BASE>
	class DynamicUpstreamNode : public BASE{
		typedef typename BASE::node_t node_t;
		unsigned int consCapacity;
		static const int allocationMultiplier = 2;
		static const int initialAllocation = 4;
		DynamicUpstreamNode& operator=(const DynamicUpstreamNode&) = delete;
	public:
		void Connect(const node_t *up)
		{
			if (this->numCons>=consCapacity)
			{
				this->upstream = (const ImmutableNode**)realloc(this->upstream,consCapacity * allocationMultiplier * sizeof(const ImmutableNode*));
				consCapacity*=allocationMultiplier;
			}
			BASE::Connect(up);
		}

		DynamicUpstreamNode(const DynamicUpstreamNode<BASE> &src) :BASE(src), consCapacity(src.consCapacity)
		{
			this->upstream = (const ImmutableNode**)malloc(consCapacity * sizeof(const ImmutableNode*));
			std::memcpy(this->upstream,src.upstream,consCapacity * sizeof(const ImmutableNode*));
		}

		DynamicUpstreamNode()
		{
			this->upstream = (const ImmutableNode**)malloc(initialAllocation * sizeof(const ImmutableNode*));
			consCapacity = initialAllocation;
		}

		~DynamicUpstreamNode()
		{
			free(this->upstream);
		}
	};
};

