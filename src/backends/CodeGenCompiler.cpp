#include "CodeGenCompiler.h"
#include "DriverSignature.h"
#include "Native.h"

namespace K3 {
	namespace Backends {
		/* is a upstream of b? */
		static bool InSubgraph(CTRef a, CTRef b) {
			if (a == b) return true;
			for (unsigned i(0); i < b->GetNumCons(); ++i) {
				if (InSubgraph(a, b->GetUp(i))) return true;
			}
			return false;
		}

		bool ActivityMaskVector::operator<(const ActivityMaskVector& rhs) const {
			auto comp_sub = [](const std::vector<int>& a, const std::vector<int>& b) {
				return lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
			};
			return lexicographical_compare(begin(), end(), rhs.begin(), rhs.end(), comp_sub);
		}

		DriverActivity FilterDriverActivity(const K3::Type& dr, ActivityMaskVector* avm, ICompilationPass& master) {
			if (avm == nullptr) return master.IsDriverActive(dr);

			DriverSignature dsig(dr);
			if (dsig.Masks().size()) {
				std::vector<int> dsigMasks;
				dsigMasks.reserve(dsig.Masks().size());

				for (auto i(dsig.Masks().begin()); i != dsig.Masks().end(); ++i) dsigMasks.push_back(check_cast<int>(*i));

				if (std::find(avm->begin(), avm->end(), dsigMasks) != avm->end()) {
					/* we know his mask set has an active component, so discard all the masks */
					dsig.Masks().clear();
				}
				return RemoveCounter(avm, master.IsDriverActive(dsig));
			} else return RemoveCounter(avm, master.IsDriverActive(dr)); // no masks so will just check the counter
		}

		DriverActivity RemoveCounter(ActivityMaskVector *avm, DriverActivity act) {
			if (act >= Counter) {
				for (auto m : *avm) if (m.size() == 1 && -1 - act == m[0]) return Always;
			}
			return act;
		}

		Ref<ActivityMaskVector> CodeGenTransformBase::CollectionToMask(const Qxx::IEnumerable<Type>& drivers, bool allDrivers) {
			Ref<ActivityMaskVector> result;
			// null driver
			if (drivers.Any() == false) return Ref<ActivityMaskVector>::Cons();

			for (auto dr : drivers) { //.Where([&](const Type& t) {return allDrivers || GetCompilationPass().IsDriverActive(t);})) {
				DriverActivity act = allDrivers ? Always : GetCompilationPass().IsDriverActive(dr);

				if (act != Never) {
					if (result.NotNull() == false) result = new ActivityMaskVector;
					DriverSignature dsig(dr);

					if (act >= Counter) {
						result->emplace_back(std::vector<int>(1, -1 - act));
					} else { // act == ICompilationPass::Always 
						result->emplace_back(std::vector<int>());
					}

					if (dsig.Masks().size()) {
						result->back().reserve(dsig.Masks().size());
						for (auto msk : dsig.Masks()) result->back().push_back((unsigned)msk);
						std::sort(result->back().begin(), result->back().end());
					}

					if (result->back().empty()) {
						return Ref<ActivityMaskVector>::Cons();
					}
				}
			}
			// null or conditionally active
			if (result.NotNull()) std::sort(result->begin(), result->end());
			return result;
		}

		Ref<ActivityMaskVector> CodeGenTransformBase::GetActivityMasks(CTRef node) {
			bool allDrivers = false;
			Ref<ActivityMaskVector> result;
			switch (compilationPass.GetPassType()) {
			case Sizing:
				return nullptr;
			case Initialization:
			case InitializationWithReturn:
				allDrivers = true;
			case Evaluation:
			case Reactive:
				{
					const Nodes::Subroutine *subr;
					if (node->Cast(subr)) {
						return CollectionToMask(Qxx::From(GetCompilationPass().GetCallGraphAnalysis(subr)->GetActiveStates()), allDrivers);
					} else if (node->GetReactivity()) {
						auto tmp(Qxx::FromGraph(node->GetReactivity())
								 .OfType<Reactive::DriverNode>()
								 .Select([](const Reactive::DriverNode* dn) 
										 { return dn->GetID(); })
								 .ToVector());
						return CollectionToMask(Qxx::From(tmp), allDrivers);
					} else {
						return Ref<ActivityMaskVector>::Cons(); // no reactivity data: statically active
					}
				}
			}
			KRONOS_UNREACHABLE;
		}

