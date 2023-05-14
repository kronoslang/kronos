#pragma once

#include "CodeGenModule.h"
#include "NodeBases.h"

namespace K3 {
	namespace Backends {
		using namespace K3::Nodes;
		

		template <typename TCodeGen>
		class GenericEmitterTransform : public TCodeGen {
		public:

			using ModuleTy = typename TCodeGen::ModuleTy;
			using FunctionTy = typename TCodeGen::FunctionTy;
			using FunctionTyTy = typename TCodeGen::FunctionTyTy;
			using TypeTy = typename TCodeGen::TypeTy;
			using ValueTy = typename TCodeGen::ValueTy;
			using BuilderTy = typename TCodeGen::BuilderTy;
			using BlockTy = typename TCodeGen::BlockTy;

			struct FunctionKey : std::tuple<FunctionTyTy, Graph<const Typed>> {
				FunctionKey(const Graph<const Typed>& key, FunctionTyTy fty) :std::tuple<FunctionTyTy, Graph<const Typed>>(fty, key) {}
				FunctionTyTy GetFunctionTy() const { return std::get<0>(*this); }
				Graph<const Typed> GetGraph() const { return std::get<1>(*this); }
				size_t GetHash() const { return GetGraph()->GetHash(true) ^ std::hash<FunctionTyTy>()(GetFunctionTy()); }
				bool operator==(const FunctionKey& rhs) const { return (CTRef)GetGraph() == (CTRef)rhs.GetGraph() && GetFunctionTy() == rhs.GetFunctionTy(); }
			};

			struct FunctionKeyHash {
				size_t operator()(const FunctionKey& fk) const { return fk.GetHash(); }
			};

			using FunctionCacheTy = std::unordered_map<FunctionKey, FunctionTy, FunctionKeyHash>;

			class IGenericCompilationPass : public ICompilationPass {
			public:
				virtual ModuleTy& GetModule() = 0;
				virtual FunctionTy GetMemoized(CTRef src, FunctionTyTy fty) = 0;
				virtual void Memoize(CTRef source, FunctionTyTy, FunctionTy) = 0;
			};

			struct GenericDriverActivityFilter : public IGenericCompilationPass {
				ActivityMaskVector* avm;
				IGenericCompilationPass& master;
				FunctionCacheTy cache;

				GenericDriverActivityFilter(IGenericCompilationPass& m, ActivityMaskVector* avm) :avm(avm), master(m) {}

				FunctionTy GetMemoized(CTRef body, FunctionTyTy fty) override {
					auto f{ cache.find(FunctionKey{body, fty}) };
					assert(f == cache.end() || f->first.GetFunctionTy() == fty);
					return f != cache.end() ? f->second : FunctionTy{};
				}

				void Memoize(CTRef body, FunctionTyTy fty, FunctionTy func) override {
					FunctionKey fk{ Graph<const Typed>(body), fty };
					assert(cache.count(fk) == 0);
					cache.emplace(fk, func);
				}

				ModuleTy& GetModule() override { return master.GetModule(); }

				const CallGraphNode* GetCallGraphAnalysis(const Nodes::Subroutine* s) override { return master.GetCallGraphAnalysis(s); }
				const std::string& GetCompilationPassName() override { return master.GetCompilationPassName(); }

				DriverActivity IsDriverActive(const Type& sig) override {
					return FilterDriverActivity(sig, avm, master);
				}

				Backends::BuilderPass GetPassType() override {
					return master.GetPassType();
				}

				void SetPassType(Backends::BuilderPass bp) override {
					master.SetPassType(bp);
				}
			};

			void StoreSignalMaskBit(int bitIdx, ValueTy gateBool) {
				int outSubIdx{ 0 };
				auto wideTruth = current.Select(gateBool, current.Const(-1), current.Const(0));
				auto baseMask = current.GetSignalMaskWord(bitIdx, outSubIdx);
				auto switchMask = current.Const(1 << outSubIdx);
				auto setBit = current.XorInt32(
					baseMask, 
					current.AndInt32(
						current.XorInt32(wideTruth, baseMask), 
						switchMask));

				current.StoreSignalMaskWord(bitIdx, setBit);				
			}

			ValueTy GetSignalMaskBit(int bitIdx) {
				if (bitIdx < 0) {
					// this index refers to a subphase counter: reverse the index and emit the code into the entry block as these are invariant.
					//auto insertPt = lt.GetBuilder().GetInsertBlock();
					//lt.GetBuilder().SetInsertPoint(&insertPt->getParent()->getEntryBlock());
					auto val = GetSignalMaskBit(-1 - bitIdx);
					//lt.GetBuilder().SetInsertPoint(insertPt);
					return val;
				}

				int outSubIdx(0);
				auto& builder{ current };
				ValueTy word = builder.GetSignalMaskWord(bitIdx, outSubIdx);
				return builder.NeInt32(builder.AndInt32(word, builder.Const(1 << outSubIdx)), builder.NullConst(Int32Ty()));
			}

			ValueTy GenerateNodeActiveFlag(ActivityMaskVector* avm) {
				if (!avm) return current.NullConst(BoolTy());
				auto& builder{ current };
				ValueTy nodeActive = nullptr;
				for (auto& maskSet : *avm) {
					if (maskSet.size()) {
						ValueTy driverActive = nullptr;
						for (int mask : maskSet) {
							auto cmp = GetSignalMaskBit(mask);
							driverActive = driverActive ? builder.And(driverActive, cmp) : cmp;
						}
						if (driverActive) {
							nodeActive = nodeActive ? builder.Or(nodeActive, driverActive) : driverActive;
						}
					}
				}
				return nodeActive;
			}

