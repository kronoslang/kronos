#include <cstdlib>
#include "RegionNode.h"
#ifndef NDEBUG
#define GUARD_REGION 0
#include <iostream>
#else
#define GUARD_REGION 0
#endif

MemoryRegion::MemoryRegion():pos(0),size(InitialSize)
{
	allocation.push_back(owned);
}

void* MemoryRegion::AllocateAligned(size_t bytes)
{
	assert((bytes & 15) == 0 && "Allocation must be a multiple of 16 bytes");
	if (pos + bytes >= size)
	{
		do size*=2; while (size < bytes);
		pos = 0;
		allocation.push_back(malloc(size+GUARD_REGION));
//		printf("*Allocate(%i) = %x - %x\n",size+GUARD_REGION,allocation.back(),(intptr_t)allocation.back()+size+GUARD_REGION);
#ifndef NDEBUG
		for(unsigned i(0);i<(size+GUARD_REGION)/sizeof(unsigned);++i)
		{
			((unsigned*)allocation.back())[i]=(unsigned)0xdeadbeefdeadbeef;
		}		
#endif
	}
	void *buf((char*)allocation.back()+pos);
	pos+=bytes;
	return buf;
}

MemoryRegion::~MemoryRegion()
{
	for(auto k(dtors.begin());k!=dtors.end();++k) 
	{
		(*k)->~DisposableClass();	
	}
	if (allocation.size()>1) 
	{
#ifndef NDEBUG
		size_t sz = InitialSize;
#endif
		for(auto k(allocation.begin()+1);k!=allocation.end();++k) 
		{
#ifndef NDEBUG
//			printf("*Free(%i) = %x - %x\n",sz+GUARD_REGION,*k,(intptr_t)*k+size+GUARD_REGION);
			memset(*k,0xcdcdcdcd,sz + GUARD_REGION);
			sz<<=1;
#endif
			free(*k);
		}
	}
}

thread_local MemoryRegion* CurrentRegion = 0;
MemoryRegion* MemoryRegion::GetCurrentRegion()
{
	return CurrentRegion;
}

/*void MemoryRegion::AddDependency(const MemoryRegion *subRegion)
{
	if (subRegion == 0) return;
	assert(subRegion == this || subRegion->HasDependency(this) == false); // check for circular dependency
	if (subRegion && !HasDependency(subRegion)) dependencies.push_back(subRegion);
}

bool MemoryRegion::HasDependency(const MemoryRegion *subRegion) const
{
	if (subRegion == this || subRegion == 0) return true;
	for(auto& x : dependencies) if (x->HasDependency(subRegion)) return true;
	return false;
}*/

void MemoryRegion::AddToCleanupList(DisposableClass* node)
{
	dtors.push_back(node);
}


RegionAllocator::RegionAllocator():region(new MemoryRegion),prevRegion(CurrentRegion)
{
#ifndef NDEBUG
	if (CurrentRegion == region)
	{
		std::cerr << "Redundant region allocator\n";
	}
#endif
	CurrentRegion = region;
}

RegionAllocator::RegionAllocator(MemoryRegion *r):region(r),prevRegion(CurrentRegion)
{
	CurrentRegion = region;
}

RegionAllocator::~RegionAllocator()
{
	assert(CurrentRegion == region);
	CurrentRegion = prevRegion;
}

/*void RegionNodeBase::InterRegionConnect(RegionNodeBase *upstream) 
{
	GetHostRegion()->AddDependency(upstream->GetHostRegion());
	ImmutableNode::Connect(upstream);
}*/

#ifndef NDEBUG
#include <iostream>
void RegionNodeBase::Connect(const ImmutableNode *upstream)
{
/*	if (!(ptr->GetHostRegion() == 0 || GetHostRegion()->HasDependency(ptr->GetHostRegion())))
	{
		std::cout << "* Silent inter-region connection\n";
	}*/
	ImmutableNode::Connect(upstream);
}

#endif
