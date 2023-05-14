#include "common/PlatformUtils.h"

#include "kronos_abi.h"
#include "DriverSignature.h"
#include "Reactive.h"

#include <tuple>
#include <algorithm>
#include <cassert>

namespace K3 {
	using namespace std;

	DriverSignature::DriverSignature(const Type& t) {
		if (t.IsUserType()) {
			if (t.GetDescriptor( ) == &Reactive::RecursiveDriver) {
                auto success = t.UnwrapUserType().Tie(metadata); (void)success;
                assert(success);
				priority = Type::Nil;
				mul = div = 0;
				drvClass = Recursive;
				return;
			} else {
				double m, d;
				if (t.UnwrapUserType( ).Tie(metadata, priority, m, d)) {
					Type maskList = t.UnwrapUserType( ).Rest(4);
					for (auto t = maskList; t.IsPair( ); t = t.Rest( ))
						masks.push_back((int)(intptr_t)t.First( ).GetInternalTag( ));
					drvClass = User;
					mul = m;
					div = d;
					return;
				}
				assert(0 && "Bad metadata");
			}
		} else if (t == Type(&Reactive::InitializationDriver) || t == Type(&Reactive::NullDriver)) { 
			metadata = t;
			mul = div = 1;
			drvClass = InitOrNull; 
			return; 
		} else if (t == Type(&Reactive::ArgumentDriver)) {
			metadata = t;
			mul = div = 1;
			drvClass = EvalArgument;
			return;
		}
		
		INTERNAL_ERROR("Unrecognized driver type");
	}

	DriverSignature::DriverSignature(const Type& meta, const Type& prio, double mul, double div)
		:priority(prio),metadata(meta),mul(mul),div(div),drvClass(User) { }

	DriverSignature::operator Type() const {
		switch (drvClass) {
			case EvalArgument:
			case InitOrNull: return GetMetadata( );
			case User: {
				Type maskList = Type::Nil;
				for (auto m : masks) maskList = Type::Pair(Type::InternalUse((const void*)(intptr_t)m), maskList);

				return Type::User(&Reactive::ReactiveDriver,
					Type::Tuple(metadata, priority, Type(check_cast<double>(mul)), Type(check_cast<double>(div)), maskList));
			}
			case Recursive: INTERNAL_ERROR("Querying a recursive clock");
			default: INTERNAL_ERROR("Unsupported driver class");

		}
	}

	int DriverSignature::OrdinalCompare(const DriverSignature& rhs) const {
		if (tie(drvClass,priority) < tie(rhs.drvClass,rhs.priority)) return -1;
		if (tie(drvClass,priority) > tie(rhs.drvClass,rhs.priority)) return 1;

		if (metadata == rhs.metadata) {
			if (mul*rhs.div > rhs.mul*div) return 1;
			if (mul*rhs.div < rhs.mul*div) return -1;
		}

		for(unsigned i(0);i<min(masks.size(),rhs.masks.size());++i) {
			if (masks[i] != rhs.masks[i]) return 0;
		}

		if (masks.size() < rhs.masks.size()) return 1;
		if (masks.size() > rhs.masks.size()) return -1;
		return 0;
	}
}
