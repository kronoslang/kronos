#include "common/PlatformUtils.h"

#include "CodeMotionPass.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "DynamicVariables.h"
#include "EnumerableGraph.h"
#include "CompilerNodes.h"
#include "Reactive.h"

#include <algorithm>


namespace K3 {
	namespace Backends{
		using namespace Nodes;
		EquivalentExpression EquivalentExpression::Pair(const EquivalentExpression& lhs, const EquivalentExpression& rhs) {
			if (lhs.inlinedExpr == 0 && rhs.inlinedExpr == 0) return EquivalentExpression(0, MaximumTrackDistance + 1);
			else {
				CTRef lg(lhs.inlinedExpr ? lhs.inlinedExpr : Typed::Nil());
				CTRef rg(rhs.inlinedExpr ? rhs.inlinedExpr : Typed::Nil());
				EquivalentExpression tmp(const_cast<Typed*>(Nodes::Pair::New(lg, rg)), std::max(lhs.distanceFromLeaf, rhs.distanceFromLeaf));
				if (std::min(lhs.distanceFromLeaf, rhs.distanceFromLeaf) < MaximumTrackDistance) {
					tmp.pairLhs = new RefCounted<EquivalentExpression>(lhs);
					tmp.pairRhs = new RefCounted<EquivalentExpression>(rhs);
				}
				return tmp;
			}
		}

		EquivalentExpression EquivalentExpression::First() const {
			if (pairLhs) return pairLhs;
			if (inlinedExpr) return EquivalentExpression(const_cast<Typed*>(First::New(inlinedExpr)), distanceFromLeaf + 1);
			return EquivalentExpression(0);
		}

		EquivalentExpression EquivalentExpression::Rest() const {
			if (pairRhs) return pairRhs;
			if (inlinedExpr) return EquivalentExpression(const_cast<Typed*>(Rest::New(inlinedExpr)), distanceFromLeaf + 1);
			return EquivalentExpression(0);
		}


		CodeMotionAnalysis::CodeMotionAnalysis(CTRef root, EquivalenceClassMap& eqClassMap, EquivalentExpression arg, CodeMotionAnalysis *parent, CTRef enclosing, size_t vec)
			:CachedTransform(root), equivalenceClasses(eqClassMap), argument(arg), codeVectorSize(vec),
			parent(parent), enclosing(enclosing) {
		}

		void CodeMotionAnalysis::FillUniquePath(UniquePathVector& dest) {
			if (parent) {
				parent->FillUniquePath(dest);
				dest.push_back(enclosing);
			}
		}

		UniquePathVector CodeMotionAnalysis::GetUniquePath(CTRef to) {
			UniquePathVector tmp;
			FillUniquePath(tmp);
			tmp.push_back(to);
			return tmp;
		}


		EquivalentExpression CodeMotionAnalysis::_operateInsertCache(CTRef src) {
			src->GetHash(true);
			auto& item = cache.insert(src, EquivalentExpression(src->ConstructShallowCopy(), -1));
			return item.second = operate(src);
		}

#define TYPE_CHECK(type) IsOfExactType<type>(node) || 
#define IS_A(...) META_MAP(TYPE_CHECK,__VA_ARGS__) false

		static bool IsNonTrivial(CTRef graph) {
			return Qxx::FromGraph(graph).Where([](CTRef node)
			{
				return IS_A(FunctionCall,
					FunctionSequence,
					RingBuffer,
					GetGlobalVariable,
					ReactiveOperators::BaseRate);
			}).Any();
		}

#undef TYPE_CHECK
#undef IS_A

		void CodeMotionAnalysis::AddExpressionClass(CTRef from, const EquivalentExpression& ee) {
			/* decompose pairs before adding equivalents */
			if (ee.IsPair()) {
				Pair *p;
				if (from->Cast(p)) {
					AddExpressionClass(p->GetUp(0), ee.First());
					AddExpressionClass(p->GetUp(1), ee.Rest());
				}
			} else if (ee.inlinedExpr && ee.distanceFromLeaf > 0) {
				auto f(equivalenceClasses.find(ee.inlinedExpr));
				if (f == equivalenceClasses.end()) {
					RegionAllocator finalExpression;
					f = equivalenceClasses.insert(std::make_pair(
						Graph<Typed>(Transform::Identity<const Typed>(ee.inlinedExpr).Go()),
						ExpressionOccurrences())).first;
				}

				f->second.Occurrences.push_back(GetUniquePath(from));
				f->second.count += check_cast<unsigned int>(codeVectorSize);
			}
		}