		bool CodeGenTransformBase::Schedule(const SchedulingUnit& au, const SchedulingUnit& bu) {
			CTRef a = std::get<0>(au);
			CTRef b = std::get<0>(bu);

			if (a == b) return false;

			auto am(std::get<1>(au).Pointer()), bm(std::get<1>(bu).Pointer());

			/* primary: data dependency */
			if (InSubgraph(a, b)) return true;
			if (InSubgraph(b, a)) return false;
			/* secondary: mask block membership, larger blocks first */
			if (am) {
				if (bm == 0) return am->empty();

				if (am->size() < bm->size()) return true;
				if (am->size() > bm->size()) return false;
				/* element-wise compare of blocks */
				for (unsigned i(0); i<am->size(); ++i) {
					if (am->at(i) < bm->at(i)) return true;
					else if (am->at(i) > bm->at(i)) return false;
				}
			} else return bm ? bm->empty() : false;

			/* tertiary: node priority */
			return a->SchedulingPriority() > b->SchedulingPriority();
		}

		bool CodeGenTransformBase::RefersLocalBuffers(CTRef node) {
			Buffer *b;
			if (node->Cast(b) && b->GetAllocation() == Buffer::Stack) return true;

			if (IsOfExactType<Deps>(node) ||
				IsOfExactType<Offset>(node) ||
				IsOfExactType<Dereference>(node) ||
				IsOfExactType<Reference>(node)) {
				return RefersLocalBuffers(node->GetUp(0));
			} else return false;
		}


		std::vector<CodeGenTransformBase::SchedulingUnit> CodeGenTransformBase::TopologicalSort(const std::vector<CodeGenTransformBase::SchedulingUnit>& units) {
			using namespace std;
			struct Priority {
				bool operator()(const SchedulingUnit* a, const SchedulingUnit* b) const {
					const ActivityMaskVector *am(get<1>(*a)), *bm(get<1>(*b));

					if (am == nullptr) {
						if (bm != nullptr) return false;
					} else {
						if (bm == nullptr) return true;
					}

					if (am && bm) {
						if (am->size() < bm->size()) return false;
						if (am->size() > bm->size()) return true;
						/* element-wise compare of blocks */
						for (unsigned i(0); i < am->size(); ++i) {
							if (am->at(i) < bm->at(i)) return true;
							else if (am->at(i) > bm->at(i)) return false;
						}
					}

					/* tertiary: node priority */
					return get<0>(*a)->SchedulingPriority() > get<0>(*b)->SchedulingPriority();
				}
			};

			unordered_multimap<CTRef, const SchedulingUnit*> edges;
			multiset<const SchedulingUnit*, Priority> free;
			unordered_map<const SchedulingUnit*, unsigned> numDeps;
			/* compute edges, initial free set and mask lookup */
			for (unsigned i(0); i < units.size(); ++i) {
				auto& su(units.at(i));
				CTRef node = get<0>(su);
				if (node->GetNumCons()) {
					for (auto up : node->Upstream()) edges.insert(make_pair(up, &su));
					assert(numDeps.find(&su) == numDeps.end() && "Repeated node");
					numDeps[&su] = node->GetNumCons();
				} else {
					free.insert(&su);
				}
			}
			vector<SchedulingUnit> schedule;
			schedule.reserve(units.size());
			assert(free.empty() == false); // assume at least one schedulable node

	#ifndef NDEBUG
			unsigned numDepsNodes = 0;
	#endif
			while (free.empty() == false) {
				const SchedulingUnit *next = *free.begin();
				free.erase(free.begin());
				auto node = get<0>(*next);
				if (IsOfExactType<Deps>(node) == false &&
					IsOfExactType<SubroutineMeta>(node) == false) 
				{
					schedule.push_back(*next);
				}
	#ifndef NDEBUG
				else {
					numDepsNodes++;
				}
	#endif

				auto deps(edges.equal_range(get<0>(*next)));
				for (auto i(deps.first); i != deps.second; ++i) {
					assert(numDeps[i->second] > 0 && "Broken scheduling detected");
					if (--numDeps[i->second] == 0) free.insert(i->second);
				}
			}
	#ifndef NDEBUG
			assert(schedule.size() + numDepsNodes == units.size() && "Unscheduled nodes found");
	#endif
			return schedule;
		}
	}
}