 #include "common/Enumerable.h"
#include "common/PlatformUtils.h"

#include "LLVMCompiler.h"
#include "Native.h"
#include "NativeVector.h"
#include "Reactive.h"
#include "Conversions.h"
#include "CompilerNodes.h"
#include "EnumerableGraph.h"
#include "DriverSignature.h"
#include "FlowControl.h"
#include "UserErrors.h"

#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Attributes.h"

#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <set>

static size_t AlignPowerOf2(size_t align) {
	if (align < 4) return 4;
	return (align & ~(align - 1));
}

using namespace llvm;
namespace K3 {

	namespace Backends{
		bool FoldConstantInt(std::int64_t& i, CTRef node);

        static llvm::Type* GetVectorType(llvm::Type* baseType, int width, LLVMContext& cx) {
            if (width == 1) return baseType;
            else return llvm::VectorType::get(baseType, width);
        }

        static LLVMValue PassiveValueFor(LLVMTransform& lt, const K3::Type& type) {
			return Val(UndefValue::get(lt.GetType(type)));
		}

		llvm::Value* GetSignalMaskWord(llvm::IRBuilder<>& ab, llvm::Value* selfPtr, int bitIdx, int& outSubIdx) {
			auto self = ab.CreateBitCast(selfPtr, ab.getInt32Ty()->getPointerTo());
			int wordIdx = bitIdx / 32;
			outSubIdx = bitIdx % 32;
			auto val = ab.CreateLoad(ab.CreateGEP(self, ab.getInt32(-1 - wordIdx)));

			return val;
		}

		static llvm::Value* GetSignalMaskBit(LLVMTransform& lt, int bitIdx) {
			if (bitIdx < 0) {
				// this index refers to a subphase counter: reverse the index and emit the code into the entry block as these are invariant.
				//auto insertPt = lt.GetBuilder().GetInsertBlock();
				//lt.GetBuilder().SetInsertPoint(&insertPt->getParent()->getEntryBlock());
				auto val = GetSignalMaskBit(lt, -1-bitIdx);
				//lt.GetBuilder().SetInsertPoint(insertPt);
				return val;
			}

			auto& entryBlock = lt.GetBuilder().GetInsertBlock()->getParent()->getEntryBlock();
			IRBuilder<> b{ &entryBlock, entryBlock.begin() };

			int outSubIdx(0);
			llvm::Value* word = GetSignalMaskWord(b, lt.GetParameter(1), bitIdx, outSubIdx);
			return b.CreateICmpNE(b.CreateAnd(word, (1ll << outSubIdx)), Constant::getNullValue(word->getType()));
		}

		void StoreSignalMaskWord(llvm::IRBuilder<>& b, llvm::Value* selfPtr, int bitIdx, llvm::Value *word) {
			auto self = b.CreateBitCast(selfPtr, b.getInt32Ty()->getPointerTo());
			int wordIdx = bitIdx / 32;
			b.CreateStore(word, b.CreateGEP(self, b.getInt32(-1 - wordIdx)));
		}

		void StoreSignalMaskBit(llvm::IRBuilder<>& stub, llvm::Value* selfPtr, int bitIdx, llvm::Value* bit) {
			auto WideTruth = stub.CreateSExt(bit, stub.getInt32Ty());
			int outSubIdx(0);
			auto BaseMask = GetSignalMaskWord(stub, selfPtr, bitIdx, outSubIdx);
			auto SwitchMask = stub.getInt32(1 << outSubIdx);

			auto self = stub.CreateBitCast(selfPtr, stub.getInt32Ty()->getPointerTo());
			auto SetBit = stub.CreateXor(BaseMask,
										 stub.CreateAnd(
											 stub.CreateXor(WideTruth, BaseMask), SwitchMask));

			Backends::StoreSignalMaskWord(stub, self, bitIdx / 32, SetBit);
		}


		LLVMSignal::LLVMSignal(llvm::Value* v):val(v) { }

		LLVMTransform::LLVMTransform(CTRef root, ILLVMCompilationPass& comp, Function *f)
			: CachedTransform(root), bb(BasicBlock::Create(comp.GetContext( ), "Top", f)), compilation(comp), 
			function(f), builder(bb), CodeGenTransformBase(comp), currentActivityMask(0) {
			for (auto&& arg : f->args()) {
				funParam.push_back(&arg);
			}
			for (unsigned i(0);i < funParam.size();++i) {
				switch (i) {
				case 0: funParam[i]->setName("global"); break;
				case 1: funParam[i]->setName("state"); break;
				default: funParam[i]->setName(Twine("p") + Twine(i - 1));
				}
			}
		}

		LLVMTransform::LLVMTransform(ILLVMCompilationPass &comp) : CachedTransform(0), compilation(comp), bb(BasicBlock::Create(comp.GetContext(), "Top", function)), builder(bb),
			function(llvm::Function::Create(
			llvm::FunctionType::get(llvm::Type::getInt8PtrTy(comp.GetContext()), llvm::Type::getInt8PtrTy(comp.GetContext()), false),
			llvm::GlobalValue::InternalLinkage, "launch", GetModule().get())),
			CodeGenTransformBase(comp), currentActivityMask(0) { }

		LLVMTransform::LLVMTransform(const LLVMTransform& src)
			: bb(src.bb), builder(bb), compilation(src.compilation), function(src.function),
			CachedTransform(src.GetRoot()), CodeGenTransformBase(src.compilation), currentActivityMask(src.currentActivityMask), funParam(src.funParam) {
			cache = src.cache;
		}

		LLVMValue LLVMTransform::operate(CTRef node) {
			return node->Compile(*this, currentActivityMask);
		}

		llvm::Type* LLVMTransform::GetType(const K3::Type& t) {
			if (t.IsNativeVector()) {
				return llvm::VectorType::get(GetType(t.GetVectorElement()), t.GetVectorWidth());
			} else if (t.IsFloat32()) return builder.getFloatTy();
			else if (t.IsFloat64()) return builder.getDoubleTy();
			else if (t.IsInt32()) return builder.getInt32Ty();
			else if (t.IsInt64()) return builder.getInt64Ty();
			else if (t.IsPair()) {
				std::vector<llvm::Type*> elTys;

				for (Type tt = t;tt.GetSize();) {
					auto fst = tt.First();
					auto count = tt.CountLeadingElements(fst);
					tt = tt.Rest(count);

					if (fst.GetSize() == 0) {
						continue;
					}

					if (tt == fst) {
						elTys.emplace_back(llvm::ArrayType::get(GetType(fst), count + 1));
						break;
					}

					elTys.emplace_back(llvm::ArrayType::get(GetType(fst), count));
				}

				switch (elTys.size()) {
				case 1:
					return elTys.front();
				default:
					return llvm::StructType::get(builder.getContext(), elTys, true);
				}
			} else if (t.IsUserType()) {
				return GetType(t.UnwrapUserType());
			}
			else INTERNAL_ERROR("Bad Kronos/LLVM type conversion");
		}

		template <typename OSTREAM, typename T>
		OSTREAM& operator<<(OSTREAM& strm, const std::vector<T*>& vec) {
			if (vec.size()) {
				strm << *vec.front();
				for (unsigned i(1); i < vec.size(); ++i) strm << ", " << *vec.at(i);
			}
			return strm;
		}

		struct LLVMDriverActivityFilter : public ILLVMCompilationPass {
			FunctionCache cache;
			ActivityMaskVector* avm;

