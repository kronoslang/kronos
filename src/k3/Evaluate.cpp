#include "common/Graphviz.h"
#include "Evaluate.h"
#include "Errors.h"
#include "UserErrors.h"
#include "Invariant.h"
#include "Native.h"
#include "TypeRuleGenerator.h"
#include "TypeAlgebra.h"
#include "TupleTypeEnumerator.h"
#include "common/Enumerable.h"
#include "EnumerableGraph.h"
#include "TLS.h"
#include "LibraryRef.h"
#include <iostream>

#include <sstream>
#include <cstring>

//#define DEBUG_SPECIALIZATION

static const int InlineTreshold = 24;

#ifndef NDEBUG
static thread_local K3::Type outerArgumentType;
struct ArgBinder {
	K3::Type old;
	ArgBinder(K3::Type t) { 
		old = outerArgumentType; outerArgumentType = t; 
	}
	~ArgBinder() {
		outerArgumentType = old;
	}
};
#endif

namespace K3 {
	namespace Nodes {

		static TypeDescriptor SequenceSpecializationFailed("seq failed");

		Specialization GenericArgument::Specialize(SpecializationState &t) const {
			return Specialization(t.GetArgumentType().GetSize() ? Argument::New() : Typed::Nil(),
								  t.GetArgumentType());
		}

		Evaluate::Evaluate(const char *l, CGRef graph, CGRef arg, const char *le) :GenericBinary(graph, arg) {
			if (!le) le = l + sizeof(label);
			else if (le > l + sizeof(label)) le = l + sizeof(label);
			snprintf(label, le - l, "%s", l);
		}

		Evaluate* Evaluate::New(const char *label, CGRef graph, CGRef arg,const char *nend) {
			return new Evaluate(label, graph, arg, nend);
		}

		Evaluate* Evaluate::CallLib(const char *qn, CGRef arg) {
			return New(qn, Lib::Reference::New({ qn }), arg);
		}


		class SpecializationTransformHolder : public RefCounting {
			CachedTransform<Generic, Specialization>::map_t state;
			Graph<Typed> evalPoint;
		public:
			SpecializationTransformHolder(const CachedTransform<Generic, Specialization>::map_t& src, CTRef evalPoint) :state(src), evalPoint(evalPoint) {}
			CachedTransform<Generic, Specialization>::map_t& GetTransform() { return state; }
			CTRef GetEvalPoint() { return evalPoint; }
		};

		struct Dataflows {
			CTRef Inline;
			CTRef Outline;
			Type Ty;
		};

		struct DataflowInliner {
			CTRef fnArg;
			MemoryRegion *inner, *outer;

			static CTRef FoldPair(CTRef fst, CTRef rst) {
				First *f; Rest *r;
				if (fst->Cast(f) && rst->Cast(r) && f->GetUp(0) == r->GetUp(0)) {
					return f->GetUp(0);
				} else {
					return Pair::New(fst, rst);
				}
			}

			Dataflows Walk(CTRef body, CTRef fnOutline, const Type& fnResultTy) {
				Native::Constant *c;
				if (body->Cast<Argument>()) {
					return { fnArg, nullptr, Type::Nil };
				} else if (Pair *p = body->Cast<Pair>()) {
					auto l = Walk(body->GetUp(0), fnOutline->GraphFirst(), fnResultTy.First());
					auto r = Walk(body->GetUp(1), fnOutline->GraphRest(), fnResultTy.Rest());

					if (l.Outline == nullptr) l.Outline = Typed::Nil();
					if (r.Outline == nullptr) r.Outline = Typed::Nil();

					RegionAllocator allocOutline{ inner };
					CTRef outline = FoldPair(l.Outline, r.Outline);

					RegionAllocator allocInline{ outer };
					CTRef inlne = FoldPair(l.Inline, r.Inline);

					return {
						inlne,
						outline,
						Type::Pair(l.Ty, r.Ty)
					};
				} else if (body->Cast<First>()) {
					RegionAllocator allocOutline{ inner };
					auto f = Walk(body->GetUp(0), Pair::New(fnOutline, Typed::Nil()), Type::Pair(fnResultTy, Type::Nil));

					auto outline = f.Outline;
					auto ty = f.Ty;

					if (outline != nullptr) {
						outline = outline->GraphFirst();
						ty = ty.First();
					}

					RegionAllocator allocInline{ outer };
					return {
						f.Inline->GraphFirst(), 
						outline, 
						ty
					};
				} else if (body->Cast<Rest>()) {
					RegionAllocator allocOutline{ inner };
					auto r = Walk(body->GetUp(0), Pair::New(Typed::Nil(), fnOutline), Type::Pair(Type::Nil, fnResultTy));

					auto outline = r.Outline;
					Type ty = r.Ty;

					if (outline != nullptr) {
						outline = outline->GraphRest();
						ty = ty.Rest();
					}

					RegionAllocator allocInline{ outer };
					return {
						r.Inline->GraphRest(), 
						outline, 
						ty
					};
				} else if (body->Cast(c)) {
					return {
						body,
						nullptr,
						c->FixedResult()
					};
				} else {
					return {
						fnOutline,
						body,
						fnResultTy
					};
				}
			}
		};

