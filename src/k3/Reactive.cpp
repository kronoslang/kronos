#include "common/PlatformUtils.h"
#include "common/Graphviz.h"
#include "backends/DriverSignature.h"
#include "Reactive.h"
#include "UserErrors.h"
#include "TypeAlgebra.h"
#include "TLS.h"
#include "Stateful.h"
#include "FlowControl.h"
#include "Evaluate.h"
#include "Native.h"
#include "CompilerNodes.h"
#include "DynamicVariables.h"
#include "Invariant.h"
#include "NativeVector.h"
#include "UserErrors.h"
#include "kronos.h"
#include <cmath>
#include <algorithm>
#include <memory>

#include "config/system.h"
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif


static void OutputDriver(std::ostream& strm, K3::DriverSignature dsig) {
	strm << dsig.GetMetadata( );
	if (dsig.GetRatio( ) != 1.0) {
		strm << " @ " << dsig.GetMul( ) << "/" << dsig.GetDiv( );
	}
}

namespace K3 {
	namespace Nodes{
		using namespace K3::Reactive;

		template <typename T> static bool Equal(const T& a, const T& b) {return a == b;}
		static bool Negate(bool t) {return !t;}
		static bool IsTrue(bool t) {return t;}

		static CTRef GetBody(CTRef r) {return r;}
		static const Node* GetReactivity(CTRef r) {return r->GetReactivity();} 

#pragma region Reactive Analysis Subroutines
		const Node* Typed::ReactiveAnalyze(Reactive::Analysis& t, const Node** upRx) const {
			const Node** rxArray = (const Node**)alloca(sizeof(const Node*)*GetNumCons( ));
			int numRx(0);
			for (unsigned i = 0; i < GetNumCons( ); ++i) {
				if (upRx[i]) {
					rxArray[numRx++] = upRx[i];
				}
			}

			if (numRx == 0) return t.GetLeafReactivity( );

			auto begin = rxArray, end = rxArray + numRx;

			if (Qxx::From(begin, end)
				.Where([](const Node* r) { return r->IsFused( ) == false; })
				.Any( ) ||
				Qxx::From(begin, end)
				.Aggregate([](const Node* a, const Node *b) { return a == b ? a : 0; }) == 0) {

				// merge
				DriverSet ds;
				for (auto n : Qxx::From(begin, end)) {
					for (auto dn : Qxx::FromGraph(n).OfType<DriverNode>( )) {
						ds.Merge(t.GetDelegate( ), dn->GetID( ) );
					}
				}

				auto newReact(t.Memoize(ds));
				return newReact;
			} else {
				// reuse upstream reactivity
				return *begin;
			}
		}

		CTRef Typed::ReactiveReconstruct(Analysis& t) const {
			Typed* newCopy = ConstructShallowCopy();
			auto rx = t.ReactivityOf(this);
			newCopy->SetReactivity(rx);
			for (unsigned i(0); i < newCopy->GetNumCons( ); ++i) {
				newCopy->Reconnect(i, t.Boundary(t(GetUp(i)), rx, t.ReactivityOf(GetUp(i))));
			}
			return newCopy;
		}

		const Node* Argument::ReactiveAnalyze(Analysis& t, const Node**) const {
			return t.GetArgumentReactivity( );
		}

		CRRef Pair::ReactiveAnalyze(Analysis& t, CRRef* upRx) const {
			if (upRx[0]->Compare(*upRx[1]) == 0) return upRx[0];
			IFixedResultType *ifr;
			GetUp(1)->Cast(ifr);
			if (upRx[0]->IsFused() && (Typed::IsNil(GetUp(1)) || (ifr && ifr->FixedResult().GetSize() == 0))) {
				// 'rest' reactivity is inconsequential; simplify
				return upRx[0];
			}

			GetUp(0)->Cast(ifr);
			if (upRx[1]->IsFused() && (Typed::IsNil(GetUp(0)) || (ifr && ifr->FixedResult().GetSize() == 0))) {
				// 'rest' reactivity is inconsequential; simplify
				return upRx[1];
			}
			return new LazyPair(upRx[0], upRx[1]);
		}
	
		CTRef Pair::ReactiveReconstruct(Analysis& t) const { 
			// no boundaries at pair nodes
			auto p(New(t(GetUp(0)), t(GetUp(1))));
			const_cast<Typed*>(p)->SetReactivity(t.ReactivityOf(this));
			return p;
		}

		const Node* First::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			return upRx[0]->First( );
		}

		CTRef First::ReactiveReconstruct(Analysis& t) const {
			auto cpy = IdentityTransform(t);
			const_cast<Typed*>(cpy)->SetReactivity(t.ReactivityOf(this));
			return cpy;
		}