			using TCodeGen::RefersLocalBuffers;
			using TCodeGen::Int32Ty;
			using TCodeGen::Int64Ty;
			using TCodeGen::Float32Ty;
			using TCodeGen::Float64Ty;
			using TCodeGen::VoidTy;
			using TCodeGen::BoolTy;
			using TCodeGen::PtrTy;
			using TCodeGen::CreateFunction;
			using TCodeGen::CreateFunctionTy;

			FunctionTy CurrentFunction() { return currentFn; }

			template <typename... TArgs>
			GenericEmitterTransform(IGenericCompilationPass& gp, CTRef root, ModuleTy M) :compilation(gp), TCodeGen(root, M, gp) {}
			GenericEmitterTransform(const GenericEmitterTransform& parent, CTRef root) :compilation(parent.compilation), TCodeGen(root, parent) {}
		protected:
			FunctionTy currentFn;
			IGenericCompilationPass & compilation;

			ValueTy InstancePtr(BuilderTy& b) {
				return b.FnArg(0);
			}

			template <typename TIterator>
			void EmitNodeRange(TIterator beg, TIterator end, BuilderTy& b) {
				auto old = current;
				current = b;
				for (auto i = beg;i != end;++i) {
					currentActivityMask = std::get<1>(*i);
					auto node = std::get<0>(*i);
					if (!IsOfExactType<Deps>(node)) (*this)(node);
				}
				current = old;
			}

			template <typename TIterator>
			void EmitPassiveNodeRange(TIterator beg, TIterator end, BuilderTy& b) {
				auto old = current;
				current = b;
				for (auto i = beg;i != end;++i) {
					currentActivityMask = nullptr;
					auto node = std::get<0>(*i);
					if (!IsOfExactType<Deps>(node)) (*this)(node);
				}
				current = old;
			}

			BuilderTy current;

			using SchedulingUnit = CodeGenTransformBase::SchedulingUnit;

			void BuildSubroutineBody(FunctionTy& fn, const char* label, CTRef body, const std::vector<TypeTy>& params) {
				currentFn = fn;
				current = { fn };

				/* handle the sizing pass */
				if (IsSizingPass()) {
					ActivityMaskVector sizingActive;
					currentActivityMask = &sizingActive;
					auto result = current.TmpVar((*this)(body));
					// must match codegens (BinaryenCompiler)
					auto szvar = "sizeof_" + std::to_string((uintptr_t)body);
					auto ac = current.FnArg(0); // state ptr
					auto szval = current.SubInt64(
						current.Coerce(Int64Ty(), current.PtrToInt(result)),
						current.Coerce(Int64Ty(), current.PtrToInt(ac)));

					current.SetGVar(szvar, szval);
					current.Ret(result);
					return;
				}

				/* produce reactive masks and schedule the nodes */
				std::vector<SchedulingUnit> nodeList(
					CodeGenTransformBase::TopologicalSort(
						Qxx::FromGraph(body)
						.Select([&](CTRef node) { 
							return std::make_tuple(node, 
							CodeGenTransformBase::GetActivityMasks(node)); 
						})
						.ToVector()));

				ActivityMaskVector emptyMaskVector;
				ActivityMaskVector *currentMaskVector{ nullptr };
				std::vector<CTRef> skippedNodes;
				auto& b{ current };

				auto pendingBlockStart = nodeList.begin();

//				std::clog << "\n\n[" << label << "]\n";

				for (auto i = nodeList.begin(); i != nodeList.end(); ++i) {
					CTRef node; 
					ActivityMaskVector *avm;
					std::tie(node, avm) = *i;

/*					std::clog << " - " << node->GetLabel();
					if (node->GetReactivity()) std::clog << " : " << *node->GetReactivity() << "\n";
					else std::clog << "\n";*/
				
					if (currentMaskVector != avm && (currentMaskVector == nullptr || avm == nullptr || *currentMaskVector != *avm)) {
						if (currentMaskVector && currentMaskVector->size()) {
							auto flag = GenerateNodeActiveFlag(currentMaskVector);
							assert(flag && "Should always generate dynamic flag in this branch");
							
							this->OpenBranch();							
							b.If(flag,
							[&](BuilderTy& rx) {
								EmitNodeRange(pendingBlockStart, i, rx);
							},
							[&](BuilderTy& passive) {
								EmitPassiveNodeRange(pendingBlockStart, i, passive);
							});
							this->CloseBranch();
						} else {
							EmitNodeRange(pendingBlockStart, i, current);
						}

						pendingBlockStart = i;
						currentMaskVector = avm;
					}
				}

				if (currentMaskVector && currentMaskVector->size()) {
					this->OpenBranch();
					b.If(GenerateNodeActiveFlag(currentMaskVector), [&](BuilderTy& rx) {
						EmitNodeRange(pendingBlockStart, nodeList.end(), rx);
					}, [&](BuilderTy& rx) {
						EmitPassiveNodeRange(pendingBlockStart, nodeList.end(), rx);
					});
					this->CloseBranch();
				} else {
					EmitNodeRange(pendingBlockStart, nodeList.end(), current);
				}
				current.Ret((*this)(body));
			}

			virtual FunctionTy Build(const char*, CTRef, const std::vector<TypeTy>&) = 0;

			ActivityMaskVector *currentActivityMask = nullptr;
		public:
			BuilderTy* operator->() {
				return &current;
			}
			IGenericCompilationPass& GetCompilationPass() { return compilation; }
			bool IsSizingPass() {
				return compilation.GetPassType() == BuilderPass::Sizing;
			}
		};
	}
}