		static Specialization CompleteFunctionCall(const char *label, const std::pair<Graph<Typed>, Type>& spec, const Type& argument, CTRef upstream, bool shouldInline, MemoryRegion* outer) {
			if (!spec.first) return Specialization(spec.first, spec.second);

			MemoryRegion *inner = spec.first->GetHostRegion();

			if (shouldInline) {
				// construct inlined copy
				InliningTransform it(spec.first, upstream);
				return Specialization(it.Go(), spec.second);
			} else {
				if (IsOfExactType<Argument>(spec.first)) {
					// happy path for identity functions
					return Specialization{ upstream, spec.second};
				}

				if (auto constantResult = spec.first->Cast<Native::Constant>()) {
					RegionAllocator allocFrom{ outer };
					return { spec.first->ConstructShallowCopy(), spec.second };
				}

				if (spec.second.GetSize() == 0) {
					auto resTy = spec.second.Fix();
					RegionAllocator allocFrom{ outer };
					return { Native::Constant::New(resTy, nullptr), resTy };
				}

				RegionAllocator allocFrom{ outer };
				auto fcn = FunctionCall::New(label, spec.first, argument.Fix(), spec.second.Fix(Type::GenerateNoRules), upstream);
				// spec.second must be fixed later depending on which parts end up used inside the function
				FunctionCall* fc;
				if (fcn->Cast(fc)) {
					// see if some of the dataflow can be hoisted
					DataflowInliner dfInliner{ upstream, inner, outer };
					auto result = dfInliner.Walk(fc->GetBody(), fc, fc ->FixedResult());
					fcn = result.Inline;

					if (result.Outline != nullptr) {
#ifndef NDEBUG
						if (TLS::GetCurrentInstance()->ShouldTrace("Reflow", label)) {
							std::clog << "Reflow: [" << label << "]\n\n[upstream]\n" << *upstream << "\n\n[body]\n" << *spec.first << "\n\n[outline]\n" << *result.Outline << "\n\n[Inline]\n" << *result.Inline << "\n\n\n";
						}
#endif

						if (result.Outline != fc->GetBody()) {
							fc->SetBody(result.Outline);
							fc->SetResultType(result.Ty.Fix());

#ifndef NDEBUG
							if (TLS::GetCurrentInstance()->ShouldTrace("Reflow", label)) {
								ResultTypeWithConstantArgument rtt{ fc->GetBody(), fc->ArgumentType() };
								auto verifiedType = rtt.Go();
								if (verifiedType != fc->FixedResult()) {
									std::clog << "Callee reflowed from " << spec.second << " to " << verifiedType << "\n";
								}
							}
#endif
							assert(result.Ty.IsFixed() && "stray recursion solver rules appear in dataflowinliner result");
						} else {
							spec.second.Fix();
						}
					}
				} 

#ifndef NDEBUG
				if (TLS::GetCurrentInstance()->ShouldTrace("Reflow", label)) {
					ResultTypeWithConstantArgument rtt{ fcn, outerArgumentType };
					auto verifiedType = rtt.Go();
					if (verifiedType != spec.second) std::clog << "Caller reflowed from " << spec.second << " to " << verifiedType << "\n";
					assert(spec.second.GetSize() == verifiedType.GetSize() && "Incompatible reflow!");
				}
#endif
				return Specialization(fcn, spec.second);
			}
		}