		EquivalentExpression CodeMotionAnalysis::operate(CTRef source) {
			Pair *p;
			First *f;
			Rest *r;
			std::pair<CTRef, EquivalentExpression> *promise(cache.find(source));
			/* handle pair algebra with no or fulfilled promises */
			if (source->Cast(p) && (promise == 0 || promise->second.distanceFromLeaf >= 0))
				return EquivalentExpression::Pair((*this)(p->GetUp(0)), (*this)(p->GetUp(1)));
			if (source->Cast(f) && (promise == 0 || promise->second.distanceFromLeaf >= 0))
				return (*this)(f->GetUp(0)).First();
			if (source->Cast(r) && (promise == 0 || promise->second.distanceFromLeaf >= 0))
				return (*this)(r->GetUp(0)).Rest();

			/* trace dataflows from global graph leaves up to a maximum distance */
			if (source->GetNumCons()) {
				std::vector<EquivalentExpression> eeArray(source->GetNumCons());
				//					(EquivalentExpression*)alloca(sizeof(EquivalentExpression) * source->GetNumCons());

				int maxDist(0);
				bool isRecursive(false);
				for (unsigned i(0); i < source->GetNumCons(); ++i) {
					eeArray[i] = (*this)(source->GetUp(i));
					if (eeArray[i].distanceFromLeaf > maxDist) maxDist = eeArray[i].distanceFromLeaf;
					isRecursive |= eeArray[i].distanceFromLeaf < 0;
					
					FunctionCall *fc;
					FunctionSequence *fs;
					if (source->Cast(fc)) {							
						//std::clog << "[Code Motion Analysis: " << fc->GetLabel() << "]\n" << *fc->GetBody();
						EquivalentExpression ee = CodeMotionAnalysis(fc->GetBody(), equivalenceClasses, eeArray[0], this, fc, codeVectorSize).Go();
						// track the result of the function call if we're not in a recursive branch
						if (eeArray[i].distanceFromLeaf >= EquivalentExpression::MaximumTrackDistance || source == GetRoot()) {
							if (promise == 0 || promise->second.distanceFromLeaf > 0) return ee;
						} else break;
						// otherwise stop tracking
					} else if (source->Cast(fs)) {
						//std::clog << "[Code Motion Analysis: " << fs->GetLabel() << "]\n";
						CodeMotionAnalysis(fs->GetIterator(), equivalenceClasses, EquivalentExpression(0), this, fs, codeVectorSize * fs->GetRepeatCount()).Go();
						CodeMotionAnalysis(fs->GetGenerator(), equivalenceClasses, EquivalentExpression(0), this, fs, codeVectorSize * fs->GetRepeatCount()).Go();
						CodeMotionAnalysis(fs->GetTailCall(), equivalenceClasses, EquivalentExpression(0), this, fs, codeVectorSize).Go();
						break;
					}

				}

				/* too distant? terminate leaf tracking for this branch. */
				if (maxDist >= EquivalentExpression::MaximumTrackDistance) {
					if (isRecursive == false) {
						/* add nonrecursive nodes to global leaf list */
						for (unsigned i(0); i<source->GetNumCons(); ++i) {
							if (eeArray[i].inlinedExpr && eeArray[i].distanceFromLeaf > 0) {
								if (IsNonTrivial(eeArray[i].inlinedExpr)) {
									AddExpressionClass(source->GetUp(i), eeArray[i]);
								} 
							}
						}
					}
					return EquivalentExpression(0);
				}

				/* increment leaf distance if not recursive */
				EquivalentExpression ee(0, maxDist < 0 ? maxDist : maxDist + 1);
				/* use promise instance if available (recursive case) or construct */
				if (promise) ee.inlinedExpr = promise->second.inlinedExpr;
				else ee.inlinedExpr = source->ConstructShallowCopy();


				/* finalize */
				for (unsigned i(0); i < source->GetNumCons(); ++i) {
					if (eeArray[i].inlinedExpr)
						ee.inlinedExpr->Reconnect(i, eeArray[i].inlinedExpr);
				}

				return ee;
			} else {
				Argument *arg;
				if (source->Cast(arg)) return argument;
				else return EquivalentExpression(const_cast<Typed*>(source), 0);
			}
		}

