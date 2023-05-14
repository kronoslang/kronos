#include "LibraryRef.h"
#include "Errors.h"
#include "TLS.h"
#include "Invariant.h"
#include "DynamicVariables.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
#include "CompilerNodes.h"
#include "Stateful.h"
#include "FlowControl.h"
#include <sstream>

namespace K3 {
	namespace Nodes{
		namespace Lib{

			unsigned Reference::ComputeLocalHash() const {
				auto h = GenericPolyadic::ComputeLocalHash();
				for (auto &l : lookup) HASHER(h, std::hash<std::string>()(l));
				return (unsigned)h;
			}

			int Reference::LocalCompare(const ImmutableNode & r) const {
				auto& rr((Reference&)r);
				int t = ordinalCmp(lookup.size(), rr.lookup.size());
				if (t) return t;
				t = ordinalCmp(alias, rr.alias);
				if (t) return t;
				for (size_t i(0);i < lookup.size();++i) {
					t = ordinalCmp(lookup[i], rr.lookup[i]);
					if (t) return t;
				}
				return 0;
			}
			
			bool Reference::CheckCycle(CGRef graph) {
				GenericRingBuffer *rbb;
				if (graph->Cast(rbb)) {
					for (int i = 0;i < 2;++i) {
						if (CheckCycle(graph->GetUp(i))) return true;
					}
					return false;
				}
				Reference *ref;
				if (graph->Cast(ref)) {
					if (lookup.front() == ref->lookup.front()) {
						return true;
					}
				}
				for (int i = 0;i < graph->GetNumCons();++i) {
					if (CheckCycle(graph->GetUp(i))) return true;
				}
				return false;
			}

			void Reference::Resolve(CGRef to) {
				assert(GetNumCons() == 0);
				if (CheckCycle(to)) {
					Connect(Raise::NewFatalFailure("Cyclic definition not allowed without a signal delay"));
				} else {
					Connect(to);
				}
			}


			Specialization Reference::Specialize(SpecializationState& t) const {
				if (GetNumCons()) return t(GetUp(0));
				for (auto &l : lookup) {
                    if (l.front() == ':') {
                        auto s = TLS::ResolveSymbol(l.c_str());
                        if (s) {
                            return t(s);
                        }
                    }
				}
				t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::UndefinedSymbol, "Undefined symbol '%s'.", lookup.front().c_str());
				return TypeError(&FatalFailure, Type(("Unbound symbol '" + lookup.front() + "'").c_str()));
			}

			Specialization Symbol::Specialize(SpecializationState& t) const {
				SPECIALIZE(t, sym, GetUp(0));
				std::stringstream symName;
				sym.result.OutputText(symName);
				auto s = TLS::ResolveSymbol(symName.str().c_str());
				if (s) return t(s);
				t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::UndefinedSymbol, "Undefined symbol '%s'.",symName.str().c_str());
				return TypeError(&FatalFailure, Type(("Unbound symbol '" + symName.str() + "'").c_str()));
			}
		};

		Specialization GenericGetGlobalVariable::Specialize(SpecializationState& spec) const {
			return Specialization(GetGlobalVariable::New(uid,t,Type::Nil,std::make_pair(1,1)),t);
		}
	};
};