		static bool ShouldInline(CTRef body) {
			// should this form be inlined?
			int wt(0);
			for (auto n : Qxx::FromGraph(body)) {
				wt += n->GetWeight();
				if (wt > InlineTreshold) {
					return false;
				}
			}
			return (wt <= InlineTreshold);
		}

		Specialization Evaluate::SpecializeRecursive(
			SpecializationState& t,
			const Specialization& arg,
			const Qxx::IEnumerable<const Type&>& Forms,
			const std::vector<Nodes::CGRef >* recurPt) const {
			
			t.GetRep().Diagnostic(Verbosity::LogEverything, this, Error::Info, "rec-solver");
			MemoryRegion* surroundingMemoryRegion = MemoryRegion::GetCurrentRegion();
			RegionAllocator GarbageCollector;
			TypeRuleSet RecursionRules;

			bool refFallback(false);

			auto recursionTest(SpecializeBody(t, recurPt,
											  Forms,
											  Specialization(arg.node, Type(new TypeRuleGenerator(
												  arg.result.Fix(Type::GenerateNoRules),
												  RecursionRules))),refFallback));

			assert(!refFallback);

			if (recursionTest.first) {
				// recursive form didn't fire
				RegionAllocator hostNodeAllocator(surroundingMemoryRegion);
				Transform::Identity<const Typed> copy(recursionTest.first);

				return { copy.Go(), recursionTest.second.Fix() };
			}

			if (!recursionTest.second.IsUserType() || recursionTest.second.GetDescriptor() != &RecursionTrap) {
				// we have a propagating error
				return Specialization(nullptr, recursionTest.second);
			} else {
				// we found a recursive form with initial argument evolution rules
				Type tmp(recursionTest.second.UnwrapUserType());
				Type recursiveEvalPoint(tmp.Element(0));
				Type recursiveBody(tmp.Element(1));
				Type recursiveArgument(tmp.Element(2));
				Ref<SpecializationTransformHolder> partialResults = (SpecializationTransformHolder*)tmp.Element(3).GetUnknownManagedObject();

				Type fixedArg = arg.result.Fix(Type::GenerateNoRules);

				RecursionRules.AcceptMoreRules(false); // fix the ruleset
				auto variableArgument(RecursionRules.GetArgumentBundle(fixedArg,
																	   recursiveArgument, GenericArgument::New()));

				if (variableArgument == nullptr) {
					RecursionRules.AcceptMoreRules(true); // the rules may be amended now
					// can't deduce formal argument evolution, produce individual sequences
					class PartialSpec : public SpecializationTransform {
						CGRef recursivePoint;
						Specialization recursiveUpstream;
					public:
						PartialSpec(CGRef root, const Type& arg, SpecializationDiagnostic& rep, CGRef recurPt, Specialization upstream, SpecializationState::Mode m) :
							recursivePoint(recurPt), recursiveUpstream(upstream), SpecializationTransform(root, arg, rep, m) {}
						Specialization operate(CGRef node) {
							if (node == recursivePoint) {
								return recursiveUpstream;
							} else return SpecializationTransform::operate(node);
						}
					};

					PartialSpec partial(recursiveBody.GetGraph(), arg.result, t.GetRep(), recursiveEvalPoint.GetGraph()->GetUp(1),
										Specialization(partialResults->GetEvalPoint(), recursiveArgument), t.mode);

					partial.GetMap() = partialResults->GetTransform();

					Specialization partialSpec = partial.Go();
					{
						RegionAllocator hostNodeAllocator(surroundingMemoryRegion);
						if (partialSpec.node == nullptr || Typed::IsNil(partialSpec.node)) {
							return Specialization(partialSpec.node, partialSpec.result.Fix(Type::GenerateNoRules));
						}
						Transform::Identity<const Typed> copy(partialSpec.node);
						return { copy.Go(), partialSpec.result.Fix(Type::GenerateRulesForAllTypes) };
					}
				} else {
#ifdef DEBUG_SPECIALIZATION
					std::clog << "[ *** " << label << " *** ]" << fixedArg << "\n";
					std::clog << "[BODY] " << *recursiveBody.GetGraph() << endl;
					std::clog << "Closed form argument: " << *variableArgument << std::endl;
					RecursionRules.PrintRules(std::clog);
#endif
					int64_t N(RecursionRules.SolveRecursionDepth(variableArgument));

					while (N > 1) {
#ifdef DEBUG_SPECIALIZATION
						std::clog << "Trying sequence length " << N << endl;
#endif
						// the argument for body after recursion rules no longer apply
						t.GetRep().Diagnostic(Verbosity::LogEverything, this, Error::Info, "try seq %lld", N);

						auto outArg(SpecializationTransform::Infer(variableArgument, Type::InvariantI64(N)));
						outArg = outArg.Fix(Type::GenerateNoRules);

#ifdef DEBUG_SPECIALIZATION
						std::clog << "Outgoing argument: " << outArg << std::endl;
#endif
						bool refFallback(false);
						t.GetRep().Diagnostic(Verbosity::LogEverything, this, Error::Info, "seq tail");
						auto outGoingCall(SpecializeBody(t, 0, Forms, Specialization(arg.node, outArg), refFallback));
						assert(!refFallback);
						if (!outGoingCall.first) {
							// try shorter seq
							N /= 2;
							continue;
						}

#ifdef DEBUG_SPECIALIZATION
						std::clog << "Outgoing result  : " << outGoingCall.second << std::endl;								
#endif
						// build closed form return type
						// build generator by combining recursive argument with result

						LambdaTransform<const Generic, CGRef> genTransform(recursiveBody.GetGraph());

						genTransform.SetTransform([&, rep=recursiveEvalPoint.GetGraph()](CGRef source) {
							if (source == rep) return GenericRest::New(GenericArgument::New());
							else if (IsOfExactType<GenericArgument>(source)) return GenericFirst::New(GenericArgument::New());
							else return source->IdentityTransform(genTransform);
						});

						Graph<Generic> genericIterator = recursiveEvalPoint.GetGraph()->GetUp(1);
						Graph<Generic> genericGenerator = genTransform.Go();

//						clog << *recursiveBody.GetGraph() << endl << *genericGenerator << endl;

						Type finalRecursiveArgument(SpecializationTransform::Infer(variableArgument, Type::InvariantI64(N - 1)));

						TypeRuleSet GeneratorRules;
						Ref<TypeRuleGenerator> grgen(new TypeRuleGenerator(Type::Pair(finalRecursiveArgument, outGoingCall.second, true), GeneratorRules));
						Type generatorRuleLifter(Type::Pair(grgen->First(false), grgen->Rest(false), true));


						Specialization generatorSpec = SpecializationTransform(genericGenerator, generatorRuleLifter, t.GetRep(), t.mode).Go();
						if (generatorSpec.node == nullptr) {
							return Specialization(generatorSpec.node, generatorSpec.result.Fix(Type::GenerateNoRules));
						}

						GeneratorRules.AcceptMoreRules(false);
						auto variableResult(GeneratorRules.GetArgumentBundle(outGoingCall.second, generatorSpec.result,
																			 GenericRest::New(GenericArgument::New())));

						if (variableResult == nullptr) {
							/* try shorter seq */
							N /= 2;
						} else {
#ifdef DEBUG_SPECIALIZATION
							GeneratorRules.PrintRules(std::clog);
#endif

							LambdaTransform<const Generic, CGRef> argumentInverter(variableArgument);
							argumentInverter.SetTransform([&](CGRef node) {
								if (IsOfExactType<GenericArgument>(node))
									return Invariant::Sub(Invariant::Constant::New(Type::InvariantI64(N)),
														  GenericArgument::New());
								else return node->IdentityTransform(argumentInverter);
							});

#ifdef DEBUG_SPECIALIZATION
							std::clog << "Closed form result: " << *variableResult << std::endl;
#endif
							int64_t Ng(GeneratorRules.SolveRecursionDepth(
								GenericPair::New(argumentInverter.Go(), variableResult)));

#ifdef DEBUG_SPECIALIZATION
							std::clog << "Generator form works for " << Ng << " repetitions\n";
#endif

							if (Ng >= N) {
								/* recursion optimization good to go
								 * perform generational garbage collection
								 * by identity transform into a fresh memory region
								 */

								RegionAllocator FinalGraphs;


								CGRef argFormula(Transform::Identity<const Generic>(variableArgument)(variableArgument));
								CGRef resFormula(Transform::Identity<const Generic>(variableResult)(variableResult));
								Graph<Typed> generator(Transform::Identity<const Typed>(generatorSpec.node).Go());
								Graph<Typed> iterator(SpecializationTransform::Process(genericIterator, fixedArg, SpecializationTransform::Normal).second);

#ifdef DEBUG_SPECIALIZATION
								std::clog << "[ITERATOR] " << *genericIterator << std::endl;
								std::clog << "[GENERATOR] " << *genericGenerator << std::endl;
								std::clog << "Iterator: " << *iterator << std::endl << "Generator: " << *generatorSpec.node << std::endl;
								std::clog << "["<<label<<"] Sequence : " << N << std::endl;
#endif

								t.GetRep().Diagnostic(Verbosity::LogEverything, this, Error::Info, "seq %s: len %i", label, (int)N);

								arg.result.Fix();

								RegionAllocator hostNodeAllocator(surroundingMemoryRegion);

								return { FunctionSequence::New(TLS::GetCurrentInstance()->Memoize(GetLabel() + string("_seq")),
																					  argFormula,
																					  resFormula,
																					  iterator,
																					  generator,
																					  outGoingCall.first,
																					  (size_t)N, Argument::New()),
									SpecializationTransform::Infer(variableResult, Type::InvariantI64(N)) };
							} else {
								/* form fails in this sequence length
								* Shorten the sequence
								*/
								if (Ng > 0) N -= Ng;
								else break;
							}
						}
					}
				}
			}

			t.GetRep().Diagnostic(LogEverything, this, Error::Info, "seq failed");
			return Specialization(0, Type(&SequenceSpecializationFailed));
		}