		CodeMotionPass::CodeMotionPass(CTRef root, EquivalenceClassMap& eqmap, bool finalBoundary) :Identity(root), equivalenceClasses(eqmap), materializeAll(finalBoundary) {
			for (auto ee : equivalenceClasses) {
				for (auto up : ee.second.Occurrences) {
					if (up.size() == 1) {
						/* this substitution is local to this function body */
						up.front()->GetHash(true);
						substitutions.insert(std::make_pair(up.front(), ee.first));
					}
				}
			}

			Qxx::FromGraph(root).OfType<SetGlobalVariable>()
				.Select([](const SetGlobalVariable* setvar)
			{ return setvar->GetUID(); }).ToContainer(variableBoundaries);
		}

		CTRef CodeMotionPass::operate(CTRef src) {
			/* look for substitutes */
			auto f(substitutions.find(src));
			if (f != substitutions.end()) {
				/* this expression should be replaced */
				/* set up as a dynamic scope variable with the equivalence class expr as UID */
				insert(f->second);
				return GetGlobalVariable::New(f->second, ResultTypeWithNoArgument(f->second).Go(), Type::Nil, std::make_pair(0,0));
			}

			FunctionCall *fc;
			FunctionSequence *fs;

			src->Cast(fc);
			src->Cast(fs);

			if (fc || fs) {
				EquivalenceClassMap relevantExpressions;

				Qxx::From(equivalenceClasses)
					.Select([src](std::pair<Graph<Typed>, ExpressionOccurrences> eo)
				{
					eo.second.Occurrences =
						Qxx::From(eo.second.Occurrences)
						.Where([src](const UniquePathVector& upv)
					{
						return upv[0] == src;
					})
						.Select([](const UniquePathVector& upv)
					{
						return UniquePathVector(upv.begin() + 1, upv.end());
					}).ToVector();
					return eo;
				})
					.Select([&relevantExpressions](const std::pair<Graph<Typed>, ExpressionOccurrences>& eo)
				{
					if (eo.second.Occurrences.empty() == false) relevantExpressions.insert(eo);
					return 0;
				}).Now();

				if (relevantExpressions.empty() == false) {
					if (fc) {
						FunctionCall *nfc(fc->MakeMutableCopy());
						for (unsigned i(0); i < nfc->GetNumCons(); ++i) nfc->Reconnect(i, (*this)(nfc->GetUp(i)));
						CodeMotionPass subPass(fc->GetBody(), relevantExpressions);
						nfc->SetBody(subPass.Go());
						return materializeVariables(subPass, nfc);
					} else if (fs) {
						FunctionSequence *nfs(fs->MakeMutableCopy());
						for (unsigned i(0); i < nfs->GetNumCons(); ++i) nfs->Reconnect(i, (*this)(nfs->GetUp(i)));
						CodeMotionPass subPassI(fs->GetIterator(), relevantExpressions);
						CodeMotionPass subPassG(fs->GetGenerator(), relevantExpressions);
						CodeMotionPass subPassT(fs->GetTailCall(), relevantExpressions);
						/* boundaries in any of the func seq parts affect all the other parts as well */
						subPassI.variableBoundaries.insert(subPassG.variableBoundaries.begin(), subPassG.variableBoundaries.end());
						subPassI.variableBoundaries.insert(subPassT.variableBoundaries.begin(), subPassT.variableBoundaries.end());
						subPassT.variableBoundaries = subPassG.variableBoundaries = subPassI.variableBoundaries;
						nfs->SetIterator(subPassI);
						nfs->SetGenerator(subPassG);
						nfs->SetTailCall(subPassT);

						return materializeVariables(subPassI,
							materializeVariables(subPassG, materializeVariables(subPassT, nfs)));
					}
				}
			}
			return src->IdentityTransform(*this);
		}

		Typed* CodeMotionPass::materializeVariables(CodeMotionPass& cmp, Typed* call) {
			Deps *variableMaterializer(0);
			for (auto var : cmp) {
				if (materializeAll || variableBoundaries.find(var) != variableBoundaries.end()) {
					if (variableMaterializer == 0) {
						variableMaterializer = Deps::New();
						variableMaterializer->Connect(call->GetUp(0));
					}
					variableMaterializer->Connect(SetGlobalVariable::New(var, var));
				} else insert(var);
			}

			if (variableMaterializer) {
				/* some variables must be materialized */
				call->Reconnect(0, variableMaterializer);
			}
			return call;
		}

		CodeMotionPass::operator CTRef() {
			auto tmp(Go());
			return tmp;
		}
	}
}