		const Node* Rest::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			return upRx[0]->Rest( );
		}

		CTRef Rest::ReactiveReconstruct(Analysis& t) const {
			auto cpy = IdentityTransform(t);
			const_cast<Typed*>(cpy)->SetReactivity(t.ReactivityOf(this));
			return cpy;
		}

		const Node* Deps::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			if (upRx[0]) return upRx[0];
			else return nullptr;
		}

		CTRef Deps::ReactiveReconstruct(Analysis& t) const {
			auto cpy = IdentityTransform(t);
			const_cast<Typed*>(cpy)->SetReactivity(t.ReactivityOf(this));
			return cpy;
		}

		const Node* RingBuffer::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			DriverSet ds;
			for (auto dn : Qxx::FromGraph(upRx[1]).OfType<DriverNode>( )) {
				ds.Merge(t.GetDelegate( ), dn->GetID( ));
			}
			return t.Memoize(ds);
		}


		CTRef RingBuffer::ReactiveReconstruct(Analysis& t) const {
			auto cpy = IdentityTransform(t);
			const_cast<Typed*>(cpy)->SetReactivity(t.ReactivityOf(GetUp(1)));
			// SideEffectTransform spoofs additional connections to break cycles
			GetUp(1)->HasInvisibleConnections();
			return cpy;
		}

		static void ReserveOutputSignalMasks(const Node* output, Analysis& a) {
			for(auto dn : Qxx::FromGraph(output).OfType<DriverNode>()) {
				DriverSignature dsig = dn->GetID();
				for(auto m : dsig.Masks()) {
					a.ReserveSignalMask(m);
				}
			}
		}

		const Node* FunctionCall::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			if (t.GetDataflowNode(this)) return t.GetDataflowNode(this)->GetReactivity( );
			if (upRx[0]) {
				auto fc = ConstructShallowCopy();
#ifndef NDEBUG
				if (TLS::GetCurrentInstance()->ShouldTrace("RX", GetLabel())) {
					std::clog << "RX: [" << GetLabel() << "]\n" << *body << "\n\n";
				}
#endif
				Analysis subRoutine(body, t, t.GetDelegate( ), upRx[0], t.GetLeafReactivity( ), t.GetNullReactivity( ));
				CRRef rx;
				fc->body = TLS::WithNewStack([&]() { return subRoutine.Go(rx);} );
				t.AddDataflowNode(this, fc);
				ReserveOutputSignalMasks(rx, t);
				fc->SetReactivity(rx); 	
				fc->SetArgumentReactivity(upRx[0]);
				return rx;
			} else {
				return nullptr;
			}
		}

		CTRef FunctionBase::ReactiveReconstruct(Analysis& t) const {
			Typed* cpy, *nxt = t.GetDataflowNode(this);
			do {
				cpy = nxt;
				nxt = t.GetDataflowNode(nxt);
			} while (nxt);

			cpy->Reconnect(0, t(cpy->GetUp(0)));
			cpy->SetReactivity(t.ReactivityOf(this));
			return cpy;
		}
	
		const Node* FunctionSequence::ReactiveAnalyze(Analysis& t, const Node** upRx) const {
			if (t.GetDataflowNode(this)) return t.GetDataflowNode(this)->GetReactivity( );

			if (upRx[0]) {
				auto fs = ConstructShallowCopy( );
				Analysis sequenceBodyAnalysis(fs->iterator, t, t.GetDelegate( ), upRx[0], t.GetLeafReactivity( ), t.GetNullReactivity( ));
				CRRef iterRx(nullptr);
				auto iter(sequenceBodyAnalysis.Go(iterRx));

				if (fs->GetRepeatCount( ) > 1 && iterRx->Compare(*upRx[0])) {
					fs->SplitSequenceAt(1);
					t.AddDataflowNode(this, fs);
					return fs->ReactiveAnalyze(t, upRx);
				}

				Analysis tailAnalysis(fs->tailContinuation, t, t.GetDelegate( ), iterRx, t.GetLeafReactivity( ), t.GetNullReactivity( ));
				CRRef tailRx(nullptr);
				CTRef tail(tailAnalysis.Go(tailRx));

				sequenceBodyAnalysis.SetArgumentReactivity(new LazyPair(
					upRx[0], tailRx));

				sequenceBodyAnalysis.Rebase(fs->generator);
				CRRef geneRx(nullptr);
				auto gene(sequenceBodyAnalysis.Go(geneRx));
				if (fs->GetRepeatCount( ) > 1 && geneRx->Compare(*tailRx)) {
					fs->SplitSequenceAt(check_cast<unsigned>(fs->GetRepeatCount( ) / 2));
					t.AddDataflowNode(this, fs);
					return TLS::WithNewStack([&]() {return fs->ReactiveAnalyze(t, upRx);});
				}

				fs->iterator = iter;
				fs->generator = gene;
				fs->tailContinuation = tail;

				ReserveOutputSignalMasks(geneRx, t);
				fs->SetArgumentReactivity(upRx[0]);
				fs->SetReactivity(geneRx);


				t.AddDataflowNode(this, fs);
				return geneRx;
			} else return nullptr;
		}


		const Node* SetGlobalVariable::ReactiveAnalyze(Analysis& a, const Node** upFx) const {
			a.GetDelegate( ).SetGlobalVariableReactivity(uid, upFx[0]);
			return upFx[0];
		}

		CTRef SetGlobalVariable::ReactiveReconstruct(Analysis& a) const {
			auto cpy = IdentityTransform(a);
			const_cast<Typed*>(cpy)->SetReactivity(a.ReactivityOf(GetUp(0)));
			return cpy;
		}

		const Node* GetGlobalVariable::ReactiveAnalyze(Analysis& a, const Node** upFx) const {
			auto r(a.GetDelegate( ).GetGlobalVariableReactivity(uid));
			if (r == 0) r = a.GetNullReactivity( );
			return r;
		}

		CTRef GetGlobalVariable::ReactiveReconstruct(Analysis& a) const {
			auto cpy(IdentityTransform(a));
			const_cast<Typed*>(cpy)->SetReactivity(a.ReactivityOf(this));
			return cpy;
		}

		//void StreamOutput::ReactiveFinalize(Analysis& a, Typed *t) const {
		//	StreamOutput *so = (StreamOutput*)t;
		//	Typed::ReactiveFinalize(a, so);
		//	Type signatures;
		//	for (auto dn : Qxx::FromGraph(so->GetReactivity()).OfType<DriverNode>()) {
		//		if (signatures.IsNil()) signatures = dn->GetID();
		//		else signatures = Type::Pair(dn->GetID(), signatures);
		//	}
		//	so->callback = TLS::GetCurrentInstance()->SetExternalStreamParameters(key, data, signatures, so->handlerData);
		//}