		Specialization Evaluate::Specialize(SpecializationState& t) const {
			return TLS::WithNewStack([this, &t]() mutable {
				return this->SpecializeCore(t);
			});
		}

		Specialization Evaluate::SpecializeCore(SpecializationState &t) const {
			SPECIALIZE_ARGS(t, 0, 1);
			auto bl = t.GetRep().Block(LogAlways, "eval", "label='%s'", label);

#ifndef NDEBUG
			ArgBinder bind{ t.GetArgumentType() };
#endif

			/* check specialization cache */
			Ref<SpecializationCache> cache = TLS::GetCurrentInstance()->GetSpecializationCache();
			SpecializationKey key = SpecializationKey(A0.result, A1.result);
			bool fixed(key.GetGraph().IsFixed() && key.GetArgument().IsFixed());

			if (cache && fixed) {
				auto form(cache->find(key));
				if (form != cache->end()) {
					Graph<Typed> body; Type result; bool shouldInline, isFallback;
					std::tie(body, result, shouldInline, isFallback) = form->second;
					t.GetRep().Diagnostic(LogEverything, this, Error::Info, "cached");
					t.GetRep().SuccessForm(LogTrace, GetLabel(), A1.result, result);
					return CompleteFunctionCall(label, std::make_pair(body, result), A1.result, isFallback ? Pair::New(A0.node, A1.node) : A1.node, 
												shouldInline, MemoryRegion::GetCurrentRegion());
				}
			}

			Type name, recurPts, forms;
			if (A0.result.IsUserType(FunctionTag)) {
				A0.result.UnwrapUserType().Tie(name, recurPts, forms);
				if (A1.result.GetSize()) {
					if (recurPts.IsNil() == false) {
						std::vector<CGRef> rpvec;
						for (;recurPts.IsNil() == false;recurPts = recurPts.Rest()) rpvec.push_back(recurPts.First().GetGraph());
						auto Forms(From(TupleTypeEnumerator(forms)));
						
						auto spec(SpecializeRecursive(t, A1, Forms, &rpvec));

						if (spec.node) {
							if (fixed && cache && spec.result.IsFixed()) {
								cache->emplace(key, std::make_tuple(spec.node, spec.result, true, false));
							} 
							t.GetRep().SuccessForm(LogTrace, GetLabel(), A1.result, spec.result);
							return CompleteFunctionCall(label, std::make_pair(spec.node, spec.result), A1.result, A1.node, true, MemoryRegion::GetCurrentRegion());
						} else if (!spec.result.IsTypeTag() || spec.result.GetDescriptor() != &SequenceSpecializationFailed) {
							return spec;
						}
						// if the failure is specifically SequenceSpecializationFailed, retry non-recursive spec
					}
				}
			} else forms = A0.result;

			bool shouldInline = false, isFallbackForm = false;
			auto Forms(From(TupleTypeEnumerator(forms)));
			auto spec(SpecializeBody(t, 0, Forms, A1, isFallbackForm));

			if (spec.first) {
				t.GetRep().SuccessForm(LogTrace, GetLabel(), A1.result.Fix(Type::GenerateNoRules), spec.second.Fix(Type::GenerateNoRules));
				shouldInline = isFallbackForm || ShouldInline(spec.first);
			}

			if (cache && fixed && spec.second.IsFixed()) {
				cache->emplace(key, std::make_tuple(spec.first, spec.second, shouldInline, isFallbackForm));
			} 

			return CompleteFunctionCall(label, spec, A1.result, isFallbackForm ? Pair::New(A0.node, A1.node) : A1.node, 
										shouldInline, MemoryRegion::GetCurrentRegion());
		}