			llvm::Function* GetMemoized(CTRef body, llvm::FunctionType* fty) {
				auto f(cache.find(FunctionKey(body, fty)));
				return f != cache.end() ? f->second : 0;
			}

			void Memoize(CTRef body, llvm::Function* func) {
				cache.insert(std::make_pair(FunctionKey(Graph<const Typed>(body), func->getFunctionType()), func));
			}

			LLVMDriverActivityFilter(ILLVMCompilationPass& m, ActivityMaskVector *a) :avm(a), master(m) {}
			ILLVMCompilationPass& master;
			llvm::LLVMContext& GetContext() { return master.GetContext(); }
			const std::unique_ptr<llvm::Module>& GetModule() { return master.GetModule(); }
			const CallGraphNode* GetCallGraphAnalysis(const Nodes::Subroutine* s) { return master.GetCallGraphAnalysis(s); }
			const std::string& GetCompilationPassName() { return master.GetCompilationPassName(); }

			DriverActivity IsDriverActive(const Type& sig) override {
				return FilterDriverActivity(sig, avm, master);
			}

			BuilderPass GetPassType() override { return master.GetPassType(); }
			void SetPassType(BuilderPass bp) override { master.SetPassType(bp); }
		};

		void LLVMTransform::ProcessPendingMergeBlock(llvm::BasicBlock*& pendingMergeBlock, llvm::BasicBlock *passiveBlock, LLVMTransform& subroutineBuilder, vector<CTRef> &skippedNodes) {
			if (pendingMergeBlock) {
				llvm::IRBuilder<>& b(subroutineBuilder.GetBuilder());
				llvm::BasicBlock *activeBlock = b.GetInsertBlock();

				// terminate active block
				b.CreateBr(pendingMergeBlock);

				LLVMTransform passiveBuilder(subroutineBuilder); {
					auto &b(passiveBuilder.GetBuilder());
					b.SetInsertPoint(passiveBlock);
					for (auto sn : skippedNodes) {
						auto val(sn->Compile(passiveBuilder, nullptr));
						if (val) {
							passiveBuilder.cache.find(sn)->second = val;
						}
					}
					b.CreateBr(pendingMergeBlock);
				}

				b.SetInsertPoint(pendingMergeBlock);

				// merge active and passive values via phi nodes
				for (auto sn : skippedNodes) {
					auto valptr(subroutineBuilder.cache.find(sn));
					if (valptr && passiveBuilder.cache.find(sn)) {
						llvm::Value* activeValue = valptr->second;
						llvm::Value* passiveValue = passiveBuilder.cache.find(sn)->second;
						if (activeValue != passiveValue) {
							auto phi = b.CreatePHI(activeValue->getType(), 2, "rx.merge");
							phi->addIncoming(activeValue, activeBlock);
							phi->addIncoming(passiveValue, passiveBlock);
							valptr->second = Val(phi);
						}
					}
				}
				skippedNodes.clear();
				pendingMergeBlock = 0;
			}
		}

		Function* LLVMTransform::BuildSubroutine(const char *label, CTRef body, const std::vector<llvm::Type*> &params) {
			auto core = [&]() mutable {
				auto newSubr = BuildSubroutineCore(label, body, params);
				return newSubr;
			};
#ifndef NDEBUG
			std::unordered_set<int64_t> BufferUIDS;
			for (auto n : Qxx::FromGraph(body).OfType<Buffer>()) {
				if (BufferUIDS.count(n->GetUID())) {
					INTERNAL_ERROR("Aliasing buffers detected");
				} else BufferUIDS.emplace(n->GetUID());
			}
#endif
			auto out = TLS::WithNewStack(core);

#ifndef NDEBUG
			std::string contextLabel = GetCompilationPass().GetCompilationPassName() + "::" + label;
			if (TLS::GetCurrentInstance()->ShouldTrace("Emit", contextLabel.c_str())) {
				std::string fn;
				llvm::raw_string_ostream os(fn);
				os << *out;
				std::clog << "Emit:	[" << label << "]\n" << *body << "\n\n" << fn;
			}
#endif

			return out;
		}

		llvm::Value* LLVMTransform::GenerateNodeActiveFlag(IRBuilder<>& b, ActivityMaskVector* avm) {
			if (!avm) return Constant::getNullValue(b.getInt1Ty());
			llvm::Value *nodeActive = nullptr; 
			for (auto& maskSet : *avm) {
				if (maskSet.size()) {
					llvm::Value *driverActive = nullptr;
					for (int mask : maskSet) {
						auto findBit = signalMaskBits.find(mask);
						if (findBit == signalMaskBits.end()) {
							findBit = signalMaskBits.emplace(mask, GetSignalMaskBit(*this, mask)).first;
						}
						auto cmp = findBit->second;
						driverActive = driverActive ? b.CreateAnd(driverActive, cmp) : cmp;
					}
					if (driverActive) {
						nodeActive = nodeActive ? b.CreateOr(nodeActive, driverActive) : driverActive;
					}
				}
			}
			return nodeActive;
		}

