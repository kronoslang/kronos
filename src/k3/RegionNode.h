#pragma once

#include "common/DynamicScope.h"
#include "common/Ref.h"
#include "k3/ImmutableNode.h"
#include <vector>
#include <type_traits>


class DisposableClass{
public:
	virtual ~DisposableClass() {}
};

class MemoryRegion : public RefCounting {
	friend class RegionAllocator;
	std::vector<void *> allocation;
	std::vector<DisposableClass*> dtors;
	size_t pos;
	size_t size;
	static const int InitialSize = 512;
	char owned[InitialSize];
	MemoryRegion(const MemoryRegion&) = delete;
	MemoryRegion& operator=(const MemoryRegion&) = delete;
public:
	MemoryRegion();
	~MemoryRegion();
	void *AllocateAligned(size_t bytes);
	void* Allocate(size_t bytes) {return AllocateAligned((bytes + 15) & (~15));}
	static MemoryRegion* GetCurrentRegion();
	void AddToCleanupList(DisposableClass *c);
};

class RegionAllocator{
	MemoryRegion* const prevRegion;
	Ref<MemoryRegion> region;
	void *operator new(size_t);
	RegionAllocator& operator=(RegionAllocator& src);
	RegionAllocator(const RegionAllocator& src);
public:
	void Done();
	RegionAllocator();
	RegionAllocator(MemoryRegion *region);
	~RegionAllocator();
 	operator Ref<MemoryRegion>() {return region;}
	MemoryRegion* GetRegion() const {return (MemoryRegion*)region.Pointer();}
};

class RegionAllocated
{
private:
	MemoryRegion* hostRegion;
protected:
	void MakeStatic() {hostRegion=0;}
public:
	MemoryRegion* GetHostRegion() const {return hostRegion;}
	RegionAllocated(MemoryRegion* host = MemoryRegion::GetCurrentRegion()):hostRegion(host) {}
	RegionAllocated(const RegionAllocated& src):hostRegion(MemoryRegion::GetCurrentRegion()) {}
};

class RegionNodeBase : public ImmutableNode, public RegionAllocated {
public:
	RegionNodeBase(MemoryRegion* host = MemoryRegion::GetCurrentRegion()):RegionAllocated(host) {}
//	void InterRegionConnect(RegionNodeBase *upstream);
#ifndef NDEBUG
	void Connect(const ImmutableNode *upstream);
#endif
};

/* supress warning about nonaccessible base class destructors */
//#pragma warning(disable:4624)

template <class IMPLEMENTATION>
class RegionNode : public IMPLEMENTATION{
public:
		typedef IMPLEMENTATION node_t;
#ifndef NDEBUG
	static const bool __MustHaveTrivialDTOR = true;
#endif
};

template <class IMPLEMENTATION>
class DisposableRegionNode : public IMPLEMENTATION, public DisposableClass{
public: DisposableRegionNode() 
		{if (this->GetHostRegion()) this->GetHostRegion()->AddToCleanupList(this);} 
		typedef IMPLEMENTATION node_t;
#ifndef NDEBUG
	static const bool __MustHaveTrivialDTOR = false;
#endif
};

#ifndef NDEBUG
#define REGION_ALLOC(classname) void *operator new(size_t bytes) \
{ \
	assert(MemoryRegion::GetCurrentRegion() != 0 && "Undefined allocator region");assert(sizeof(classname) <= bytes);\
	if (classname::__MustHaveTrivialDTOR && std::is_trivially_destructible<classname>::value == false) assert(0 && "MAKE CLASS DISPOSABLE"); \
	return MemoryRegion::GetCurrentRegion()->Allocate(bytes);\
} void operator delete(void *ptr) {}
#else
#define REGION_ALLOC(classname) void *operator new(size_t bytes) {return MemoryRegion::GetCurrentRegion()->Allocate(bytes);} void operator delete(void *ptr) {}
#endif