		static bool ShouldTryArgumentEvolution(const Type& outer, const Type& inner) {
			if (outer == inner) return true;
			// some scalar evolutions for invariants are supported
			if (outer.IsInvariant() && inner.IsInvariant()) return true;

			// some homogenic tuple sections are supported
			if (inner.IsPair() && outer.IsPair()) {
				auto innerCount = inner.CountLeadingElements(inner.First());
				auto outerCount = outer.CountLeadingElements(outer.First());
				if (inner.First() == outer.First() &&
					inner.Rest(innerCount) == outer.Rest(outerCount)) {
					return true;
				}
				return ShouldTryArgumentEvolution(inner.First(), outer.First())
					&& ShouldTryArgumentEvolution(inner.Rest(), outer.Rest());
			}
			// evolution inside user types are sometimes supported
			if (inner.IsUserType() && outer.IsUserType() &&
				inner.GetDescriptor() == outer.GetDescriptor()) {
				return ShouldTryArgumentEvolution(outer.UnwrapUserType(), inner.UnwrapUserType());
			}
			return false;
		}

		std::pair<Graph<Typed>, Type> Evaluate::SpecializeBody(SpecializationState& t, const std::vector<Nodes::CGRef> *recurPt, const Qxx::IEnumerable<const Type&>& Forms, const Specialization& A1, bool& outUsedFallback) const {
			outUsedFallback = false;
			t.GetRep().Diagnostic(LogTrace, this, Error::Info, A1.result, "arg");
			for (auto Form : Forms) {
				if (Form.IsGraph()) {
					auto form = t.GetRep().Block(LogWarnings, "form");
					RegionAllocator SpecializationAttemptAllocator;
					Specialization spec;

					if (recurPt) {
						struct _st : public SpecializationTransform {
							const Type Form;
							const std::vector<CGRef> *const recurPt;
							_st(const Type& Form, const Type& argType, SpecializationDiagnostic& rep, const std::vector<CGRef> *recurPt,
								SpecializationState::Mode m)
								:SpecializationTransform(Form.GetGraph(), argType, rep, m), Form(Form), recurPt(recurPt) {}
							Specialization operate(CGRef src) {
								for (auto r : *recurPt) {
									if (r == src) {
										auto upstream = (*this)(r->GetUp(1));


										// should we try the recursion solver?
										if (ShouldTryArgumentEvolution(
											GetArgumentType().Fix(Type::GenerateNoRules),
											upstream.result.Fix(Type::GenerateNoRules))) {
											if (upstream.node == nullptr) return upstream;

											if (GetMap().HasQueuedPostProcessing()) {
												GetMap().PostProcess(Specialization(nullptr, Type::Nil));
											}
											// store cache
											return Specialization(0, Type::User(&RecursionTrap,
																				Type::Tuple(Type(r),
																							Form,
																							upstream.result,
																							Type(new SpecializationTransformHolder(GetMap(), upstream.node)),
																							Type::Nil)));
										}
									}
								}
								return SpecializationTransform::operate(src);
							}
						} specifiedTransform(Form,
											 A1.result,
											 t.GetRep(), recurPt, t.mode);

						spec = specifiedTransform.Go();
					} else {
						SpecializationTransform SpecXFM(Form.GetGraph(), A1.result, t.GetRep(), t.mode);
						spec = SpecXFM.Go();
					}

					if (spec.node == 0) {
						// form selection on specialization failure, propagation otherwise
						if (spec.result.GetDescriptor() != &K3::SpecializationFailure) {
							if (spec.result.GetDescriptor() == &PropagateFailure) {
								t.GetRep().Diagnostic(LogTrace, this, Error::TypeMismatchInSpecialization, 
													  "No match");
								return { spec.node, Type(&K3::SpecializationFailure) };
							}

							if (spec.result.GetDescriptor() == &RecursionTrap) {
								return { spec.node, spec.result };
							}

							if (spec.result.GetDescriptor() == &NoEvalFallback) {
								// fail all forms but return specialization failure to parent
								return make_pair(spec.node, Type(&K3::SpecializationFailure));
							}

							if (t.GetRep().IsActive()) {
								stringstream errortext;
								errortext << spec.result;
								t.GetRep().Diagnostic(LogErrors, this, Error::SpecializationFailed, A1.result, errortext.str().c_str());
							}

							auto s = t.GetRep().TypeError(label, A1.result, spec.result);

							return make_pair(s.node, s.result);
						}
					} else {
#ifndef NDEBUG
						if (TLS::GetCurrentInstance()->ShouldTrace("Derive", label)) {
							std::clog << "Derive: [" << label << "]\n";
							std::clog << *Form.GetGraph() << "\n" << A1.result << " -> " << spec.result << "\n\n";
						}
#endif
						return make_pair(spec.node, spec.result);
					}
				} else if (Form.IsUserType()) {
					CGRef extEval = TLS::ResolveSymbol(":Fallback:Eval");
					if (extEval) {
						Evaluate tmp(":Fallback:Eval", extEval, GenericArgument::New());
						SpecializationTransform	 fallback(&tmp, Type::Pair(Form, A1.result, true), t.GetRep(), t.mode);
						auto s(tmp.Specialize(fallback));
						outUsedFallback = true;
						return std::make_pair(s.node, s.result);
					}

					// type mismatch in graph
					t.GetRep().Diagnostic(LogErrors, this, Error::EmptyGraph, "Not a function");
					auto s(TypeError(&FatalFailure));
					return std::make_pair(s.node, s.result);
				}
			}

//			t.GetRep().Diagnostic(LogErrors, this, Error::SpecializationFailed, A1.result, "No valid forms");
			auto s = SpecializationFailure(); // t.GetRep().TypeError(label, A1.result);
			return std::make_pair(s.node, s.result);
		}