		Function* LLVMTransform::BuildSubroutineCore(const char *label, CTRef body, const std::vector<llvm::Type*> &params) {
			bool returnStatePtr( true );
			auto buildType(FunctionType::get(returnStatePtr ? GetBuilder().getInt8PtrTy() : GetBuilder().getVoidTy(), params, false));

			Function *memoized(GetCompilationPass().GetMemoized(body, buildType));
			if (memoized) {
				return memoized;
			}

			Function *build = llvm::Function::Create(buildType, GlobalValue::LinkageTypes::PrivateLinkage, llvm::Twine(GetCompilationPass().GetCompilationPassName()) + "::" + label, GetModule().get());
			GetCompilationPass().Memoize(body, build);

			build->setCallingConv(CallingConv::Fast);
			build->setDoesNotThrow();
			build->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

			LLVMDriverActivityFilter knownActiveMasks(compilation, currentActivityMask);

			LLVMTransform subroutineBuilder(body, currentActivityMask && currentActivityMask->size() ? knownActiveMasks : compilation, build);

			auto bodyBB = llvm::BasicBlock::Create(GetContext(), "body", build);
			auto& builder(subroutineBuilder.GetBuilder());
			builder.SetInsertPoint(bodyBB);

			if (IsSizingPass()) {
				auto& b{ subroutineBuilder.GetBuilder() };
				llvm::Value* result = subroutineBuilder(body, nullptr);
				auto szvar = "sizeof_" + std::to_string((uintptr_t)body);
				auto sz = new GlobalVariable(*GetModule(), b.getInt64Ty(), false, 
								   GlobalValue::InternalLinkage,
								   UndefValue::get(b.getInt64Ty()), szvar);

				b.CreateStore(
					b.CreateSub(
						b.CreatePtrToInt(
							result, b.getInt64Ty()),
						b.CreatePtrToInt(
							subroutineBuilder.GetParameter(2),
							b.getInt64Ty())),
					sz);
				
				b.CreateRet(result);			
				b.SetInsertPoint(&build->getEntryBlock());
				b.CreateBr(bodyBB);
				return build;
			}

			/* figure out reactivity masks and schedule of nodes */
			std::vector<SchedulingUnit> nodeList(
				TopologicalSort(
					Qxx::FromGraph(body)
					.Select([&](CTRef node) {
						assert(!node->Cast<DataSource>());
						return std::make_tuple(node, GetActivityMasks(node)); 
					})
					.ToVector()));

#ifndef NDEBUG
			//for (auto i = nodeList.begin(); i != nodeList.end(); ++i) {
			//	for (auto j(nodeList.begin()); j != i; ++j) {
			//		assert(InSubgraph(std::get<0>(*i), std::get<0>(*j)) == false && "Scheduling is borked");
			//	}
			//}
#endif

			ActivityMaskVector emptyMaskVector;
			auto &b(subroutineBuilder.GetBuilder());
			ActivityMaskVector *current(0);
			std::vector<CTRef> skippedNodes;
			llvm::BasicBlock *pendingMergeBlock(0);
			llvm::BasicBlock *pendingPassiveBlock(0);

#ifndef NDEBUG
#endif

			for (auto schdUnit : nodeList) {
				CTRef node = std::get<0>(schdUnit);
				if (IsOfExactType<Deps>(node) || 
					IsOfExactType<SubroutineMeta>(node)) continue;

				ActivityMaskVector* avm = std::get<1>(schdUnit);
				// start a new block?
				if (current != avm && (current == 0 || avm == 0 || *current != *avm)) {
					if (pendingMergeBlock) {
						ProcessPendingMergeBlock(pendingMergeBlock, pendingPassiveBlock, subroutineBuilder, skippedNodes);
					}

					current = avm;

					// start a new block, otherwise stay in the merge block
					if (avm && avm->size()) {
						// emit condition
						llvm::Value *nodeActive = subroutineBuilder.GenerateNodeActiveFlag(b, avm);

						if (nodeActive) {
							llvm::BasicBlock *activeBlock = BasicBlock::Create(GetContext(), "rx.active", subroutineBuilder.GetFunction());
							pendingPassiveBlock = BasicBlock::Create(GetContext(), "rx.passive", subroutineBuilder.GetFunction());
							pendingMergeBlock = BasicBlock::Create(GetContext(), "rx.mergeblock", subroutineBuilder.GetFunction());
							assert(nodeActive->getType() == b.getInt1Ty());
							b.CreateCondBr(nodeActive, activeBlock, pendingPassiveBlock);
							b.SetInsertPoint(activeBlock);
						}
					}
				}

				if (avm) {
					if (avm->size()) skippedNodes.push_back(node);
					subroutineBuilder(node, avm);
				} else subroutineBuilder(node, NULL);
			}

			if (pendingMergeBlock) {
				ProcessPendingMergeBlock(pendingMergeBlock, pendingPassiveBlock, subroutineBuilder, skippedNodes);
			}

			if (returnStatePtr) {
				subroutineBuilder.GetBuilder().CreateRet(
					subroutineBuilder(body, &emptyMaskVector));
			} else {
				subroutineBuilder.GetBuilder().CreateRetVoid();
			}

			builder.SetInsertPoint(&build->getEntryBlock());
			builder.CreateBr(bodyBB);

#ifndef NDEBUG
			//if (IsInitPass()) {
			//	std::cout << *body << "\n";
			//	std::string buf;
			//	llvm::raw_string_ostream os(buf);
			//	build->print(os);
			//	std::cout << os.str();
			//}
#endif
			return build;
		}

		llvm::Value* LLVMTransform::GetParameter(size_t ID) {
			return funParam[ID - 1];
		}

		llvm::Value* LLVMTransform::GetSequenceCounter() {
			llvm::Value *counter(GetParameter(function->arg_size()));
			return builder.CreateZExt(counter, builder.getInt64Ty());
		}

		template <typename T> static void SetAlignment(IRBuilder<>& b, T* inst, int a) {
			int ap2 = AlignPowerOf2(a);
			inst->setAlignment(ap2);
		}

		llvm::Value* LLVMTransform::Alloca(size_t GUID, llvm::Value* sz, size_t alignment) {
			auto f(AllocatedBuffers.find(GUID));
			if (f != AllocatedBuffers.end()) return f->second;

			auto &b(GetBuilder());
			auto &entryBlock = b.GetInsertBlock()->getParent()->getEntryBlock();
			IRBuilder<> ab{ &entryBlock, entryBlock.begin() };

			auto allocaInst(ab.CreateAlloca(b.getInt8Ty(), sz));

			auto constSz = dyn_cast<ConstantInt>(sz);
			if (constSz) {
				while (alignment > constSz->getZExtValue()) alignment >>= 1;
			}

			SetAlignment(ab, allocaInst, alignment);

			std::stringstream name;
			name << "local" << GUID;
			allocaInst->setName(name.str());

			b.CreateLifetimeStart(allocaInst, dyn_cast<ConstantInt>(sz));
			AllocatedBuffers.insert(make_pair(GUID, allocaInst));

			return allocaInst;
		}
	};

	using namespace Backends;
	namespace Nodes{

		LLVMValue MultiDispatch::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto &b(lt.GetBuilder());
			// allocate state for all the subroutines and place passive sizer versions in transform cache
			// these are all always called, so state pointer accumulation dominates all subsequent blocks
			auto statePtr = lt(GetUp(2));
			for (unsigned i(0);i < dispatchees.size();++i) {
				if (dispatchees[i].first) 
					statePtr = lt(dispatchees[i].first, nullptr);
			}

			auto idx = lt(GetUp(0));
			auto writeSti = lt(GetUp(1));

			// ensure that branches don't pull any upstream nodes into dispatch blocks
			// that could cause domination errors
			for (auto &d : dispatchees) {
				if (d.first)
				for (unsigned j(1);j < d.first->GetNumCons();++j) {
					if (d.first) lt(d.first->GetUp(j),active);
				}
			}

			std::vector<llvm::BasicBlock*> dispatchBlocks(dispatchees.size());

			auto stiPointer = writeSti ? b.CreateBitOrPointerCast(writeSti, b.getInt32Ty()->getPointerTo()) : nullptr;

			auto mainBlock = b.GetInsertBlock();
			auto postBlock = llvm::BasicBlock::Create(lt.GetContext(), "merge", lt.GetFunction());

			for (unsigned i(0);i < dispatchees.size();++i) {
				b.SetInsertPoint(mainBlock);

				auto tw = Twine("dispatch.").concat(Twine(i));
				dispatchBlocks[i] = llvm::BasicBlock::Create(lt.GetContext(), tw, lt.GetFunction(), postBlock);
				b.SetInsertPoint(dispatchBlocks[i]);

				if (dispatchees[i].first) {
					// bypass transform cache as we want to ignore the cached sizer and produce active
					// branch instead. 
					dispatchees[i].first->Compile(lt, active);
				}

				if (stiPointer) {
					// write union tag
					b.CreateStore(b.getInt32(dispatchees[i].second), stiPointer);
				}
				b.CreateBr(postBlock);
			}
			b.SetInsertPoint(mainBlock);
			auto sw = b.CreateSwitch(idx, postBlock, dispatchBlocks.size());
			for (unsigned i(0);i < dispatchBlocks.size();++i) {
				sw->addCase(b.getInt32(i), dispatchBlocks[i]);
			}
			b.SetInsertPoint(postBlock);

			return statePtr;
		}

