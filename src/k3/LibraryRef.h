#pragma once

#include "NodeBases.h"
#include "UserErrors.h"
#include <string>

static bool operator<(const std::string& a, const std::string &b)
{
	return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end());
}

namespace K3 {
	namespace Nodes{
		class Evaluate;
		namespace Lib{
			GENERIC_NODE(Reference, GenericPolyadic)
				std::vector<std::string> lookup;
				bool alias;
				Reference(std::vector<std::string> l,bool alias):lookup(std::move(l)),alias(alias) {}
				int LocalCompare(const ImmutableNode& r) const override;
				unsigned ComputeLocalHash() const override;
				bool CheckCycle(CGRef in);
		public:
				static Reference* New(std::vector<std::string> p, bool alias = false) { return new Reference(std::move(p),alias); }
				const std::string& GetName() const { return lookup.front(); }
				void Output(std::ostream& s) const override { s << GetName(); }
				bool IsAlias() const { return alias; }
				const std::vector<std::string>& GetLookupSequence() const { return lookup; }
				CGRef GetLocalResolution() const { return GetNumCons()?GetUp(0):nullptr; }
				void Resolve(CGRef to);
			END

			GENERIC_NODE(Symbol, GenericUnary)
				Symbol(CGRef up) :GenericUnary(up) {}
			PUBLIC
				static Symbol* New(CGRef str) { return new Symbol(str); }
			END
		}
	};
};