#pragma endregion 
				
		namespace ReactiveOperators{
			Specialization GenericTick::Specialize(SpecializationState& spec) const {
				SPECIALIZE_ARGS(spec,0);
				if (A0.result.IsPair() == false) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors,this,Error::BadDefinition,"Tick requires clock priority and an identifier");
					return spec.GetRep().TypeError("Tick", A0.result);
				}
				return Specialization(Tick::New(A0.result.Fix()),Type::Float32);
			}

			const Node* Tick::ReactiveAnalyze(Analysis& state, const Node** upFx) const {
				DriverSet ds;
				Type clockId = DriverSignature(Identifier.Rest( ), Identifier.First( ));
				ds.Merge(state.GetDelegate( ), clockId);
				state.GetDelegate( ).RegisterDriver(clockId, 1.0, 1.0);
				return state.Memoize(ds);
			}

			CTRef Tick::ReactiveReconstruct(Analysis& state) const {
				auto cpy(Native::Constant::New(0.f));
				cpy->SetReactivity(state.ReactivityOf(this));
				return cpy;
			}

			void Tick::Output(std::ostream& strm) const {
				strm << "Tick<" << Identifier << ">";
			}


			Specialization GenericImpose::Specialize(SpecializationState& spec) const {
				SPECIALIZE_ARGS(spec,0,1);
				if (spec.mode == SpecializationState::Configuration) {
					return A1;
				}
				return Specialization(Impose::New(A0.node,A1.node),A1.result.Fix());
			}

			const Node* Impose::ReactiveAnalyze(Analysis& state, const Node** upRx) const {
				if (upRx[0] == nullptr) return nullptr;
				// merge
				DriverSet ds;
				for (auto dn : Qxx::FromGraph(upRx[0]).OfType<DriverNode>()) {
					ds.Merge(state.GetDelegate(), dn->GetID());
				}

				auto newReact(state.Memoize(ds));
				return newReact;
			}

			CTRef Impose::ReactiveReconstruct(Analysis& state) const {
				auto up = state(GetUp(1));
				return state.Boundary(up, state.ReactivityOf(this), up->GetReactivity());
			}

			CTRef Impose::GraphFirst() const {
				return New(GetUp(0),GetUp(1)->GraphFirst());
			}

			CTRef Impose::GraphRest() const {
				return New(GetUp(0),GetUp(1)->GraphRest());
			}

			Specialization GenericRateChange::Specialize(SpecializationState& spec) const {
				SPECIALIZE_ARGS(spec,0,1);
				if (A0.result.IsInvariant()) {
					return Specialization(RateChange::New(A0.result.GetInvariant(),A1.node),A1.result.Fix());
				} else {
					spec.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,A0.result.Fix(Type::GenerateNoRules),Type::InvariantI64(1),"Downsampling factor must be an invariant constant");
					return SpecializationFailure();
				}
			}

			template<typename INT> static INT GCD(INT a, INT b) {
				while(true) {
					a = a % b;
					if (a) {
						b = b % a;
						if (b == 0) return a;
					}
					else return b;
				}
			}

			CTRef RateChange::ReactiveReconstruct(Analysis& st) const {
				auto up = st(GetUp(0));
				return st.Boundary(up, st.ReactivityOf(this), up->GetReactivity());
			}

			const Node* RateChange::ReactiveAnalyze(Analysis& st, const Node** upRx) const {
				DriverSet drivers;
				
				Qxx::FromGraph(upRx[0]).OfType<DriverNode>().Select([&](const DriverNode* dn) {
					drivers.Merge(st.GetDelegate(),dn->GetID());
					return 0;
				}).Now();

				drivers.transform([&](const Type& t) -> Type {
					DriverSignature dsig(t);
					if (dsig.GetDriverClass() == DriverSignature::User) {
						double mul,div;
						dsig.GetMultiplier(mul,div);
						if (factor > 0.0) mul*=factor;
						if (factor < -0.0) div*=-factor;

						int64_t n((int64_t)floor(mul+0.5));
						int64_t d((int64_t)floor(div+0.5));
						auto gcd = GCD(n, d);
						n /= gcd;
						d /= gcd;

						dsig.SetMultiplier(static_cast<long double>(n),static_cast<long double>(d));		
						Type newDriver(dsig);
						st.GetDelegate().RegisterDriver(newDriver,(long double)n,(long double)d);
						return newDriver;
					}
					else return t;
				});
				return st.Memoize(drivers);
			}			

			Specialization GenericAdjustPriority::Specialize(SpecializationTransform& spec) const {
				SPECIALIZE_ARGS(spec, 0, 1);
				if (spec.mode == SpecializationState::Configuration) {
					return A0;
				}
				return Specialization(RelativePriority::New(A0.node, A1.node, relative), A0.result);
			}

			const Node* ClockEdge::ReactiveAnalyze(Analysis& rx, const Node** upRx) const {
				return rx.GetLeafReactivity();
			}

			CTRef ClockEdge::ReactiveReconstruct(Analysis& rx) const {
				auto ce = ClockEdge::New(Typed::Nil());
				ce->SetReactivity(rx.GetLeafReactivity());
				ce->SetClock(rx.ReactivityOf(GetUp(0)));
				return ce;
			}

			Specialization GenericClockEdge::Specialize(SpecializationTransform& spec) const {
				SPECIALIZE_ARGS(spec, 0);
				return Specialization(ClockEdge::New(A0.node), Type::Float32);
			}

			Specialization GenericMerge::Specialize(SpecializationTransform& spec) const {
				SPECIALIZE_ARGS(spec,0);
				Type a(A0.result.Fix());
				Type r(a);
				Merge* m(Merge::New(r.IsPair()?r.First():r));
				CTRef up(A0.node);
				while(r.IsPair()) {
					if (r.First() == a.First()) {
						m->Connect(up->GraphFirst());
						up = up->GraphRest();
						r = r.Rest();
					} else {

						spec.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,r.First(),a.First(),"Can only merge a homogenous tuple");
						return SpecializationFailure();
					}
				}

				if (spec.mode == SpecializationState::Configuration) {
					return Specialization(m->GetUp(0), a);
				}

				if (r.IsNil() && m->GetNumCons()) {
					return Specialization(m,a.First());
				} else if (r == a.First()) {
					m->Connect(up);
					return Specialization(m,a.First());
				} else {
					spec.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,r.First(),a.First(),"Can only merge a homogenous tuple");
					return SpecializationFailure();
				}
			}

			Specialization GenericGate::Specialize(SpecializationState& spec) const
			{
				SPECIALIZE_ARGS(spec,0,1);
				
				if (spec.mode == SpecializationState::Configuration) {
					return A1;
				}

				if (A0.result.IsNativeType() ) {
					return Specialization(Gate::New(A0.node,A1.node),A1.result.Fix());
				} else {
					spec.GetRep().Diagnostic(Verbosity::LogErrors,this,Error::TypeMismatchInSpecialization,"Gate switch signal must be a simple native type");
					return SpecializationFailure();
				}
			}

			CTRef Gate::ReactiveReconstruct(Analysis &st) const {
				auto m = Deps::New( );
				int bitIdx = st.GetSignalMask(GetUp(0));
				m->SetReactivity(st.ReactivityOf(this));
				m->Connect(st.Boundary(st(GetUp(1)), st.ReactivityOf(GetUp(1)), m->GetReactivity( )));
				auto setter = SignalMaskSetter::New(st(GetUp(0)), bitIdx);
				setter->SetReactivity(st.ReactivityOf(GetUp(0)));
				m->Connect(setter);
				return m;
			}

			const Node* Gate::ReactiveAnalyze(Analysis& st, const Node** upRx) const {
				if (upRx[0] == nullptr) return upRx[1];
				if (upRx[1] == nullptr) return upRx[0];

				DriverSet drivers;
				
				Qxx::FromGraph(upRx[1]).OfType<DriverNode>().Select([&](const DriverNode* dn) {
					drivers.Merge(st.GetDelegate(),dn->GetID());
					return 0;
				}).Now();

				int bitIdx = st.GetSignalMask(GetUp(0));

				drivers.transform([&](const Type& t) -> Type {
					DriverSignature sig(t);
					if (sig.GetDriverClass() == DriverSignature::User) {
						if (t.IsUserType(ReactiveDriver)) {
							sig.Masks().push_back(bitIdx);
							return sig;
						}
					}
					return t;
				});
				return st.Memoize(drivers);
			}

			CTRef RelativePriority::ReactiveReconstruct(Reactive::Analysis& rx) const {
				auto up = rx(GetUp(0));
				return rx.Boundary(up, rx.ReactivityOf(this), up->GetReactivity());
			}

			const Node* RelativePriority::ReactiveAnalyze(Reactive::Analysis& rx, const Node** upRx) const {
				// make every driver priority in sig (0) match the highest driver in second arg
				Type maxPriority, minPriority;
				DriverSet signatures;
				for (auto pdn : Qxx::FromGraph(upRx[1]).OfType<DriverNode>()) {
					DriverSignature dsgn(pdn->GetID());
					if (dsgn.GetDriverClass() != DriverSignature::Recursive) {
						signatures.insert(pdn->GetID());
						if (maxPriority.IsNil() || dsgn.GetPriority() > maxPriority) maxPriority = dsgn.GetPriority();
						if (minPriority.IsNil() || dsgn.GetPriority() < minPriority) minPriority = dsgn.GetPriority();
					}
				}

				Type newPriority;

				switch (opcode) {
					case GenericAdjustPriority::Abdicate:
						newPriority = minPriority - Type::InvariantI64(1); break;
					case GenericAdjustPriority::Supercede:
						newPriority = maxPriority + Type::InvariantI64(1); break;
					case GenericAdjustPriority::Cohabit:
						newPriority = minPriority; break;
					case GenericAdjustPriority::Share:
						newPriority = maxPriority; break;
				}

				signatures.transform([newPriority](Type& t) -> Type {
					DriverSignature dsgn{ t };
					dsgn.SetPriority(newPriority);
					return dsgn;
				});

				return rx.Memoize(signatures);
			}

			CTRef Merge::ReactiveReconstruct(Analysis& t) const {
				auto newCopy = ConstructShallowCopy();
				for (int i = 0;i < GetNumCons();++i) {
					// merge never needs boundaries
					newCopy->Reconnect(i, t(GetUp(i)));
					newCopy->upRx.push_back(t.ReactivityOf(GetUp(i)));
				}
				newCopy->SetReactivity(t.ReactivityOf(this));
				return newCopy;
			}

			Specialization GenericRate::Specialize(SpecializationTransform& spec) const {
				SPECIALIZE_ARGS(spec,0);
				if (spec.mode == SpecializationState::Configuration) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::BadDefinition, "Can not interrogate reactive clock rates in configurator expressions");
					return spec.GetRep().TypeError("Rate", A0.result);
				}
				return Specialization(BaseRate::New(A0.node),Type::Float32);
			}

			static int64_t GCD(int64_t a, int64_t b) {
				while(true) {				
					a = a % b;
					if (a) {
						b = b % a;
						if (b == 0) return a;
					} else return b;
				}
			}
				

			template <typename ITER> float CountTicks(ITER beg, ITER end) {
				int64_t total_mul = Qxx::From(beg,end)
										.Select([](pair<int,int> a) { return int64_t(a.first); } )
										.Aggregate([](int64_t a, int64_t b) {
					return a * b / GCD(a,b);
				});

				assert((uint64_t)total_mul < numeric_limits<size_t>::max());
				vector<int> activation((size_t)total_mul);

				for(auto mul(beg);mul!=end;++mul) {
					for(int64_t i(0);i<total_mul;i+=(total_mul / mul->first)) {
						if (activation[i]) activation[i] = min(mul->second,activation[i]);
						else activation[i] = mul->second;
					}
				}

				double rate(0);
				for(int distance : activation) {
					if (distance > 0) {
						rate += (double)total_mul / (double)distance;
					}
				}

				return (float)rate;
			}

			const Node* BaseRate::ReactiveAnalyze(Reactive::Analysis& a, const Node** upRx) const {
				if (upRx[0] == nullptr) return nullptr;

				if ((TLS::GetCurrentFlags() & Kronos::BuildFlags::DynamicRateSupport) != 0) {
					DriverSet rateReactivity;

					for (auto drv : Qxx::FromGraph(upRx[0])
						 .OfType<DriverNode>()
						 .Select([](const DriverNode* dn) { return dn->GetID(); })
						 .Where([](const Type& t) { return t.IsUserType(Reactive::ReactiveDriver); })) {

						DriverSignature sig(drv);

						if (sig.GetDriverClass() == DriverSignature::User) {
							DriverSignature rateSig(Type::User(&ReactiveRateTag, sig.GetMetadata()), Type(0));
							Type derivedId = rateSig;
							a.GetDelegate().RegisterDriver(derivedId, 1, 1);
							rateReactivity.Merge(a.GetDelegate(), derivedId);
						}
					}

					return a.Memoize(rateReactivity);
				} else {
					DriverSet once;
					once.insert(DriverSignature{ Type("init"), Type::InvariantI64(-1) });
					return a.Memoize( once );
				}
			}

			CTRef BaseRate::ReactiveReconstruct(Analysis& a) const {
				Typed* maxRate = nullptr;

				for (auto d : Qxx::FromGraph(a.ReactivityOf(GetUp(0))).OfType<DriverNode>( )) {
					DriverSignature sig(d->GetID( ));
					if (sig.GetDriverClass( ) == DriverSignature::User) {
						auto rateSig = Type::User(&ReactiveRateTag, sig.GetMetadata());
						Typed* extRate = GetGlobalVariable::New(
							TLS::GetCurrentInstance()->Memoize(rateSig),
							Type::Float32,
							rateSig,
							std::make_pair(1, 1),
							nullptr,
							(TLS::GetCurrentFlags() & Kronos::DynamicRateSupport) != 0 
							? External 
							: Configuration);

						double ratio = sig.GetRatio( );
						if (ratio != 1.0) extRate = Native::MakeFloat("mul", Native::Mul, extRate, Native::Constant::New((float)ratio));
						extRate->SetReactivity(a.ReactivityOf(this));

						if (maxRate) maxRate = Native::MakeFloat("max", Native::Max, extRate, maxRate);
						else maxRate = extRate;

						maxRate->SetReactivity(a.ReactivityOf(this));
					}
				}

				if (maxRate == nullptr) {
					maxRate = Native::Constant::New(0.f);
					maxRate->SetReactivity(a.ReactivityOf(this));
				}

				return maxRate;
			}
		};

		void Boundary::Output(std::ostream& strm) const {
			strm<<"B<";
			if (GetReactivity( )) {
				for (auto d : Qxx::FromGraph(GetReactivity( )).OfType<Reactive::DriverNode>( )) {
					OutputDriver(strm, d->GetID( ));
				}
			} else strm << "nil";
			strm << " <- ";
			if (upstreamReactivity) {
				for (auto d : Qxx::FromGraph(upstreamReactivity).OfType<Reactive::DriverNode>( )) {
					OutputDriver(strm, d->GetID( ));
				}
			} else strm << "nil";
			strm << ">";
		}
	};

	namespace Reactive {
		void RecursiveClockCompletion::Add(FusedSet* fs) {
			if (find(begin( ), end( ), fs) == end( )) push_back(fs);
		}

		void RecursiveClockCompletion::Commit(IDelegate& d, const Node* newRx, void *tagId) {
			for (auto fs : *this) {
					for (unsigned i(0); i < fs->GetNumCons(); ++i) {
                    bool replaceCon = true;
					DriverNode *dn;
					if (fs->GetUp(i)->Cast(dn)) {
						DriverSignature dsig(dn->GetID());
						if (dsig.GetDriverClass() == DriverSignature::Recursive 
							&& dsig.GetMetadata().GetInternalTag() == tagId) {
							bool anyRxDrivers = false;
							for (auto dn : Qxx::FromGraph(newRx).OfType<DriverNode>()) {
								anyRxDrivers = true;
								if (replaceCon) {
									fs->Reconnect(i, dn);
									replaceCon = false;
								} else {
									fs->Connect(dn);
								}
							}
							if (anyRxDrivers == false) {
								if (replaceCon) fs->Reconnect(i, newRx);
								else fs->Connect(newRx);
								replaceCon = false;
							}
						}
					}
				}
                fs->Canonicalize(d);
			}
#ifndef NDEBUG
			clear( );
#endif
		}

		DriverNode::DriverNode(const Type& driver) :DriverId(driver.Fix(Type::GenerateRulesForAllTypes)) { 
		}

		void DriverNode::Output(std::ostream& strm) const {
			DriverSignature dsig(DriverId);
			OutputDriver(strm, dsig);
		}


		Analysis::Analysis(CTRef root, Analysis& used, IDelegate& del, const Node* arg, const Node* leaf, const Node* nul)
			:del(del),arg(arg),leaf(leaf),noReact(nul),CachedTransform(root) {
			for(auto &mask : used.signalMaskIndices) {
				signalMaskIndices.push_back(make_pair(nullptr, mask.second));
			}
		}
	
		Analysis::Analysis(CTRef root, IDelegate& del, const Node* arg, const Node* leaf, const Node* nul)
			:del(del),arg(arg),leaf(leaf),noReact(nul),CachedTransform(root) { }

		int Analysis::GetSignalMask(CTRef expr) {
			int unusedId = 0;
			bool checkAgain;
			do {
				checkAgain = false;
				for(auto &m : signalMaskIndices) {
					if (expr && *expr == *m.first) return m.second;
					else if (unusedId == m.second) {
						unusedId++;
						checkAgain = true;
					}
				}
			} while(checkAgain);
			signalMaskIndices.push_back(make_pair(expr,unusedId));
			GetDelegate().RegisterSignalMaskSlot(unusedId);
			return unusedId;
		}

		void Analysis::ReserveSignalMask(int id) {
			for(auto &m : signalMaskIndices) {
				if (m.second == id) return;
			}
			signalMaskIndices.push_back(make_pair(nullptr,id));
		}

		TypeDescriptor ReactiveDriver("Spring",false);
		TypeDescriptor AllDrivers("All",false);
		TypeDescriptor ArgumentDriver("Arg",false);
		TypeDescriptor SizingDriver("SizeOf",false);
		TypeDescriptor InitializationDriver("Init",false);
		TypeDescriptor NullDriver("Null",false);
		TypeDescriptor RecursiveDriver("RecursiveDriver",false);

		static int CompareDrivers(const IDelegate& d, const Type& driver1, const Type& driver2) {
			int result = d.OrdinalCompare(driver1, driver2);
			if (result) {
				bool recursive = false;
				if (driver1.IsUserType( ) && driver1.GetDescriptor( ) == &RecursiveDriver) {
					// if this driver is recursive, collect the drivers it was compared against
					DriverSignature dsig(driver1);
					auto completion = (RecursiveClockCompletion*)dsig.GetMetadata( ).GetInternalTag( );
					completion->LoopDriverSet.Merge(d, driver2);
					recursive = true;
				}
				if (driver2.IsUserType( ) && driver2.GetDescriptor( ) == &RecursiveDriver) {
					// if this driver is recursive, collect the drivers it was compared against
					DriverSignature dsig(driver2);
					auto completion = (RecursiveClockCompletion*)dsig.GetMetadata( ).GetInternalTag( );
					completion->LoopDriverSet.Merge(d, driver1);
					recursive = true;
				}
				if (recursive) return 0;
			}
			return result;
		}

		void DriverSet::Merge(const IDelegate& d, const Type& driverId) {
			if (size() == 0) {
				insert(driverId);
				return;
			}

			/* optimize for no changes */
			if (Qxx::From(GetEnumerator()).Select([&](const Type& driver) {
					if (driver == driverId) return true;
					return CompareDrivers(d,driverId,driver) < 0;
				}).Where(IsTrue).Any()) return;

			Sml::Set<Type> overrides;
			for_each([&](const Type& in_set) {
				if (CompareDrivers(d, in_set, driverId) < 0 && overrides.find(in_set) == 0) {
					overrides.insert(in_set);
				}
			});

			// plant the new driver
			insert(driverId);

			if (overrides.size()) {
				Sml::Set<Type> newDrivers;
				for_each([&](const Type& in_set) {
					if (CompareDrivers(d,in_set,driverId) >= 0 &&
						newDrivers.find(in_set) == 0) newDrivers.insert(in_set);
				});
				*this = std::move(newDrivers);
			}
		}

		static bool IsTrivial(CTRef node) {
//			std::cout << *node << " is trivial?\n";

			if (IsOfExactType<Pair>(node) ||
				IsOfExactType<First>(node) ||
				IsOfExactType<Rest>(node) ||
				IsOfExactType<Native::Constant>(node)) {
				for(auto up : node->Upstream()) if (!IsTrivial(up)) return false;
				return true;
			}
			else return false;
		}

		CTRef Analysis::Boundary(CTRef upstream, const Node* ongoing, const Node* upstreamRx) {

			if (upstreamRx == ongoing) {
				return upstream;
			}

			assert(Qxx::FromGraph(ongoing).OfType<LazyPair>().Any() == false);

			DriverSet up, down, low_edge, high_edge;
			
			Qxx::FromGraph(upstreamRx).OfType<DriverNode>().Select([&](const DriverNode* dn) {
				Type upDrv{ dn->GetID() };
				DriverSignature dsgn{ upDrv };

				// ignore priority in boundary generation
				if (dsgn.GetDriverClass() != DriverSignature::Recursive) {
					dsgn.SetPriority(Type::Nil);
					upDrv = dsgn;
				}

				if (up.find(upDrv) == nullptr) up.insert(upDrv);
				return 0;
			}).Now();

			if (up.size( ) == 0) return upstream;


			Qxx::FromGraph(ongoing).OfType<DriverNode>().Select([&](const DriverNode* dn) {
				Type dnDrv{ dn->GetID() };
				DriverSignature dsgn{ dnDrv };
				if (dsgn.GetDriverClass() != DriverSignature::Recursive) {
					dsgn.SetPriority(Type::Nil);
					dnDrv = dsgn;
				}
				down.insert(dnDrv);
				if (up.find(dnDrv) == nullptr) high_edge.insert(dn->GetID());
				return 0;
			}).Now();

			up.for_each([&](const Type& id) {
				if (down.find(id) == nullptr) low_edge.insert(id);
			});

			if ( high_edge.size() > 0 || low_edge.size() > 0 ) {
				auto boundary(Boundary::New(true,upstream,upstreamRx,ongoing));

				auto generated = generatedBoundaries.equal_range(upstream);
				for (auto i = generated.first;i != generated.second;++i) {
					if (*i->second == *boundary) {
						return i->second;
					}
				}

				generatedBoundaries.emplace(upstream, boundary);

				return boundary;
			}
			else return upstream;
		}

		void FusedSet::Canonicalize(IDelegate& withDelegate) {
			DriverSet ds;
			for (auto dn : Qxx::FromGraph(this).OfType<DriverNode>( )) {
				ds.Merge(withDelegate, dn->GetID( ));
			}

			numCons = 0;
			ds.for_each([this](const Type& t) {
				DriverSignature dsig(t);
				if (dsig.GetDriverClass( ) == DriverSignature::Recursive) {
					auto complete = (RecursiveClockCompletion*)dsig.GetMetadata( ).GetInternalTag();
					complete->Add(this);
				}
				this->Connect(new DriverNode(t));
			});
		}

		void FusedSet::Canonicalize( ) {
			std::set<Type> driverIds;
			for (auto dn : Qxx::FromGraph(this).OfType<DriverNode>()) {
				driverIds.insert(dn->GetID( ));
			}

			numCons = 0;
			for (auto d : driverIds) {
				Connect(new DriverNode(d));
			}
		}

		const FusedSet* Analysis::Memoize(const DriverSet& drivers) {
			auto f(memoizedReactivity.find(drivers));
			if (f) return f->second;
			else {
				auto fs(new FusedSet( ));
				drivers.for_each([=](const Type& id) {
					fs->Connect(new DriverNode(id));
					if (id.IsUserType( ) && id.GetDescriptor( ) == &RecursiveDriver) {
						// if this driver is recursive, insert completion record
						DriverSignature dsig(id);
						auto completion = (RecursiveClockCompletion*)dsig.GetMetadata( ).GetInternalTag( );
						completion->Add(fs);
					}
				});
				fs->Canonicalize( );
				return memoizedReactivity.insert(make_pair(drivers,fs)).second;
			}
		}

		void Analysis::AddDataflowNode(CTRef old, Typed* n) {
			FunctionTranslation.insert(std::make_pair(old, n));
		}

		Typed* Analysis::GetDataflowNode(CTRef old)  {
			auto f = FunctionTranslation.find(old);
			if (f != FunctionTranslation.end( )) {
				// kludge
				return (Typed*)(CTRef)f->second;
			}
			return nullptr;
		}

		void Analysis::InvalidateDataflowNode(CTRef old) {
			FunctionTranslation.erase(old);
		}

		CRRef Analysis::ReactivityOf(CTRef old) const {
			return Siblings.find(old)->second;
		}

		const Node* Analysis::DataflowPass(CTRef node, const Analysis::CycleMap* cycles, Analysis::SiblingMap& completed) {
			// is this a recursive path?
			auto cycle = cycles ? cycles->find(node) : nullptr;
			if (cycle != nullptr) {
				// return tentative reactivity
				assert(*cycle);
				if (!**cycle) {
					clockCycles.emplace_back();
					**cycle = &clockCycles.back();
				}
				DriverSet ds;
				auto tag = Type::InternalUse((const void*)**cycle);
				ds.Merge(GetDelegate( ), Type::User(&RecursiveDriver, Type::Tuple(tag, tag, Type::Nil)));
				auto myRx = Memoize(ds);
//				std::clog << "[" << (void*)node << "] ~ " << *myRx << "\n";
				return myRx;
			}

			auto f = completed.find(node);
			if (f != completed.end( )) return f->second;

			if (node->GetNumCons( ) == 0) {
				auto rx = node->ReactiveAnalyze(*this, nullptr);
				completed.insert(std::make_pair(node, rx));
//				std::clog << node->GetLabel() << "[" << (void*)node << "]\n" << *node << " @ " << *rx << "\n";
				return rx;
			}

			int numRxCons = node->GetNumCons();

			const Node** upRx = (const Node**)alloca(sizeof(const Node*) * numRxCons);
			memset(upRx, 0, sizeof(const Node*)*numRxCons);
			RecursiveClockCompletion*  completeClock = nullptr;
			RecursiveClockCompletion** completeClockSetter = &completeClock;
			for (int i(0); i < numRxCons; ++i) {
				auto add_cycle = cycles->insert(node, completeClockSetter);
				upRx[i] = DataflowPass(node->GetUp(i), &add_cycle, completed);
			}

			auto myRx = node->ReactiveAnalyze(*this, upRx);
//			std::clog << node->GetLabel() << "[" << (void*)node << "]\n" <<*node << " @ " << *myRx << "\n";

			if (completeClock) {
				for (auto dn : Qxx::FromGraph(myRx).OfType<DriverNode>( )) {
					DriverSignature dsig(dn->GetID( ));
					if (dsig.GetDriverClass( ) == DriverSignature::Recursive && dsig.GetMetadata( ).GetInternalTag( ) == (void*)completeClock) {
						for (auto dn2 : Qxx::FromGraph(GetLeafReactivity( )).OfType<DriverNode>( )) {
							(completeClock)->LoopDriverSet.Merge(GetDelegate( ), dn2->GetID());
						}
					} else {
						(completeClock)->LoopDriverSet.Merge(GetDelegate( ), dn->GetID( ));
					}
				}

				if ((completeClock)->LoopDriverSet.size() == 0) {
					// no reactive drivers for loop
				}

				myRx = Memoize((completeClock)->LoopDriverSet);
				(completeClock)->Commit(GetDelegate(), myRx, (void*)completeClock);

#ifndef NDEBUG
				memoizedReactivity.for_each([&completeClock](const DriverSet& ds, CRRef rx) {
					if (Qxx::FromGraph(rx).OfType<DriverNode>( ).Where([&completeClock](const DriverNode* dn) {
						DriverSignature dsig(dn->GetID( ));
						if (dsig.GetDriverClass( ) == DriverSignature::Recursive) {
							return dsig.GetMetadata( ) == Type::InternalUse(&completeClock);
						} else return false;
					}).Any()) {
                        std::cerr << rx << " is unpatched\n";
						assert(false && "Incomplete recursions left in the memo tree\n");
					}
				});
#endif
			}


			completed.insert(std::make_pair(node, myRx));
			return myRx;
		}

		CTRef Analysis::Go(CRRef& rx) {
			CTRef temp;

			std::tuple<Graph<Typed>,CRRef> memoized;
			if (GetDelegate().GetMemoized(std::make_tuple(GetRoot(), arg), memoized)) {
				rx = std::get<1>(memoized);
				assert(rx);
				return std::get<0>(memoized);
			}

			rx = DataflowPass(GetRoot( ), CycleMap::empty( ), Siblings);
			temp = CachedTransform::Go();

			GetDelegate().SetMemoized(std::make_tuple( GetRoot(),arg ), std::make_tuple( temp,rx ));
			return temp;
		}


		static void CheckReactivity(CTRef node) {
#ifndef NDEBUG
			assert(node->GetReactivity() != nullptr);
			for(auto up : node->Upstream()) 
				assert(Typed::IsNil(up) || up->GetReactivity() != nullptr);
#endif
		}

		CTRef Analysis::operate(CTRef source) {
			CTRef tmp = source->ReactiveReconstruct(*this);
			if (*tmp->GetReactivity( ) != *ReactivityOf(source) && tmp->GetReactivity()->IsFused()) {
				tmp = Boundary(tmp, ReactivityOf(source), tmp->GetReactivity( ));
			}
			assert(tmp->GetReactivity() != nullptr && tmp->GetReactivity() != (const Node*)0xdeadbeefdeadbeef);
			return tmp;
		}
	}

	void BuildReactivePrimitiveOps(Parser::RepositoryBuilder pack) {
		using namespace Nodes;
		using namespace Nodes::ReactiveOperators;
		auto arg = GenericArgument::New();
		auto b1 = GenericFirst::New(arg);
		auto b2 = GenericRest::New(arg);

		pack.AddFunction("Tick",GenericTick::New(arg),"driver","Provides a reactive clock source with the specified driver.");
		pack.AddFunction("Resample",GenericImpose::New(b2,b1),"sig clock","Applies a reactive 'clock' to 'signal', which is otherwise unchanged.");
		pack.AddFunction("Gate",GenericGate::New(b2,b1),"sig gate","Inhibits updates from 'signal' if gate is zero. 'Gate' must be a simple native type.");
		pack.AddFunction("Merge",GenericMerge::New(arg),"tuple","Combines the elements of a homogenous tuple into a single element. The most recently updated element from any reactive source is always present at the output.");
		pack.AddFunction("Upsample",GenericRateChange::New(b2,b1),"sig multiplier","Causes 'signal' to update 'multiplier' times for each incoming update."); 
		pack.AddFunction("Downsample",GenericRateChange::New(Invariant::Sub(Invariant::Constant::New(0),b2),b1),"signal divider","Causes 'signal' to update only once in 'divider' incoming updates");
		pack.AddFunction("Rate",GenericRate::New(arg),"sig","Retrieves the update rate of 'sig'");
//		pack.AddFunction("Adjust-Priority", GenericAdjustPriority::New(b1, GenericFirst::New(b2), GenericRest::New(b2)), "sig priority delta", "Adjusts the priorities of drivers in 'sig' so that they are equal to the highest priority driver in 'priority' modified by the invariant constant 'delta', either positive or negative.");
		pack.AddFunction("Supercede", GenericAdjustPriority::New(b1, b2, GenericAdjustPriority::Supercede), "sig clock", "Raise all clock priorities in 'sig' to match each other and exceed those in 'clock'");
		pack.AddFunction("Abdicate", GenericAdjustPriority::New(b1, b2, GenericAdjustPriority::Abdicate), "sig clock", "Lower all clock priorities in 'sig' to match each other and be inferior to those in 'clock'");
		pack.AddFunction("Clock-Edge", GenericClockEdge::New(arg), "clock", "Returns 1 if 'clock' is on a rising edge, 0 otherwise.");
	}
}