		FunctionCall::FunctionCall(const char *label, Graph<Typed> body, const Type& arg, const Type& result, CTRef up)
			:FunctionBase(up), body(body), label(label), resultType(result), argumentType(arg) {
		}

		CTRef FunctionCall::IdentityTransform(GraphTransform<const Typed, CTRef> &copy) const {
			return FunctionBase::IdentityTransform(copy);
		}

		unsigned FunctionCall::ComputeLocalHash() const {
			size_t h(FunctionBase::ComputeLocalHash());
			HASHER(h, body->GetHash(true));
			return (unsigned)h;
		}

		int FunctionBase::LocalCompare(const ImmutableNode& rhs) const {
			auto& r((const FunctionBase&)rhs);
			int t(ArgumentType().OrdinalCompare(r.ArgumentType()));
			if (t) return t;
			else return DisposableTypedUnary::LocalCompare(rhs);
		}

		int FunctionCall::LocalCompare(const ImmutableNode& rhs) const {
			auto& r((const FunctionCall&)rhs);
			int t(FunctionBase::LocalCompare(rhs));
			if (t) return t;
			return body->Compare(*r.body);
		}

		struct IncrementalResultFormulaGenerator : public Transform::Identity<const Generic> {
			IncrementalResultFormulaGenerator(CGRef root, int delta = 0) :delta(delta), Identity(root) {}
			int delta;
			CGRef operator()(CGRef node) {
				if (IsOfExactType<GenericArgument>(node)) return Invariant::Add(node, Invariant::Constant::New(delta));
				else return node->IdentityTransform(*this);
			}
		};

	};
};