		LLVMValue SubroutineArgument::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			/* first two IDs are instance and stateptr */
			auto p(lt.GetParameter(ID + 1));
			return Val(p);
		}

		LLVMValue BoundaryBuffer::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto sig(lt(GetUp(0), active));
			auto buf(lt(GetUp(1), active));
			if (active) return sig;
			else return buf;
		}

		LLVMValue Configuration::Compile(LLVMTransform& lt, ActivityMaskVector *active) const {
			auto fn = lt.GetModule()->getOrInsertFunction("GetConfigurationSlot", llvm::FunctionType::get(lt.GetBuilder().getInt8PtrTy(), { lt.GetBuilder().getInt32Ty() }, false));
			return Val(lt.GetBuilder().CreateCall(fn, { lt.GetBuilder().getInt32(slotIndex) }));
		}

		LLVMValue DerivedConfiguration::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			std::string cfgName = "dconf_" + std::to_string((std::uintptr_t)cfg);
			auto i32t{ lt.GetBuilder().getInt32Ty() };
			if (lt.IsSizingPass()) {
				lt.GetCompilationPass().SetPassType(BuilderPass::InitializationWithReturn);
				auto val = cfg->Compile(lt, active);
				lt.GetCompilationPass().SetPassType(BuilderPass::Sizing);
				auto dc = new GlobalVariable(*lt.GetModule(), i32t,
											 false, GlobalValue::InternalLinkage, 
											 UndefValue::get(i32t),
											 cfgName);
				lt.GetBuilder().CreateStore(val, dc);
				return val;
			} else {
				return Val(lt.GetBuilder().CreateLoad(
					lt.GetModule()->getOrInsertGlobal(
						cfgName,i32t)));
			}
		}

		LLVMValue GetSlot::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto &b{ lt.GetBuilder() };
			auto self = lt.GetParameter(1);
			self = b.CreateBitCast(self, b.getInt8PtrTy()->getPointerTo(), "slots");
			auto ptrPtr = b.CreateConstGEP1_32(self, index, "slot");
			llvm::Value* ptr = b.CreateLoad(ptrPtr);
			if (lt.IsInitPass()) {
				if (GetUp(0)) {
                    if (!IsNil(GetUp(0))) {
                        llvm::Value* init = lt(GetUp(0));
                        ptr = b.CreateSelect(
                                             b.CreateICmpNE(ptr, Constant::getNullValue(ptr->getType())), ptr, init);
                        b.CreateStore(ptr, ptrPtr);
                    }
				} else {
					auto fn = lt.GetModule()->getOrInsertFunction("GetConfigurationSlot", 
																  llvm::FunctionType::get(lt.GetBuilder().getInt8PtrTy(), { lt.GetBuilder().getInt32Ty() }, false));
					return Val(lt.GetBuilder().CreateCall(fn, { lt.GetBuilder().getInt32(index) }));
				}
			}
			return Val(ptr);
		}

		LLVMValue SignalMaskSetter::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			if (active) {
				auto &b(lt.GetBuilder());
				llvm::Value* gateSig = lt(GetUp(0));
				llvm::Value* gateBool;
				if (gateSig->getType()->isIntegerTy()) {
					gateBool = b.CreateICmpNE(gateSig, Constant::getNullValue(gateSig->getType()));
				} else if (gateSig->getType()->isFloatingPointTy()) {
					gateBool = b.CreateFCmpONE(gateSig, Constant::getNullValue(gateSig->getType()));
				} else INTERNAL_ERROR("Bad gate signal type");

				StoreSignalMaskBit(b, lt.GetParameter(1), bitIdx, gateBool);
			}
			return nullptr;
		}

		llvm::Value* GetMemoizedFunctionSize(llvm::IRBuilder<>& b, const Subroutine* which) {
			std::string gvarName = "sizeof_" + std::to_string((std::uintptr_t)which->GetBody());
			return b.CreateLoad(b.GetInsertBlock()->getParent()->getParent()->getOrInsertGlobal(gvarName, b.getInt64Ty()));
		}

		LLVMValue SubroutineStateAllocation::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			if (lt.IsSizingPass()) {
				return subr->Compile(lt, active);
			} else {
				auto sz = GetMemoizedFunctionSize(lt.GetBuilder(), subr);
				return Val(
					lt.GetBuilder().CreateGEP(lt(GetUp(0)), sz));
			}
		}

		static llvm::Value* RebaseLoopGEP(LLVMTransform& lt, int argID, CTRef node, llvm::Value* counter, llvm::Value *op, int baseOffset = 0) {
			Offset* offs;
			if (node->Cast(offs)) {
				std::int64_t o(0);
				if (FoldConstantInt(o, offs->GetUp(1))) {
					return RebaseLoopGEP(lt, argID, offs->GetUp(0), counter, op, baseOffset + o);
				}
			}

			SubroutineArgument *sa;
			if (node->Cast(sa)) {
				if (sa->GetID() == argID) {
					if (baseOffset == 0) return op;
					auto &b(lt.GetBuilder());
					auto maybeMul = [&b](std::int64_t mul, llvm::Value* val) -> llvm::Value* {
						switch (mul) {
						case 0: return b.getInt64(0);
						case 1: return b.CreateZExt(val, b.getInt64Ty());
						default: return b.CreateMul(b.getInt64(mul), b.CreateZExt(val, b.getInt64Ty()));
						}
					};
					for (auto&& u : op->uses()) {
						llvm::BitCastInst* bci;
						if ((bci = llvm::dyn_cast<llvm::BitCastInst>(u.getUser()))) {
							auto pointee = bci->getDestTy()->getPointerElementType();
							int stride = pointee->getPrimitiveSizeInBits() / 8;
							if (baseOffset % stride == 0) {
								auto typedGep = b.CreateGEP(
									b.CreatePointerCast(op, bci->getDestTy()), maybeMul(baseOffset / stride, counter));
								bci->replaceAllUsesWith(typedGep);
							}
						}
					}
					return b.CreateGEP(op, maybeMul(baseOffset, counter));
				}
			}

			return nullptr;
		}

		LLVMValue Subroutine::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			llvm::Value* returnValue;
			bool tailCallSafe(true);
			for (auto up : Upstream()) {
				if (lt.RefersLocalBuffers(up)) { tailCallSafe = false; break; }
			}

			auto& b(lt.GetBuilder());
			std::vector<llvm::Value*> params;

			params.push_back(lt.GetParameter(1)); // add 'self', which is a TLS global

			for (auto up : Upstream()) {
				auto sig(lt(up, active));
				params.push_back(sig);
			}

			std::vector<std::pair<unsigned, Attribute::AttrKind>> paramAttrVector;
			paramAttrVector.reserve(params.size() * 2);

			unsigned idx(0);
			auto paramTypes(Qxx::From(params).Select([&](llvm::Value* v)
			{
				if (v->getType()->isPointerTy()) {
					paramAttrVector.emplace_back(make_pair(idx + 1, Attribute::NoAlias));
					paramAttrVector.emplace_back(make_pair(idx + 1, Attribute::NoCapture));
				}
				idx++;
				return v->getType();
			}).ToVector());


			if (conditionalRecursionLoopCount) {
				auto paramsEnd(params);
				paramsEnd.pop_back();
				paramsEnd.emplace_back(b.getInt(llvm::APInt(32, conditionalRecursionLoopCount, true)));

				params.back() = b.CreateAdd(params.back(), b.getInt32(1), "loopcount", true, true);
				auto counterZero(b.CreateICmpEQ(params.back(), b.getInt32(conditionalRecursionLoopCount)));

				auto counterGtZero(BasicBlock::Create(lt.GetContext(), "Recursion", lt.GetFunction()));
				auto counterIsZero(BasicBlock::Create(lt.GetContext(), "RecursionEnds", lt.GetFunction()));
				b.CreateCondBr(counterZero, counterIsZero, counterGtZero);

				llvm::PHINode *phi(nullptr);
				llvm::BasicBlock *mergeBlock(nullptr);

				if (tailCallSafe) {
					// loop back to function main body
					auto fun = b.GetInsertBlock()->getParent();
					auto bbi = ++fun->begin();
					
					b.SetInsertPoint(&*bbi, bbi->getFirstInsertionPt());

					auto counter = lt.GetFunction()->arg_begin();
					for (unsigned i(1);i < lt.GetFunction()->arg_size();++i,++counter);

					std::vector<llvm::Value*> transformedParam{ params.size() };
					std::vector<std::vector<llvm::Use*>> paramUses{ params.size() };

					for (unsigned i(1);i < params.size();++i) {
						auto op = lt.GetParameter(i + 1);
						for (auto&& u : op->uses()) {
							paramUses[i].emplace_back(&u);
						}
					}

					// process counter first for GEP rebases
					auto counterPhi = b.CreatePHI(params.back()->getType(), 2, "lc");
					counterPhi->addIncoming(lt.GetParameter(params.size()), &fun->getEntryBlock());
					counterPhi->addIncoming(params.back(), counterGtZero);

					transformedParam.back() = counterPhi;

					// transform recursion parameters to phi nodes from entry block or recursion
					for (unsigned  i(1);i < params.size() - 1; ++i) {
						auto op = lt.GetParameter(i + 1);
						if (params[i] != op) {
							// parameter is not invariant in recursion
							auto rebase = RebaseLoopGEP(lt, i, GetUp(i-1), counterPhi, op);
							if (rebase && rebase != op) {
								transformedParam[i] = rebase;
							} else {
								b.SetInsertPoint(&*bbi, bbi->getFirstInsertionPt());
								auto phi = b.CreatePHI(params[i]->getType(), 2, "rp" + Twine(i + 1));
								phi->addIncoming(lt.GetParameter(i + 1), &fun->getEntryBlock());
								phi->addIncoming(params[i], counterGtZero);
								
								transformedParam[i] = phi;

								if (auto fnArg = dyn_cast<llvm::Argument>(params[i])) {
									assert(fnArg == lt.GetParameter(fnArg->getArgNo() + 1));
									paramUses[fnArg->getArgNo()].emplace_back(&phi->getOperandUse(1));
								}
							}
						}
					}

					
					for (unsigned i(1);i < params.size();++i) {
						if (transformedParam[i]) {
							for (auto&& u : paramUses[i]) {
								u->set(transformedParam[i]);
							}

							if (auto fnArg = dyn_cast<llvm::Argument>(params[i])) {
								// if arguments are shuffled, tail needs to get them from the loop body, not top
								paramsEnd[i] = transformedParam[i];
							}
						}
					}

					b.SetInsertPoint(counterGtZero);
					b.CreateBr(&*bbi);

				} else {
					mergeBlock = (BasicBlock::Create(lt.GetContext(), "Merge", lt.GetFunction()));
					b.SetInsertPoint(counterGtZero);
					auto recursion(b.CreateCall(lt.GetFunction(), params, "recur"));
					recursion->setCallingConv(CallingConv::Fast);
					b.CreateBr(mergeBlock);
					b.SetInsertPoint(mergeBlock);
					phi = b.CreatePHI(b.getInt8PtrTy(), 2);
					phi->addIncoming(recursion, counterGtZero);
					returnValue = phi;
				}

				auto recursionEndFunc(lt.BuildSubroutine("tail", compiledBody, paramTypes));
				for (auto& a : paramAttrVector) recursionEndFunc->addAttribute(a.first, a.second);

				b.SetInsertPoint(counterIsZero);
				auto recursionEnd(b.CreateCall(recursionEndFunc, paramsEnd, "recur_end"));
				recursionEnd->setCallingConv(recursionEndFunc->getCallingConv());

				if (tailCallSafe) {
					recursionEnd->setTailCall();
					returnValue = recursionEnd;
				} else {
					phi->addIncoming(recursionEnd, counterIsZero);
					b.CreateBr(mergeBlock);
					b.SetInsertPoint(mergeBlock);
				}
			} else {
				if (active || lt.IsSizingPass()) {
					auto func(lt.BuildSubroutine(GetLabel(), compiledBody, paramTypes));
					for (auto& a : paramAttrVector) func->addAttribute(a.first, a.second);

					auto call(b.CreateCall(func, params, "fc"));

					call->setCallingConv(func->getCallingConv());
					if (tailCallSafe) call->setTailCall(true);
					returnValue = call;
				} else {
					returnValue = b.CreateGEP(params[1], GetMemoizedFunctionSize(b, this));
				}
			}
			return Val(returnValue);
		}

		LLVMValue SequenceCounter::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			return Val(lt.GetBuilder().CreateAdd(lt.GetSequenceCounter(), lt.GetBuilder().getInt64(counter_offset)));
		}

		LLVMValue SizeOfPointer::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto &b(lt.GetBuilder());
			auto onePtr = b.CreateConstGEP1_32(llvm::Constant::getNullValue(b.getInt8PtrTy()->getPointerTo()), 1);
			return Val(b.CreatePtrToInt(onePtr, b.getInt64Ty()));
		}

		LLVMValue Deps::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto a(lt(GetUp(0)));
			return a;
		}


		LLVMValue ExternalAsset::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto var = dyn_cast<GlobalVariable>(lt.GetModule()->getOrInsertGlobal(dataUri, lt.GetType(dataType)));
			var->setLinkage(GlobalValue::ExternalLinkage);
			return Val(
				lt.GetBuilder().CreateBitCast(
					var,
					lt.GetBuilder().getInt8PtrTy()
				));
		}

		static void MarkInvariant(llvm::IRBuilder<>& b, llvm::Module* module, llvm::Value* ptr, llvm::Value* sz) {
			auto invariant_start(Intrinsic::getDeclaration(module, Intrinsic::invariant_start));
			b.CreateCall(invariant_start, { sz, b.CreateBitCast(ptr, b.getInt8PtrTy()) },"invariant");
		}

		LLVMValue Copy::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto &b(lt.GetBuilder());

			auto dstSig = lt(GetUp(0),active);
			auto srcSig = lt(GetUp(1),active);
			auto szSig = lt(GetUp(2), active);

			if ((active || lt.IsInitPass())
				&& !lt.IsSizingPass()) {
				switch (mode) {
				case Store: {
					Value* val(srcSig?srcSig:llvm::UndefValue::get(b.getInt8PtrTy()));
					Value* rawptr(dstSig?dstSig:llvm::UndefValue::get(b.getInt8PtrTy()));
					Value* dst(b.CreateBitCast(rawptr, val->getType()->getPointerTo()));
#ifndef NDEBUG
                    dst->setName(rawptr->getName() + ".cast");
#endif
					Backends::SetAlignment(b,b.CreateStore(val, dst, false),dstAlign);
					break;
				}
				case MemCpy: {
					llvm::Value *dst(dstSig), *src(srcSig), *elSz(szSig);
					dst = b.CreateBitCast(dst, b.getInt8PtrTy());
					auto align = srcAlign && dstAlign ? AlignPowerOf2(-((-srcAlign) | (-dstAlign))) : 4;

					// don't emit empty loops
					int staticLoopCount = 999;
					if (auto c = GetUp(3)->Cast<Native::Constant>()) {
						if ((staticLoopCount = *(int32_t*)c->GetPointer()) < 1) {
							break;
						}
					}

					if (staticLoopCount > 1) {
						auto loopCount = b.CreateSExt(lt(GetUp(3), active), b.getInt64Ty());

						auto priorBlock = b.GetInsertBlock();
						auto loopBlock = llvm::BasicBlock::Create(b.getContext(), "initLoop", priorBlock->getParent());
						auto nextBlock = llvm::BasicBlock::Create(b.getContext(), "endLoop", priorBlock->getParent());
						b.CreateBr(loopBlock);
						b.SetInsertPoint(loopBlock);

						auto i = b.CreatePHI(b.getInt64Ty(), 2, "i");
						auto nextI = b.CreateAdd(i, b.getInt64(1));
						i->addIncoming(b.getInt64(0), priorBlock);
						i->addIncoming(nextI, loopBlock);
						b.CreateMemCpy(b.CreateGEP(dst, b.CreateMul(i, elSz)), src, elSz, align);
						b.CreateCondBr(b.CreateICmpNE(nextI, loopCount), loopBlock, nextBlock);

						b.SetInsertPoint(nextBlock);
					} else {
						b.CreateMemCpy(dst, src, elSz, align);
					}

					break;
				}
				default:
					INTERNAL_ERROR("Bad copy mode in LLVM backend");
				}
			}
			return dstSig;
		}

		static llvm::Value* IsBufferBig(llvm::IRBuilder<>& b, llvm::Value* size) {
			return b.CreateICmpSGT(size, b.getInt64(65536));
		}

		LLVMValue ReleaseBuffer::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto &b(lt.GetBuilder());
			auto buf = lt(GetUp(0));
			auto sz = lt(GetUp(1));
			b.CreateLifetimeEnd(buf, dyn_cast<ConstantInt>((Value*)sz));
		return LLVMValue();
		}

		LLVMValue Buffer::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			switch (alloc) {
			case Empty:
				return Val(llvm::Constant::getNullValue(lt.GetBuilder().getInt8PtrTy()));
			case StackZeroed:
			case Stack:
			{
				auto sz = lt(GetUp(0));
				auto allocaed = lt.Alloca(GUID, sz, alignment);

				if (alloc == StackZeroed) {
					lt.GetBuilder().CreateMemSet(allocaed, lt.GetBuilder().getInt8(0), sz, alignment);
				}

				return Val(allocaed);
			}
			case Module:
			{
				Native::Constant *c(0);
				GetUp(0)->Cast(c);
				assert(c && "Module buffers must be constant sized");
				auto tp(ArrayType::get(lt.GetBuilder().getInt8Ty(), *(std::int64_t*)c->GetPointer()));
                auto gv = new GlobalVariable(*lt.GetModule(), tp, false, GlobalValue::InternalLinkage, GlobalValue::getNullValue(tp));
				return Val(lt.GetBuilder().CreateBitCast(gv, lt.GetBuilder().getInt8PtrTy()));
			}
			default:
				INTERNAL_ERROR("Undefined buffer allocation mode");
			}
		}

		LLVMValue Offset::Compile(LLVMTransform& lt, ActivityMaskVector*) const {
			// todo: alignment
			ActivityMaskVector all;
			auto &b(lt.GetBuilder());
			auto ptr(lt(GetUp(0), &all));
            auto offs = lt(GetUp(1), &all);
			auto gep(b.CreateGEP(ptr, offs));
			auto sig = Val(gep);
#ifndef NDEBUG
            gep->setName("ptr");
#endif
            return sig;
		}

		LLVMValue Reference::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			KRONOS_UNREACHABLE;
		}

		LLVMValue Dereference::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			if (loadType == Type::Nil) return LLVMValue();

			if (!lt.IsSizingPass() &&
				(active || lt.IsInitPass())) {
				auto& b(lt.GetBuilder());
				auto ptr(lt(GetUp(0)));
				auto bcPtr = b.CreateBitCast(ptr,
					(loadPtr ? b.getInt8PtrTy() : lt.GetType(loadType))->getPointerTo());
				auto load(b.CreateLoad(bcPtr));
				SetAlignment(b, load, alignment);
#ifndef NDEBUG
				auto oldName = ptr->getName();
				bcPtr->setName(oldName + ".cast");
				load->setName(oldName + ".load");
#endif
				return Val(load);
			}

			if (loadPtr) {
				return Val(UndefValue::get(lt.GetBuilder().getInt8PtrTy()));
			} else {
				return Val(UndefValue::get(lt.GetType(loadType)));
			}
		}

		LLVMValue ExtractVectorElement::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			return Val(lt.GetBuilder().CreateExtractElement(lt(GetUp(0)), lt.GetBuilder().getInt32(idx)));
		}

		LLVMValue AtIndex::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto vec(lt(GetUp(0)));
			auto idx(lt(GetUp(1)));
			return Val(lt.GetBuilder().CreateGEP(vec, lt.GetBuilder().CreateMul(idx, lt.GetBuilder().getInt32(elem.GetSize()))));
		}

		LLVMValue BitCast::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
			auto value(lt(GetUp(0)));
			auto &b(lt.GetBuilder());
            llvm::Type* dstType = nullptr;
                                
			if (to.IsFloat32()) {
                dstType = b.getFloatTy();
			} else if (to.IsFloat64()) {
                dstType = b.getDoubleTy();
			} else if (to.IsInt32()) {
                dstType = b.getInt32Ty();
			} else if (to.IsInt64()) {
                dstType = b.getInt64Ty();
			} else if (to.IsArrayView()) {
				return Val(b.CreateIntToPtr(value, b.getInt8PtrTy()));
            } else {
                assert(0 && "unknown native type");
                KRONOS_UNREACHABLE;
            }
                
            return Val(b.CreateBitOrPointerCast(value, GetVectorType(dstType, vectorWidth, b.getContext())));
		}

		LLVMValue CStringLiteral::Compile(LLVMTransform& lt, ActivityMaskVector*) const {
			auto& builder = lt.GetBuilder();
			std::stringstream strlit;
			str.OutputText(strlit);
			auto stringConstant = ConstantDataArray::getString(lt.GetContext(), strlit.str());
			auto gv = new GlobalVariable(*lt.GetModule(), stringConstant->getType(), true, GlobalValue::InternalLinkage, stringConstant);
			return Val(ConstantExpr::getBitCast(gv, builder.getInt8PtrTy()));
		}

		namespace ReactiveOperators {
			LLVMValue ClockEdge::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				auto tmp{ Qxx::FromGraph(GetClock())
						 .OfType<Reactive::DriverNode>()
						 .Select([](const Reactive::DriverNode* dn) {return dn->GetID(); }).ToVector()
				};

				auto mask = lt.CollectionToMask(Qxx::From(tmp), false);

				auto &b{ lt.GetBuilder() };

				auto isActive = lt.GenerateNodeActiveFlag(b, mask);

				auto ty = b.getFloatTy();

				if (!isActive) {
					return Val(Constant::getAllOnesValue(ty));
				}

				auto val = b.CreateSelect(isActive,
					Constant::getAllOnesValue(ty),
					Constant::getNullValue(ty));
				val->setName("clock-edge");
				return Val(val);
			}
		}

		namespace Native{
			LLVMValue ForeignFunction::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				assert(compilerNode);
				if (active || lt.IsInitPass()) {
					std::vector<llvm::Value*> params(GetNumCons());
					std::vector<llvm::Type*> paramTys(GetNumCons());


					for (unsigned int i(0); i < GetNumCons(); ++i) {
						params[i] = lt(GetUp(i));
						paramTys[i] = params[i]->getType();
					}

					auto sym = Symbol;
					if (sym.back() == '!') {
						sym.pop_back();
						if (lt.IsInitPass()) {
							sym.append("_init");
						}
					}

					llvm::Type *retTy = lt.GetType(FixedResult());
					auto funTy = llvm::FunctionType::get(retTy, paramTys, false);
					auto fun = lt.GetModule()->getOrInsertFunction(sym, funTy);
					auto call = lt.GetBuilder().CreateCall(fun, params, "user");
					return Val(call);
				} else {
					return PassiveValueFor(lt, FixedResult());
				}
			}

			static llvm::Type* getIntegerBitcastType(llvm::Type *source) {
				llvm::Type *elTy = llvm::Type::getIntNTy(source->getContext(), source->getScalarSizeInBits());

				if (source->isVectorTy()) {
					return llvm::VectorType::get(elTy, source->getVectorNumElements());
				} else {
					return elTy;
				}
			}

			static Value* ComparisonMask(LLVMTransform& lt, const Type& destType, Value* truthValue) {
				auto& b(lt.GetBuilder());
				if (destType.GetVectorElement().IsFloat32() || destType.GetVectorElement().IsFloat64()) {
					return b.CreateBitCast(
						b.CreateSExt(truthValue, getIntegerBitcastType(lt.GetType(destType))), lt.GetType(destType));
				} else {
					return b.CreateSExt(truthValue, lt.GetType(destType));
				}
			}

			static Value* BitMask(LLVMTransform& lt, const Type& destType, Value* rawValue) {
				return lt.GetBuilder().CreateBitCast(rawValue, getIntegerBitcastType(lt.GetType(destType)));
			}

			static Value* TypedValue(LLVMTransform& lt, const Type& destType, Value* rawValue) {
				return lt.GetBuilder().CreateBitCast(rawValue, lt.GetType(destType));
			}

			LLVMValue Select::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				LLVMValue cond(lt(GetUp(0))), whenTrue(lt(GetUp(1))), whenFalse(lt(GetUp(2)));
				llvm::Value* cv(cond);
				if (cv->getType()->isIntOrIntVectorTy()) cv = lt.GetBuilder().CreateICmpNE(cv, llvm::Constant::getNullValue(cv->getType()));
				else cv = lt.GetBuilder().CreateFCmpUNE(cv, llvm::Constant::getNullValue(cv->getType()));
				return Val(lt.GetBuilder().CreateSelect(cv, whenTrue, whenFalse));
			}

			LLVMValue ITypedBinary::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				if (active == NULL) {
					return PassiveValueFor(lt, FixedResult());
				}

				LLVMValue lhs(lt(GetUp(0))), rhs(lt(GetUp(1)));
				auto& b(lt.GetBuilder());

				bool integerType(lt.GetType(FixedResult())->isIntOrIntVectorTy());

				llvm::Value *returnValue(0);

				switch (GetOpcode()) {
				case Add:
					returnValue = (integerType ? b.CreateAdd(lhs, rhs) : b.CreateFAdd(lhs, rhs)); break;
				case Mul:
					returnValue = (integerType ? b.CreateMul(lhs, rhs) : b.CreateFMul(lhs, rhs)); break;
				case Sub:
					returnValue = (integerType ? b.CreateSub(lhs, rhs) : b.CreateFSub(lhs, rhs)); break;
				case Div:
					returnValue = (integerType ? b.CreateSDiv(lhs, rhs) : b.CreateFDiv(lhs, rhs)); break;
				case Modulo:
					returnValue =
						b.CreateURem(b.CreateAdd(lhs, b.CreateMul(rhs, b.CreateUDiv(
						FixedResult() == Type::Int32 ? b.getInt32(0x8000000)
						: b.getInt64(0x8000000000000000ul), rhs))),
						rhs); break;
				case ClampIndex:
					returnValue = b.CreateSelect(b.CreateICmpUGT(lhs, rhs),
						llvm::Constant::getNullValue(((llvm::Value*)lhs)->getType()),
						lhs); break;
				case Max:
					returnValue = (b.CreateSelect(integerType ? b.CreateICmpSGT(lhs, rhs) : b.CreateFCmpUGT(lhs, rhs), lhs, rhs)); break;
				case Min:
					returnValue = (b.CreateSelect(integerType ? b.CreateICmpSLT(lhs, rhs) : b.CreateFCmpUGT(rhs, lhs), lhs, rhs)); break;
				case Equal:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpEQ(lhs, rhs) : b.CreateFCmpUEQ(lhs, rhs))); break;
				case Not_Equal:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpNE(lhs, rhs) : b.CreateFCmpUNE(lhs, rhs))); break;
				case Greater:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpSGT(lhs, rhs) : b.CreateFCmpUGT(lhs, rhs))); break;
				case Greater_Equal:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpSGE(lhs, rhs) : b.CreateFCmpUGE(lhs, rhs))); break;
				case Less:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpSLT(lhs, rhs) : b.CreateFCmpULT(lhs, rhs))); break;
				case Less_Equal:
					returnValue = (ComparisonMask(lt, FixedResult(), integerType ? b.CreateICmpSLE(lhs, rhs) : b.CreateFCmpULE(lhs, rhs))); break;
				case And:
					returnValue = (TypedValue(lt, FixedResult(),
						b.CreateAnd(BitMask(lt, FixedResult(), lhs), BitMask(lt, FixedResult(), rhs)))); break;
				case AndNot:
					returnValue = (TypedValue(lt, FixedResult(),
						b.CreateAnd(b.CreateNot(BitMask(lt, FixedResult(), lhs)), BitMask(lt, FixedResult(), rhs)))); break;
				case Or:
					returnValue = (TypedValue(lt, FixedResult(),
						b.CreateOr(BitMask(lt, FixedResult(), lhs), BitMask(lt, FixedResult(), rhs)))); break;
				case Xor:
					returnValue = (TypedValue(lt, FixedResult(),
						b.CreateXor(BitMask(lt, FixedResult(), lhs), BitMask(lt, FixedResult(), rhs)))); break;
				case BitShiftLeft:
					returnValue = (TypedValue(lt, FixedResult(), b.CreateShl(BitMask(lt, FixedResult(), lhs), rhs))); break;
				case BitShiftRight:
					returnValue = (TypedValue(lt, FixedResult(), b.CreateAShr(BitMask(lt, FixedResult(), lhs), rhs))); break;
				case LogicalShiftRight:
					returnValue = (TypedValue(lt, FixedResult(), b.CreateLShr(BitMask(lt, FixedResult(), lhs), rhs))); break;
				case Pow:
				{
					llvm::Value* p[2] = { lhs, rhs };
					return Val(b.CreateCall(
							Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::pow, lt.GetType(FixedResult())), p,"pow"));

				}
				case Atan2:
				{
					llvm::Value* p[2] = { lhs, rhs };
					auto funty = llvm::FunctionType::get(p[0]->getType(), { p[0]->getType(), p[1]->getType() }, false);
					auto fun = dyn_cast<llvm::Function>(FixedResult().IsFloat32() ? lt.GetModule()->getOrInsertFunction("atan2f", funty)
								: lt.GetModule()->getOrInsertFunction("atan2", funty));

					fun->setDoesNotAccessMemory();
					return Val(b.CreateCall(fun, p));
				}
				default:
					INTERNAL_ERROR("Bad binary node in compilation");
				}
				return Val(returnValue);
			}


			LLVMValue ITypedUnary::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				if (active == NULL) {
					return PassiveValueFor(lt, FixedResult());
				}

				auto& b(lt.GetBuilder());
				Type tmp(false);
				bool integerType(lt.GetType(FixedResult())->isIntOrIntVectorTy());
				llvm::Value* up(lt(GetUp(0)));


				switch (GetOpcode()) {
				case Neg:
					return Val(integerType ? b.CreateNeg(up) : b.CreateFNeg(up));
				case Abs:
					if (integerType) {
						return Val(b.CreateSelect(b.CreateICmpSGE(up, llvm::Constant::getNullValue(((llvm::Value*)up)->getType())),
							up, b.CreateSub(llvm::Constant::getNullValue(((llvm::Value*)up)->getType()), up)));
					} else {
						return Val(b.CreateCall(Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::fabs, lt.GetType(FixedResult())), up ,"abs"));
					}
				case Not:
					return Val(integerType ? b.CreateNot(up) :
						TypedValue(lt, FixedResult(), b.CreateNot(BitMask(lt, FixedResult(), up))));
				case Round:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::nearbyint, lt.GetType(FixedResult())), up,"round"));
				case Truncate:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::trunc, lt.GetType(FixedResult())), up, "trunc"));
				case Ceil:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::ceil, lt.GetType(FixedResult())), up, "ceil"));
				case Floor:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::floor, lt.GetType(FixedResult())), up, "floor"));
				case Sqrt:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::sqrt, lt.GetType(FixedResult())), up, "sqrt"));
				case Cos:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::cos, lt.GetType(FixedResult())), up, "cos"));
				case Sin:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::sin, lt.GetType(FixedResult())), up, "sin"));
				case Exp:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::exp, lt.GetType(FixedResult())), up, "exp"));
				case Log:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::log, lt.GetType(FixedResult())), up, "log"));
				case Log10:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::log10, lt.GetType(FixedResult())), up, "log10"));
				case Log2:
					return Val(b.CreateCall(
						Intrinsic::getDeclaration(lt.GetModule().get(), Intrinsic::log2, lt.GetType(FixedResult())), up, "log2"));
				default:break;
				}
				INTERNAL_ERROR("Bad unary node in compilation");
			}

			static const void* GetCompositeConstant(llvm::IRBuilder<>& b, const void *sourceData, std::vector<llvm::Constant*>& consts, const Type& t) {
				if (t.IsUserType()) {
					sourceData = GetCompositeConstant(b, sourceData, consts, t.UnwrapUserType());
					return sourceData;
				}
				if (t.IsPair()) {
					sourceData = GetCompositeConstant(b, sourceData, consts, t.First());
					sourceData = GetCompositeConstant(b, sourceData, consts, t.Rest());
					return sourceData;
				}
				if (t.IsFloat32()) {
					const float *f = (const float *)sourceData;
					consts.push_back(ConstantFP::get(b.getContext(), APFloat(*f)));
					return f + 1;
				} else if (t.IsFloat64()) {
					const double *f = (const double*)sourceData;
					consts.push_back(ConstantFP::get(b.getContext(), APFloat(*f)));
					return f + 1;
				} else if (t.IsInt32()) {
					const std::int32_t *f = (const std::int32_t *)sourceData;
					consts.push_back(b.getInt32(*f));
					return f + 1;
				} else if (t.IsInt64()) {
					const std::int64_t *f = (const std::int64_t *)sourceData;
					consts.push_back(b.getInt64(*f));
					return f + 1;
				} else {
					assert(t.GetSize() == 0);
				}
				return sourceData;
			}

			static llvm::Constant* MakeConstant(llvm::LLVMContext& cx, const Type& type, const void* memory, int offset = 0) {
				assert(type.IsNativeType());
				if (type.IsNativeVector()) {
					std::vector<llvm::Constant*> values(type.GetVectorWidth());
					for (int i = 0; i < values.size(); ++i) {
						values[i] = MakeConstant(cx, type.GetVectorElement(), memory, i);
					}
					return ConstantVector::get(values);
				} else if (type.IsFloat32()) {
					float* f((float*)memory);
					return ConstantFP::get(cx, APFloat(f[offset]));
				} else if (type.IsFloat64()) {
					double* f((double*)memory);
					return ConstantFP::get(cx, APFloat(f[offset]));
				} else if (type.IsInt32()) {
					int32_t* i((int32_t*)memory);
					return ConstantInt::get(llvm::Type::getInt32Ty(cx), i[offset], true);
				} else if (type.IsInt64()) {
					int64_t* i((int64_t*)memory);
					return ConstantInt::get(llvm::Type::getInt64Ty(cx), i[offset], true);
				}
				assert(0 && "unknown native type");
				KRONOS_UNREACHABLE;
			}

			LLVMValue Constant::Compile(LLVMTransform& lt, ActivityMaskVector* active) const {
				if (type.IsNil() || memory == nullptr) return LLVMValue();

				if (type.IsNativeType()) {
					return Val(MakeConstant(lt.GetContext(),
										type,
										GetPointer(),
										0));
				}

				std::vector<llvm::Constant*> consts;
                auto end = GetCompositeConstant(lt.GetBuilder(), memory, consts, type); (void)end;
				assert(end == (char *)memory + len);

				auto val = ConstantStruct::getAnon(consts, true);
				return Val(lt.GetBuilder().CreatePointerCast(
					new GlobalVariable(*lt.GetModule(), val->getType(), true, GlobalValue::InternalLinkage, val, "constant"),
					lt.GetBuilder().getInt8PtrTy()));
			}
		};

		/* convert from f32 */
		template<> LLVMValue  EmitCvt<float, float>(Backends::LLVMTransform& lt, Typed *src, int width) {
			return lt(src);
		}

		template<> LLVMValue  EmitCvt<double, float>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPExt(lt(src), GetVectorType(llvm::Type::getDoubleTy(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<int32_t, float>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPToSI(lt(src), GetVectorType(llvm::Type::getInt32Ty(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<int64_t, float>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPToSI(lt(src), GetVectorType(llvm::Type::getInt64Ty(cx), width, cx)));
		}

		/* convert from f64 */
		template<> LLVMValue  EmitCvt<float, double>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPTrunc(lt(src), GetVectorType(llvm::Type::getFloatTy(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<double, double>(Backends::LLVMTransform& lt, Typed *src, int width) {
			return lt(src);
		}

		template<> LLVMValue  EmitCvt<int32_t, double>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPToSI(lt(src), GetVectorType(llvm::Type::getInt32Ty(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<int64_t, double>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateFPToSI(lt(src), GetVectorType(llvm::Type::getInt64Ty(cx), width, cx)));
		}

		/* convert from i32 */
		template<> LLVMValue  EmitCvt<float, int32_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateSIToFP(lt(src), GetVectorType(llvm::Type::getFloatTy(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<double, int32_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateSIToFP(lt(src), GetVectorType(llvm::Type::getDoubleTy(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<int32_t, int32_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			return lt(src);
		}

		template<> LLVMValue  EmitCvt<int64_t, int32_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateSExt(lt(src), GetVectorType(llvm::Type::getInt64Ty(cx), width, cx)));
		}

		/* convert from i64 */
		template<> LLVMValue  EmitCvt<float, int64_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateSIToFP(lt(src), GetVectorType(llvm::Type::getFloatTy(cx), width, cx)));
		}

		template<> LLVMValue  EmitCvt<double, int64_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateSIToFP(lt(src), GetVectorType(llvm::Type::getDoubleTy(cx), width, cx)));
		}

		template<> LLVMValue EmitCvt<int32_t, int64_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			auto& b(lt.GetBuilder());
			auto& cx(lt.GetContext());
			return Val(b.CreateTrunc(lt(src), GetVectorType(llvm::Type::getInt32Ty(cx), width, cx)));
		}

		template<> LLVMValue EmitCvt<int64_t, int64_t>(Backends::LLVMTransform& lt, Typed *src, int width) {
			return lt(src);
		}
	};
};
