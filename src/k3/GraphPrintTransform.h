#pragma once

#include <functional>
#include <unordered_map>
#include <ostream>
#include <stack>
#include <string>
#include <type_traits>
#include "RegionNode.h"

namespace K3 {
	namespace Transform {
		class Bounce : public DisposableClass {
		public:
			virtual Bounce* Go() = 0;
		};

		enum CachingStrategy {
			None,
			All,
			PotentialCyclicJunctions
		};

		template <typename SOURCE, typename RESULT> using CacheTy = std::unordered_map<const SOURCE*, RESULT>;

		template <typename SOURCE, typename RESULT, CachingStrategy CACHE> struct Transform {
			CacheTy<SOURCE, RESULT> cache;
			MemoryRegion ccs;
			using SourceTy = SOURCE;
			using ResultTy = RESULT;
			using MyTy = Transform<SOURCE, RESULT, CACHE>;
			MemoryRegion& GetRegion() { return ccs; }

			class Cont : public DisposableClass {
			public:
				virtual Bounce* operator()(const ResultTy&) = 0;
				Bounce* Return(const ResultTy& r) { return operator()(r); }
			};

			void* Allocate(size_t bytes) const {
				return const_cast<MyTy*>(this)->GetRegion().Allocate(bytes);
			}

			void AddCleanup(DisposableClass* dc) const {
				return const_cast<MyTy*>(this)->GetRegion().AddToCleanupList(dc);
			}

			template <typename CC> Cont* EraseCC(CC&& cc) const {
				struct c : public Cont {
					CC ccfn;
					c(CC ccfn):ccfn(std::forward<CC>(ccfn)) {}
					virtual Bounce* operator()(const ResultTy& r) { return ccfn(r); }
				};
				auto frame = new (Allocate(sizeof(c))) c(std::forward<CC>(cc));
				if (!std::is_trivially_destructible<CC>::value) AddCleanup(frame);
				return frame;
			}

			template <typename B> Bounce* EraseBounce(B&& bb) const {
				struct b : public Bounce {
					B bfn;
					b(B bfn):bfn(std::forward<B>(bfn)) {}
					virtual Bounce* Go() { return bfn(); }
				};
				auto frame = new (Allocate(sizeof(b))) b(std::forward<B>(bb));
				if (!std::is_trivially_destructible<B>::value) AddCleanup(frame);
				return frame;
			}

			template <typename B> Bounce* operator << (B&& bb) {
				return EraseBounce(std::forward<B>(bb));
			}

			template <typename CC> Bounce* operator >> (CC&& cc) {
				return EraseCC(std::forward<CC>(cc));
			}

			virtual Bounce* operate(const SourceTy*, Cont*) = 0;

			virtual Bounce* operateCached(const SourceTy* src, Cont* cc) {
				auto f = cache.find(src);
				if (f == cache.end()) {
					return operate(src, EraseCC([src,cc,this](const ResultTy& r) mutable {
						cache.emplace(src, r);
						return EraseBounce([cc,r]() { return cc->Return(r);});
					}));
				} else {
					return EraseBounce([cc, f]() { return cc->Return(f->second);});
				}
			}

			Bounce* operateDispatch(const SourceTy* src, Cont* CC) {
				switch (CACHE) {
				case None: return operate(src, CC);
				case All: return operateCached(src, CC);
				case PotentialCyclicJunctions: 
					if (src->MayHaveMultipleDownstreamConnections()) {
						return operateCached(src, CC);
					} else {
						return operate(src, CC);
					}
				}
			}

			Bounce* operator()(const SourceTy* src, Cont* CC) {
				return operateDispatch(src, CC);
			}
		};



#define BOUNCE(mem,code) return (mem).EraseBounce([=]() { code })
#define BNC(mem) return (mem) << [=]()
#define CC(mem,var,code) (mem).EraseCC([=](auto& var) { code })
#define XFM(xfm,node,var,code) { auto __xfm_pass_node = (node); BOUNCE(xfm, { return (xfm).operateDispatch(__xfm_pass_node, CC(xfm,var,code)); }; ); }
#define BRETURN(mem,cc,val) { auto __breturn_ret_val = (val); BOUNCE(mem, { return cc->Return(__breturn_ret_val); } ); }
#define CCRETURN(mem,cc,val) CC(mem, ret_val, { BRETURN(mem,cc,val); } ) 

		class PrintTransform : public Transform<Generic, std::string, PotentialCyclicJunctions> {
			Bounce* operatePair(GenericPair* p, Cont* cc) {
				auto printSuccessor = [](PrintTransform* pt, auto loop, std::string base, GenericPair* p, Cont* cc) -> Bounce* {
					XFM(*pt, p->GetUp(0), fst, {
						GenericPair *sp;
						if (p->GetUp(1)->Cast(sp)) {
							return *pt << [=]() { 
								return loop(pt, loop, base + " " + fst, sp, cc); 
							};
						} else {
							XFM(*pt, p->GetUp(1), snd, {
								BRETURN(*pt,cc, " " + fst + " " + snd);
							});
						}
					});
				};
				return printSuccessor(this, printSuccessor, "", p, CCRETURN(*this,cc,"(" + ret_val + " )"));
			}

			virtual Bounce* operateGeneric(CGRef src, Cont* cc) {
				auto printChildren = [](PrintTransform* p, auto loop, CGRef parent, int index, std::string base, Cont* cc) -> Bounce* {
					if (index < parent->GetNumCons()) {
						XFM(*p, parent->GetUp(index), s, {
							return loop(p, loop, parent, index + 1, base + " " + s, cc);
						});
					} else BRETURN(*p, cc, base);
				};
				BNC(*this) {
					std::stringstream ss;
					src->Output(ss);
					if (src->GetNumCons() == 0) BRETURN(*this, cc, ss.str());
					ss << "(";
					return printChildren(this, printChildren, src, 0, ss.str(),
										 CCRETURN(*this, cc, ret_val + " )"));
				};
			}

			virtual Bounce* operate(CGRef src, Cont* cc) {
				GenericPair* p;
				if (src->Cast(p)) return operatePair(p, cc);
				else return operateGeneric(src, cc);
			}
		};
	}

	static std::string PrintGraph(CGRef node) {
		Transform::PrintTransform test;
		std::string result;
		auto write_result = [&result](const std::string& s) -> Transform::Bounce* { result = s; return nullptr; };
		for (auto bnc = test(node, test.EraseCC(write_result));bnc;bnc = bnc->Go());
		return result;
	}
}

