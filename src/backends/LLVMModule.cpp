#pragma warning(disable: 4267 4244 4146)
#include "llvm/Support/Host.h"
#include "llvm/IR/Verifier.h"
#include "LLVMUtil.h"
#include "llvm/Support/raw_os_ostream.h"

#include "LLVM.h"
#include "LLVMModule.h"

#include "CallGraphAnalysis.h"
#include "TLS.h"
#include "DriverSignature.h"

#include "UserErrors.h"

#include <sstream>
#include <tuple>

#include <iostream>

#include "kronosrt.h"

#include "LLVMCmdLine.h"

namespace CL {
	CmdLine::Option<string> LlvmHeader(std::string(""), "--llvm-header", "-H", "<path>", "write a C/C++ header for the LLVM-generated object to <path>, '-' for stdout");
}

namespace {

	class CompilationPass : public virtual Backends::ILLVMCompilationPass, public CodeGenPass {
		Backends::LLVM &builder;
		Backends::LLVMTransform Compiler;
		template<typename... Args> inline void pass(Args&&...) { }
		FunctionCache cache;
		Backends::BuilderPass passType;
	public:
		FunctionCache GetFunctionCache() { return cache; }
		template <typename... DRIVERS>
		CompilationPass(Backends::LLVM& builder, CounterIndiceSet& cs, const std::string& label, CTRef intermediateAST, Backends::BuilderPass pt, DRIVERS... drivers)
			:builder(builder), CodeGenPass(label, intermediateAST, cs), passType(pt),
			Compiler(intermediateAST, *this,
				llvm::cast<llvm::Function>(builder.GetModule()->getOrInsertFunction("__launch", llvm::Type::getVoidTy(builder.GetContext())))) {
			pass(insert(drivers)...);
		}

		std::unique_ptr<llvm::Module>& GetModule() { return builder.GetModule(); }
		llvm::LLVMContext& GetContext() { return builder.GetContext(); }

		operator llvm::Function*() {
			auto func = Compiler.BuildSubroutine("Main", ast,
				std::vector<llvm::Type*>(4, llvm::Type::getInt8PtrTy(builder.GetContext())));
#ifndef NDEBUG
			if (llvm::verifyFunction(*func, &llvm::errs())) {
				llvm::errs() << "*** Broken Function Starts ***\n";
				llvm::errs() << *func;
				llvm::errs() << "*** Broken Function Ends   ***\n";
				INTERNAL_ERROR("Compiler frontend emitted invalid IR");
			}
#endif
			return func;
		};

		llvm::Function* GetMemoized(CTRef body, llvm::FunctionType* fty) {
			auto f(cache.find(FunctionKey(body, fty)));
			return f != cache.end() ? f->second : 0;
		}

		void Memoize(CTRef body, llvm::Function* func) {
			cache.insert(std::make_pair(FunctionKey(Graph<const Typed>(body), func->getFunctionType()), func));
		}

		const Backends::CallGraphNode* GetCallGraphAnalysis(const Subroutine* subr) { return builder.GetCallGraphData(subr); }
		const std::string& GetCompilationPassName() { return label; }

		DriverActivity IsDriverActive(const K3::Type& driverID) {
			return CodeGenPass::IsDriverActive(driverID);
		}

		Backends::BuilderPass GetPassType() override {
			return passType;
		}

		// here be dragons
		void SetPassType(Backends::BuilderPass bp) override {
			passType = bp;
		}
	};
}
template <typename T>
static void llvmdbg(T* value) {
	std::string buf;
	llvm::raw_string_ostream s(buf);
	s << *value << "\n\n";
	std::cout << s.str();
}

namespace K3 {

	namespace Backends {
		void StoreSignalMaskWord(llvm::IRBuilder<>&, llvm::Value* self, int bitIdx, llvm::Value *word);
		llvm::Value* GetSignalMaskWord(llvm::IRBuilder<>&, llvm::Value* self, int bitIdx, int& outSubIdx);

		class LLVMContextHolder : public ManagedObject {
			INHERIT(LLVMContextHolder, ManagedObject);
		public:
			llvm::LLVMContext context;
		};

		llvm::LLVMContext& LLVM::GetContext() {
			return Context;
		}

		LLVM::~LLVM() {
		}

		void LLVM::MakeIR(Kronos::BuildFlags flags) {
			using namespace LLVMUtil;
			using GetSymbolOffsetTy = std::int64_t(std::int64_t);

			Backends::AnalyzeCallGraph(0, intermediateAST, cgmap);

			CounterIndiceSet empty;
			llvm::Function *sizeOfFunc = CompilationPass(*this, empty, "SizeOf", intermediateAST, Backends::BuilderPass::Sizing);
			llvm::Function *initFunc = CompilationPass(*this, empty, "Init", intermediateAST, Backends::BuilderPass::Initialization,
													   DriverSignature(Type(&Reactive::InitializationDriver)));

			LLVMGen ir(GetContext(), GetModule().get());

			struct {
				llvm::Type* i8;
				llvm::Type* i32;
				llvm::Type* i64;
				llvm::Type* ptr;
			} ty;

			ty.i8 = ir.ty(std::int8_t());
			ty.i32 = ir.ty(std::int32_t());
			ty.i64 = ir.ty(std::int64_t());
			ty.ptr = ir.ty((void*)0);

			llvm::Function *evalFunc(0);

			if ((flags & Kronos::OmitEvaluate) == 0) {
				CompilationPass evalPass(*this, empty, "Eval", intermediateAST, Backends::BuilderPass::Evaluation,
										 DriverSignature(Type(&Reactive::ArgumentDriver)));

				for (auto d : drivers) evalPass.insert(d);
				evalFunc = evalPass;
			} else {
				evalFunc = ir.defn<void(void*, void*, void*, void*)>(Linkage::Internal, "__Eval", [&](auto gen, auto ty) {
					gen.CreateRetVoid();
				});

				evalFunc->setCallingConv(CallingConv::Fast);
				evalFunc->setLinkage(GlobalValue::LinkageTypes::InternalLinkage);
				evalFunc->setDoesNotThrow();
			}

			auto sizeOfStateStub = ir.declare<GetSymbolOffsetTy>(Linkage::Export, "GetSymbolOffset");
			sizeOfStateStub->setCallingConv(llvm::CallingConv::C);
			sizeOfStateStub->setDoesNotThrow();

			auto sizeOfStub = ir.declare<krt_get_size_call>(Linkage::Export, "GetSize");
			sizeOfStub->setCallingConv(llvm::CallingConv::C);
			sizeOfStub->setDoesNotThrow();

			llvm::Function* evalStub = nullptr;

			if (evalFunc) {
				evalStub = ir.defn<krt_evaluate_call>(Linkage::Export, "Evaluate", [&](auto ir, auto args) {
					auto ai = args.begin();
					auto instance = (llvm::Argument*)ai++;
					auto input = (llvm::Argument*)ai++;
					auto output = (llvm::Argument*)ai++;

					auto self_offset(ir.CreateGEP(instance, ir.CreateCall(sizeOfStateStub, { ir.getInt64(0) }, "sizeof_state")));
					ir.CreateCall(evalFunc, { self_offset, instance, input, output })->setCallingConv(CallingConv::Fast);
					ir.CreateRetVoid();
				});

				evalStub->setCallingConv(llvm::CallingConv::C);
				evalStub->setDoesNotThrow();
			}

			if ((flags & Kronos::OmitReactiveDrivers) == 0) {
				unordered_set<Type> DriverSignatures;
				for (auto driver : drivers) {
					DriverSignature sig(driver);
					if (sig.GetMetadata().IsNil() == false) {
						DriverSignatures.insert(sig.GetMetadata());
					}
				}

				for (auto driver : DriverSignatures) {
					stringstream name;
					name << driver;
					std::string dn(name.str());

					if (dn.size()) dn[0] = toupper(dn[0]);

					for (unsigned i(1); i < dn.size(); ++i) {
						if (!isalpha(dn[i - 1]) && isalpha(dn[i]))
							dn[i] = toupper(dn[i]);
					}

					dn.erase(std::remove_if(dn.begin(), dn.end(), [](char c) {return !isalnum(c); }), dn.end());

					name.clear();
					name.str("");
					name << "Tick" << dn;

					llvm::Function *callable(GetActivation(name.str(), intermediateAST, driver, sizeOfStateStub, sizeOfStub));
					inputCall.insert(std::make_pair(driver, callable));
				}
			}

			auto initStub = ir.API(ir.defn<krt_constructor_call>(Linkage::Export, "Initialize", [&](auto stub, auto args) {
				size_t asz(this->GetArgumentType().GetSize()),
					rsz(this->GetResultType().GetSize());

				auto outputBuf = stub.CreateAlloca(stub.getInt8Ty(), stub.getInt64(rsz));
				outputBuf->setAlignment(16);
				auto ai = args.begin();
				auto instance = (llvm::Argument*)ai++;
				auto self_offset(stub.CreateGEP(instance, stub.CreateCall(sizeOfStateStub, { stub.getInt64(0) }, "sizeof_state")));

				auto argumentData = (llvm::Argument*)ai++;

				if (asz) {
					stub.CreateStore(
						argumentData,
						stub.CreateConstGEP1_32(
							stub.CreateBitCast(self_offset, stub.getInt8PtrTy()->getPointerTo()),
							this->GetArgumentIndex()));
				}

				stub.CreateCall(initFunc, { self_offset, instance, argumentData, outputBuf }, "init")->setCallingConv(CallingConv::Fast);
				stub.CreateRetVoid();
			}));


			auto deinitStub = ir.API(ir.defn<krt_destructor_call>(Linkage::Export, "Deinitialize", [&](auto stub, auto args) {
/*				auto ai = args.begin();
				auto instance = (llvm::Argument*)ai++;
				auto host = (llvm::Argument*)ai++;*/
				stub.CreateRetVoid();
			}));

			auto getValStub = ir.API(ir.defn<krt_get_slot_call>(Linkage::Export, "GetValue", [&](auto stub, auto args) {
				auto ai = args.begin();
				auto instance = (llvm::Argument*)ai++;
				auto slot = (llvm::Argument*)ai++;

				auto offset = stub.CreateCall(sizeOfStateStub, { stub.CreateZExt(slot, stub.getInt64Ty()) });
				auto ptr = stub.CreateBitCast(stub.CreateGEP(instance, offset), stub.getInt8PtrTy()->getPointerTo());
				stub.CreateRet(ptr);
			}));

			/* finalize sizeof last as GetNumSymbols is now final */
			ir.API(ir.implement(sizeOfStateStub, [&](auto stub, auto args) {
				// measure symbol table location

/*				auto offset(stub.CreatePtrToInt(stub.CreateCall(sizeOfFunc, { null, null, null, null }), 
												stub.getInt64Ty()));*/

				std::string totalSzVar = "sizeof_" + std::to_string((std::uintptr_t)(CTRef)intermediateAST);
				auto offset = stub.CreateLoad(ir.GetModule()->getOrInsertGlobal(totalSzVar, stub.getInt64Ty()));

				// measure bitmask 
				auto bitmask = stub.CreateAdd(offset, llvm::ConstantInt::get(ty.i64, this->GetBitmaskSize()));

				// align
				auto align = stub.CreateIntToPtr(
						stub.CreateAnd(
							stub.CreateAdd(bitmask, stub.getInt64(31)), 
							stub.CreateNot(stub.getInt64(31))),
						stub.getInt8PtrTy()->getPointerTo());

				align->setName("symbol_table");

				// align symbol table
				stub.CreateRet(stub.CreatePtrToInt(
					stub.CreateGEP(align, (llvm::Argument*)sizeOfStateStub->arg_begin()),
					stub.getInt64Ty()));
			}));

			ir.API(ir.implement(sizeOfStub, [&](auto stub, auto args) {
				// commit size variables
				auto null0(llvm::Constant::getNullValue(ty.ptr));
				stub.CreateCall(sizeOfFunc, { null0, null0, null0, null0 });
				// measure total size
				auto null(llvm::Constant::getNullValue(llvm::Type::getInt8PtrTy(Context)->getPointerTo()));
				auto offset(stub.CreateGEP(null, stub.getInt32(1)));
				auto ptrSize(stub.CreatePtrToInt(offset, stub.getInt64Ty()));
				auto stateSize(stub.CreateCall(sizeOfStateStub, { stub.getInt64(0) }, "sizeof_state"));
				stub.CreateRet(stub.CreateAdd(stateSize, stub.CreateMul(ptrSize, stub.getInt64(this->GetNumSymbols()))));
			}));

			// make symbol table
#define F(T, L) T
#define COMMA ,
			auto symTy = ir.finalizeStruct<krt_sym, KRT_SYM_SPEC(COMMA)>("krt_sym", true);
			std::vector<llvm::Constant*> symTableEntry;
			// add input-less drivers to method table

			auto methods = globalKeyTable;
			for (auto ic : inputCall) {
				auto method = methods.find(ic.first);
				if (method == methods.end()) {
					methods.emplace(ic.first, GlobalVarData{
						nullptr,
						K3::Type::Nil,
						GlobalVarType::External,
						std::make_pair(1, 1),
						K3::Type::Nil
					});
				}
			}

			methods.erase(Type::Pair(Type("unsafe"), Type("accumulator")));
			int maxNoDefaultSlot = -1;
			for (auto& gv : methods) {
				std::stringstream sym;
				sym << gv.first;

				if (sym.str() != "arg") {
					auto triggerCallback = Constant::getNullValue(LLVMGen::TypeGen::helper<krt_process_call>::get(ir));

					auto trigger = inputCall.find(gv.first);
					if (trigger != inputCall.end()) {
						triggerCallback = trigger->second;
					}

					std::stringstream descr;
					gv.second.data.OutputJSONTemplate(descr, false);

					auto slotI = globalSymbolTable.find(gv.second.uid);
					auto slotIndex = slotI != globalSymbolTable.end() ? std::int32_t(slotI->second) : -1;

					bool constructorParameter =
						((gv.second.varType == External || 
						  gv.second.varType == Configuration) 
						 && globalKeyTable.find(gv.first) != globalKeyTable.end());
                    
					if (slotIndex >= 0) {
						std::stringstream driverName;
						if (gv.second.varType == Stream) {
							gv.second.clock.OutputText(driverName);
						}
						cppHeader.DeclareSlot(sym.str(), slotIndex, gv.second.data, constructorParameter, driverName.str());
					}
							
					if (trigger != inputCall.end()) {
						cppHeader.DeclareDriver(trigger->second->getName());
					}
                    
                    bool noDefaultVal = constructorParameter || gv.second.varType == UnsafeExternal;
				
					symTableEntry.emplace_back(
						ConstantStruct::get(symTy, {
							ir.constant(sym.str()),
							ir.constant(descr.str()),
							triggerCallback,
							ir.constant(std::int64_t(gv.second.data.GetSize())),
							ir.constant(slotIndex),
							ir.constant(
								(noDefaultVal ? KRT_FLAG_NO_DEFAULT : 0) |
								(gv.second.varType == Stream ? KRT_FLAG_BLOCK_INPUT : 0))
						}));

					if (noDefaultVal && slotIndex > maxNoDefaultSlot) {
						maxNoDefaultSlot = slotIndex;
					}
				}
			}

#undef F
#define F(T, L) TypeResolver<T>::get(ir)
			auto classTy = TypeResolver<krt_class>::get(ir);
			classTy->setName("krt_class");
			classTy->setBody({
				KRT_CLASS_SPEC(COMMA),
				ArrayType::get(symTy, symTableEntry.size()),
			}, true);
#undef COMMA
#undef F

			auto symTableTy = ArrayType::get(symTy, symTableEntry.size());
			auto symTableInit = llvm::ConstantArray::get(symTableTy, symTableEntry);

			auto initTableTy = ArrayType::get(ty.ptr, maxNoDefaultSlot + 1);
			auto initTableInit = llvm::ConstantArray::getNullValue(initTableTy);

			auto slotInitializerData = ir.def(Linkage::Internal, initTableTy, "ExternalInit", false);
			slotInitializerData->setInitializer(initTableInit);

			ir.implement((llvm::Function*)M->getOrInsertFunction("GetConfigurationSlot", TypeResolver<void*(std::int32_t)>::get(ir)),
							[&](IRBuilder<>& stub, auto args) {
				auto ai = args.begin();
				auto index = (llvm::Argument*)ai++;
				auto slotPtr = stub.CreateGEP(slotInitializerData, { stub.getInt32(0), index });
				stub.CreateRet(stub.CreateLoad(slotPtr));
			});

			auto configureCall = ir.API(ir.defn<krt_configure_call>(Linkage::Export, "SetConfigurationSlot",
																	[&](IRBuilder<>& stub, auto args) {
				auto ai = args.begin();
				auto index = (llvm::Argument*)ai++;
				auto data = (llvm::Argument*)ai++;
				auto slotPtr = stub.CreateGEP(slotInitializerData, { stub.getInt32(0), index });
				stub.CreateStore(data, slotPtr);
				stub.CreateRetVoid();
			}));

			std::stringstream evalArg, resultTy;
			GetArgumentType().OutputJSONTemplate(evalArg, false);
			GetResultType().OutputJSONTemplate(resultTy, false);

			auto classInit = ConstantStruct::get(
				classTy, {
					configureCall,
					sizeOfStub,
					initStub,
					getValStub,
					evalStub,
					deinitStub,
					ir.nullConstant<krt_dispose_class_call>(),
					ir.constant(evalArg.str()),
					ir.constant(resultTy.str()),
					ir.nullConstant<void*>(),
					ir.constant(std::int64_t(GetArgumentType().GetSize())),
					ir.constant(std::int64_t(GetResultType().GetSize())),
					ir.constant(std::int32_t(symTableEntry.size())),
					symTableInit
				});

			auto classData = ir.def(Linkage::Internal, classTy, "Class", false);
			classData->setInitializer(classInit);

			ir.defn<krt_metadata_call>(Linkage::Export, "GetClassData", [&](auto& ir, auto args) {
				ir.CreateRet(classData);
			});

			GetModule()->getFunction("__launch")->removeFromParent();
#ifndef NDEBUG
			std::string errstr;
			llvm::raw_string_ostream er{ errstr };
			if (llvm::verifyModule(*GetModule(), &er)) {
				std::clog << errstr;
				errstr.clear();
				er << *GetModule();
				std::clog << errstr << std::endl;
				INTERNAL_ERROR("Compiler fronted emitted invalid IR");
			}
#endif
		}

		static bool DoClockCounters(llvm::IRBuilder<>& stub, llvm::Value* selfPtr, const CounterIndiceSet& indices, const Reactive::DriverSet& ds) {
			std::unordered_map<int, llvm::Value*> counterBits;

			auto self = stub.CreateBitCast(selfPtr, stub.getInt32Ty()->getPointerTo());

			for (auto idx : indices) {
				if (idx.second.GetDivider() > 1 && ds.find(idx.first)) {
					auto counterPtr = stub.CreateConstGEP1_32(selfPtr, idx.second.GetIndex());
					int divider = idx.second.GetDivider();

					auto Counter = stub.CreatePtrToInt(stub.CreateLoad(counterPtr, false), stub.getInt32Ty());
					auto CounterIsZero = stub.CreateICmpEQ(Counter, stub.getInt32(0));
					Counter = stub.CreateSelect(CounterIsZero, stub.getInt32(divider), Counter);
					Counter = stub.CreateSub(Counter, stub.getInt32(1));

					stub.CreateStore(stub.CreateIntToPtr(Counter, stub.getInt8PtrTy()), counterPtr, false);

					assert(idx.second.BitMaskIndex() >= 0 && "Bit mask not assigned to counter");
					int wordIdx = idx.second.BitMaskIndex() / 32;
					int subIdx = idx.second.BitMaskIndex() % 32;
					int outSubIdx(subIdx);
					if (counterBits[wordIdx] == nullptr) {
						counterBits[wordIdx] = Backends::GetSignalMaskWord(stub, self, idx.second.BitMaskIndex(), outSubIdx);
						assert(outSubIdx == subIdx && "Bit allocation mismatch between moduler builder and compiler");
					}

					auto WideTruth = stub.CreateSExt(CounterIsZero, stub.getInt32Ty());
					auto SwitchMask = stub.getInt32(1 << outSubIdx);
					//                auto self = stub.CreateBitCast(selfPtr, stub.getInt32Ty()->getPointerTo());
					auto SetBit = stub.CreateXor(counterBits[wordIdx],
						stub.CreateAnd(
							stub.CreateXor(WideTruth, counterBits[wordIdx]), SwitchMask));
					counterBits[wordIdx] = SetBit;
				}
			}

			for (auto cb : counterBits) {
				Backends::StoreSignalMaskWord(stub, self, cb.first * 32, cb.second);
			}

			return counterBits.size() != 0;
		}

		llvm::Function* LLVM::GetPartialSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indices, int longCounterTreshold) {
			llvm::Function *result = nullptr;

			CounterIndiceSet longCounters;
			if (longCounterTreshold) {
				for (auto &c : indices) {
					if (c.second.GetDivider() >= longCounterTreshold) longCounters.insert(c);
				}
			}

			if (longCounters.empty()) {
				// build activation state with this set of drivers and counter indices
				CompilationPass pass(*this, indices, name, graph, Backends::BuilderPass::Reactive);
				drivers.for_each([&pass](const Type& d) { pass.insert(d); });
				result = pass;
			} else {
				CounterIndiceSet shortCounters;
				Reactive::DriverSet shortDrivers;
				drivers.for_each([&](const Type &d) {
					auto f = indices.find(d);
					if (f == indices.end() || f->second.GetDivider() < longCounterTreshold) {
						shortDrivers.insert(d);
						if (f != indices.end()) {
							shortCounters.insert(*f);
						}
					}
				});

				llvm::Function* shortCounterActivation = GetPartialSubActivation("Common_" + name, graph, shortDrivers, shortCounters, longCounterTreshold);

				// if there is only one long counter index, its state is known statically due to the general long counter test. 
				// in that case, generate partial activation with that index mask omitted (always active)
				llvm::Function* longCounterActivation = GetPartialSubActivation("Rare_" + name, graph, drivers, longCounters.size() > 1 ? indices : shortCounters, 0);

				llvm::Function* longCounterTest = llvm::Function::Create(shortCounterActivation->getFunctionType(), llvm::GlobalValue::LinkageTypes::PrivateLinkage, "CheckLong_" + name, GetModule().get());
				longCounterTest->setCallingConv(llvm::CallingConv::Fast);
				longCounterTest->setDoesNotThrow();

				llvm::BasicBlock* top = llvm::BasicBlock::Create(GetContext(), "Top", longCounterTest);
				llvm::IRBuilder<> b(top);

				auto ai = longCounterTest->arg_begin();

				auto basePtr = b.CreateBitCast((llvm::Argument*)ai, b.getInt8PtrTy()->getPointerTo());
				llvm::Value* needsLongCounters = b.getInt1(false);
				for (auto &c : longCounters) {
					auto counter = b.CreatePtrToInt(b.CreateLoad(b.CreateConstGEP1_32(basePtr, c.second.GetIndex()), false), b.getInt32Ty());
					needsLongCounters = b.CreateOr(needsLongCounters, b.CreateICmpSLE(counter, b.getInt32(0)));
				}

				auto callLong = llvm::BasicBlock::Create(GetContext(), "Long", longCounterTest);
				auto callShort = llvm::BasicBlock::Create(GetContext(), "Short", longCounterTest);
				auto epilog = llvm::BasicBlock::Create(GetContext(), "End", longCounterTest);
				b.CreateCondBr(needsLongCounters, callLong, callShort);

				std::vector<llvm::Value*> args;
                for (auto& sca: shortCounterActivation->args()) {
                    (void)sca;
                    args.emplace_back((llvm::Argument*)ai++);
                }

				b.SetInsertPoint(callLong);
				auto longValue = b.CreateCall(longCounterActivation, args, "long_activation");
				longValue->setCallingConv(llvm::CallingConv::Fast);
				longValue->setDoesNotThrow();
				b.CreateBr(epilog);

				b.SetInsertPoint(callShort);
				auto shortValue = b.CreateCall(shortCounterActivation, args, "short_activation");
				shortValue->setCallingConv(llvm::CallingConv::Fast);
				shortValue->setDoesNotThrow();
				b.CreateBr(epilog);

				b.SetInsertPoint(epilog);
				auto resultPhi = b.CreatePHI(shortValue->getType(), 2);
				resultPhi->addIncoming(longValue, callLong);
				resultPhi->addIncoming(shortValue, callShort);
				b.CreateRet(resultPhi);

				result = longCounterTest;
#ifndef NDEBUG
				if (llvm::verifyFunction(*result, &llvm::errs())) {
					llvm::errs() << "*** Broken Function Starts ***\n";
					llvm::errs() << *result;
					llvm::errs() << "*** Broken Function Ends   ***\n";
					INTERNAL_ERROR("Compiler frontend emitted invalid IR");
				}
#endif
			}
			return result;
		}

		llvm::Function* LLVM::GetSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indices, int longCounterTreshold) {
			auto f(activations.find(drivers));
			if (f != activations.end()) {
				return f->second;
			}

			// get the code to activate graph
			llvm::Function* activationState = GetPartialSubActivation(name, graph, drivers, indices, longCounterTreshold);

			// add stream input iterators				
			auto result = llvm::Function::Create(activationState->getFunctionType(), llvm::GlobalValue::InternalLinkage, name + "_schd", GetModule().get());
							result->addFnAttr(llvm::Attribute::AttrKind::NoInline);
			llvm::IRBuilder<> stub(llvm::BasicBlock::Create(GetContext(), "top", result));

			// process clock counters
			DoClockCounters(stub, stub.CreateBitCast((llvm::Argument*)result->arg_begin(), stub.getInt8PtrTy()->getPointerTo()), indices, drivers);

			std::vector<llvm::Value*> passArgs;
			for (auto ai = result->arg_begin(); ai != result->arg_end(); ++ai) passArgs.push_back((llvm::Argument*)ai);
			auto retval = stub.CreateCall(activationState, passArgs, "process");
			retval->setCallingConv(llvm::CallingConv::Fast);
			retval->setDoesNotThrow();

			for (auto& vk : globalKeyTable) {
				drivers.for_each([&](const Type& d) {
					DriverSignature dsig = d;
					if (dsig.GetMetadata() == vk.second.clock &&
						dsig.GetMul() == vk.second.relativeRate.first &&
						dsig.GetDiv() == vk.second.relativeRate.second) {

						// if there's no input associated with the clock, skip
						if (vk.second.uid != nullptr) {
							auto inPtrPtr = stub.CreateConstGEP1_32(stub.CreateBitCast((llvm::Argument*)result->arg_begin(), stub.getInt8PtrTy()->getPointerTo()), GetIndex(vk.second.uid));
							auto f = indices.find(d);
							if (f != indices.end()) {
								int subidx(-1);
								llvm::Value* mask = K3::Backends::GetSignalMaskWord(stub, (llvm::Argument*)result->arg_begin(), f->second.BitMaskIndex(), subidx);
								auto test = stub.CreateICmpNE(stub.CreateAnd(mask, stub.getInt32(1 << subidx)), stub.getInt32(0));
								llvm::BasicBlock* increment = llvm::BasicBlock::Create(GetContext(), "stream_iteration", result);
								llvm::BasicBlock* noinc = llvm::BasicBlock::Create(GetContext(), "stream_iteration_end", result);
								stub.CreateCondBr(test, increment, noinc);
								stub.SetInsertPoint(increment);
								stub.CreateStore(stub.CreateConstGEP1_32(stub.CreateLoad(inPtrPtr), vk.second.data.GetSize()), inPtrPtr);
								stub.CreateBr(noinc);
								stub.SetInsertPoint(noinc);
							} else {
								stub.CreateStore(stub.CreateConstGEP1_32(stub.CreateLoad(inPtrPtr), vk.second.data.GetSize()), inPtrPtr);
							}
						}
					}
				});
			}

			stub.CreateRet(retval);

			activations.insert(std::make_pair(drivers, result));
			return result;
		}

		llvm::Function* LLVM::CombineSubActivations(const std::string& name, const std::vector<llvm::Function*>& superClockFrames) {
			if (superClockFrames.size() == 1) return superClockFrames[0];
			assert(!superClockFrames.empty());
			LLVMUtil::LLVMGen ir{ GetContext(), GetModule().get() };
			return ir.defn<void*(void*,void*,void*,void*)>(LLVMUtil::Internal, name + "_super", [&](auto& b, auto args) {
				auto ai = args.begin();
				auto self = (llvm::Argument*)ai++;
				auto state = (llvm::Argument*)ai++;
				auto input = (llvm::Argument*)ai++;
				auto output = (llvm::Argument*)ai++;
				llvm::CallInst *returnValue = nullptr;
				for (auto &sf : superClockFrames) {
					returnValue = b.CreateCall(sf, { self, state, input, output });
					returnValue->setCallingConv(CallingConv::Fast);
				}
				b.CreateRet(returnValue);
			});

		}

		llvm::Function* LLVM::GetActivation(const std::string& nameTemplate, CTRef graph, const Type& signature, llvm::Function *sizeOfStateStub, llvm::Function *sizeOfStub) {
			int jitter(0), tmp(0);

			ActivationMatrix Activation = GetActivationMatrix(signature, 1, jitter);
			const int vectorIterationSize = ComputeAuspiciousVectorLength(Activation, 16);
			if (vectorIterationSize > 1) {
				auto amtx = GetActivationMatrix(signature, vectorIterationSize, tmp);
				Activation = CombineRows(amtx, jitter);
			}

			CounterIndiceSet Indices = GetCounterSet(Activation, vectorIterationSize);

			// allocate ivar to track subphase within vectored process
			unsigned vectorSubphaseOffset = GetIndex();

			auto voidPtr(llvm::Type::getInt8PtrTy(Context));

			// assign bitmasks to counters
			int bitMaskIdx = firstCounterBitMaskIndex;
			for (auto &i : Indices) {
				RegisterSignalMaskSlot(bitMaskIdx);
				i.second.BitMaskIndex() = bitMaskIdx++;
			}

			llvm::Type* paramTys[] = { voidPtr, voidPtr, llvm::Type::getInt32Ty(Context) };
			llvm::Type* scalarParamTys[] = { voidPtr, voidPtr, llvm::Type::getInt32Ty(Context), llvm::Type::getInt32Ty(Context) };

			std::vector<llvm::Function*> subFrameFunctions(vectorIterationSize);

			int ticksPerFrame = Activation.size() / vectorIterationSize;

			for (size_t i = 0; i < subFrameFunctions.size(); ++i) {
				std::stringstream frameName;
				frameName << nameTemplate << "_frame_" << i;

				// make a subframe
				std::vector<llvm::Function*> superClockFrames;
				for (int j = 0; j < ticksPerFrame; ++j) {
					Reactive::DriverSet activationState;
					for (auto& m : Activation[i*ticksPerFrame + j]) {
						DriverSignature sig(m.GetDriver());
						activationState.insert(sig);
					}
					// superclock may tick multiple times per clock frame
					// due to upsampling
					superClockFrames.emplace_back(
						GetSubActivation(frameName.str() + (j ? "_sf" + std::to_string(j) : ""s), 
										 graph, activationState, Indices, Activation.size() * 4));
				}

				subFrameFunctions[i] = CombineSubActivations(nameTemplate, superClockFrames);
			}

			llvm::Function *vectorDriver = nullptr, *scalarDriver = nullptr;

			vectorDriver = llvm::Function::Create(
				llvm::FunctionType::get(llvm::Type::getVoidTy(Context), paramTys, false),
				llvm::GlobalValue::InternalLinkage,
				nameTemplate + "_vector", GetModule().get()); {

				vectorDriver->setCallingConv(llvm::CallingConv::Fast);
				vectorDriver->setDoesNotThrow();

				auto ai = vectorDriver->arg_begin();
				auto instance = (llvm::Argument*)ai++;
				auto output = (llvm::Argument*)ai++;
				auto loopCount = (llvm::Argument*)ai++;
				assert(ai == vectorDriver->arg_end());

				auto blockHeader = llvm::BasicBlock::Create(Context, "header", vectorDriver);
				auto blockLoop = llvm::BasicBlock::Create(Context, "loopBody", vectorDriver);
				auto blockFooter = llvm::BasicBlock::Create(Context, "footer", vectorDriver);
				llvm::Value *self_ptr = nullptr, *arg_ptr = nullptr;
				llvm::IRBuilder<> b(blockHeader); {
					self_ptr = b.CreateBitCast(b.CreateGEP((llvm::Argument*)instance, b.CreateCall(sizeOfStateStub, { b.getInt64(0) }, "sizeof_state")), b.getInt8PtrTy());
					self_ptr->setName("self");
					if (GetArgumentType().GetSize()) {
						arg_ptr = b.CreateLoad(b.CreateConstGEP1_32(b.CreateBitCast(self_ptr, b.getInt8PtrTy()->getPointerTo()), GetArgumentIndex()));
					}
					else {
						arg_ptr = llvm::UndefValue::get(b.getInt8PtrTy());
					}
					b.CreateCondBr(
						b.CreateICmpNE(loopCount, b.getInt32(0)),
						blockLoop, blockFooter);
				}

				b.SetInsertPoint(blockLoop); {
					auto counter = b.CreatePHI(b.getInt32Ty(), 2, "count");
					auto out = b.CreatePHI(voidPtr, 2, "out");

					counter->addIncoming(loopCount, blockHeader);
					out->addIncoming(output, blockHeader);

					// provide output as input if mix bus is requested. 
					// this violates some of the noalias we claim, have to see
					// if it miscompiles stuff
					auto mixBusKey = globalKeyTable.find(Type::Pair(Type("unsafe"), Type("accumulator")));
					llvm::Value* mixBusPtr = nullptr;
					if (mixBusKey != globalKeyTable.end()) {
						auto mixBusSlot = GetIndex(mixBusKey->second.uid);
						auto slotPtr = b.CreateBitCast(self_ptr, b.getInt8PtrTy()->getPointerTo());
						mixBusPtr = b.CreateConstGEP1_32(slotPtr, mixBusSlot);
					}

					for (size_t i(0); i < subFrameFunctions.size(); ++i) {
						auto outPtr = b.CreateConstGEP1_32(out, i*GetResultType().GetSize());
						if (mixBusPtr) b.CreateStore(outPtr, mixBusPtr);
						b.CreateCall(subFrameFunctions[i], { self_ptr, instance, arg_ptr,
							 outPtr }, "subframe")->setCallingConv(llvm::CallingConv::Fast);
					}

					auto next_counter = b.CreateSub(counter, b.getInt32(1));
					counter->addIncoming(next_counter, blockLoop);
					out->addIncoming(b.CreateConstGEP1_32(out, vectorIterationSize*GetResultType().GetSize()), blockLoop);
					b.CreateCondBr(b.CreateICmpNE(next_counter, b.getInt32(0)), blockLoop, blockFooter);
				}

				b.SetInsertPoint(blockFooter); {
					b.CreateRetVoid();
				}
			}

			if (vectorIterationSize > 1) {
				scalarDriver = llvm::Function::Create(
					llvm::FunctionType::get(llvm::Type::getVoidTy(Context), scalarParamTys, false),
					llvm::GlobalValue::InternalLinkage,
					nameTemplate + "_remainder", GetModule().get()); {

					scalarDriver->setCallingConv(llvm::CallingConv::Fast);
					scalarDriver->setDoesNotThrow();

					auto ai = scalarDriver->arg_begin();
					auto instance = (llvm::Argument*)ai++;
					auto output = (llvm::Argument*)ai++;
					auto loopCount = (llvm::Argument*)ai++;
					auto first_subphase = (llvm::Argument*)ai++;
					assert(ai == scalarDriver->arg_end());

					auto blockHeader = llvm::BasicBlock::Create(Context, "header", scalarDriver);
					auto blockLoop = llvm::BasicBlock::Create(Context, "loopBody", scalarDriver);
					auto blockFooter = llvm::BasicBlock::Create(Context, "footer", scalarDriver);

					llvm::Value* counter = nullptr;
					llvm::Value *self_ptr = nullptr, *arg_ptr = nullptr;
					llvm::Value* mixBusPtr = nullptr;

					llvm::IRBuilder<> b(blockHeader); {
						self_ptr = b.CreateBitCast(b.CreateGEP(instance, b.CreateCall(sizeOfStateStub, { b.getInt64(0) }, "sizeof_state")), b.getInt8PtrTy());
						self_ptr->setName("self");

						if (GetArgumentType().GetSize()) {
							arg_ptr = b.CreateLoad(b.CreateConstGEP1_32(b.CreateBitCast(self_ptr, b.getInt8PtrTy()->getPointerTo()), GetArgumentIndex()));
						}
						else {
							arg_ptr = llvm::UndefValue::get(b.getInt8PtrTy());
						}

						auto mixBusKey = globalKeyTable.find(Type::Pair(Type("unsafe"), Type("accumulator")));
						if (mixBusKey != globalKeyTable.end()) {
							auto mixBusSlot = GetIndex(mixBusKey->second.uid);
							auto slotPtr = b.CreateBitCast(self_ptr, b.getInt8PtrTy()->getPointerTo());
							mixBusPtr = b.CreateConstGEP1_32(slotPtr, mixBusSlot);
						}

						b.CreateCondBr(
							b.CreateICmpNE(loopCount, b.getInt32(0)),
							blockLoop, blockFooter);
					}

					b.SetInsertPoint(blockLoop); {

						auto blockSubframe0 = llvm::BasicBlock::Create(Context, "subframe", scalarDriver, blockFooter);
						llvm::SwitchInst* frame_switch = b.CreateSwitch(first_subphase, blockSubframe0, subFrameFunctions.size() - 1);

						b.SetInsertPoint(blockSubframe0);

						auto first_counter = b.CreatePHI(b.getInt32Ty(), 2, "counter");
						first_counter->addIncoming(b.getInt32(0), blockLoop);
						counter = b.CreateAdd(first_counter, b.getInt32(1));

						auto frameOut = b.CreateGEP(output, b.CreateMul(first_counter, b.getInt32(GetResultType().GetSize())));
						if (mixBusPtr) b.CreateStore(frameOut, mixBusPtr);
						auto sfcall = b.CreateCall(subFrameFunctions[0], { self_ptr, instance, arg_ptr, frameOut }, "subframe");
						sfcall->setCallingConv(llvm::CallingConv::Fast);

						for (size_t i(1); i < subFrameFunctions.size(); ++i) {
							auto blockSubframe = llvm::BasicBlock::Create(Context, "subframe", scalarDriver, blockFooter);
							frame_switch->addCase(b.getInt32(i), blockSubframe);

							auto prevBlock = b.GetInsertBlock();

							b.CreateCondBr(
								b.CreateICmpULT(counter, loopCount),
								blockSubframe, blockFooter);

							b.SetInsertPoint(blockSubframe);

							auto frame_counter = b.CreatePHI(b.getInt32Ty(), 2, "counter");
							frame_counter->addIncoming(counter, prevBlock);
							frame_counter->addIncoming(b.getInt32(0), blockLoop);
							counter = b.CreateAdd(frame_counter, b.getInt32(1));

							auto frameOut = b.CreateGEP(output, b.CreateMul(frame_counter, b.getInt32(GetResultType().GetSize())));
							if (mixBusPtr) b.CreateStore(frameOut, mixBusPtr);
							auto sfcall = b.CreateCall(subFrameFunctions[i], { self_ptr, instance, arg_ptr, frameOut }, "subframe");
							sfcall->setCallingConv(llvm::CallingConv::Fast);
						}

						first_counter->addIncoming(counter, b.GetInsertBlock());
						b.CreateCondBr(
							b.CreateICmpULT(counter, loopCount),
							blockSubframe0, blockFooter);
					}

					b.SetInsertPoint(blockFooter); {
						b.CreateRetVoid();
					}
#ifndef NDEBUG
					if (llvm::verifyFunction(*scalarDriver, &llvm::errs())) {
						llvm::errs() << "*** Broken Scalar Driver Starts ***\n";
						llvm::errs() << *scalarDriver;
						llvm::errs() << "*** Broken Function Ends   ***\n";
						INTERNAL_ERROR("Compiler frontend emitted invalid IR");
					}
#endif
				}
			}

#ifndef NDEBUG
			if (llvm::verifyFunction(*vectorDriver, &llvm::errs())) {
				llvm::errs() << "*** Broken Vector Driver Starts ***\n";
				llvm::errs() << *vectorDriver;
				llvm::errs() << "*** Broken Function Ends   ***\n";
				INTERNAL_ERROR("Compiler frontend emitted invalid IR");
			}
#endif


			llvm::Function *driverStub(
				llvm::Function::Create(
					llvm::FunctionType::get(llvm::Type::getVoidTy(Context), paramTys, false),
					llvm::GlobalValue::ExternalLinkage,
					nameTemplate, GetModule().get()));
			{
				auto ai = driverStub->arg_begin();
				auto instance = (llvm::Argument*) ai++;
				llvm::Value* output = (llvm::Argument*)ai++;
				llvm::Value* loopCount = (llvm::Argument*)ai++;
				llvm::Value *self_ptr = nullptr, *subphase = nullptr, *subphase_ptr = nullptr;

				llvm::Value *prealign_out = nullptr, *prealign_count = nullptr,
					*vector_out = nullptr, *vector_count = nullptr,
					*remainder_out = nullptr, *remainder_count = nullptr;

				auto blockHeader = llvm::BasicBlock::Create(Context, "header", driverStub);
				llvm::IRBuilder<> b(blockHeader);

				// alloca a scratch buffer if output is null
				auto outputNotNull = b.CreateICmpNE(output, llvm::Constant::getNullValue(output->getType()));
				auto scratch = b.CreateAlloca(b.getInt8Ty(), b.CreateSelect(outputNotNull, b.getInt32(0), 
									b.CreateMul(b.getInt32(GetResultType().GetSize()), loopCount)));
				output = b.CreateSelect(outputNotNull, output, scratch);

				self_ptr = b.CreateBitCast(b.CreateGEP(instance, b.CreateCall(sizeOfStateStub, { b.getInt64(0) }, "sizeof_state")), b.getInt8PtrTy());
				self_ptr->setName("self");


				auto blockVectorLoop = llvm::BasicBlock::Create(Context, "vector_loop", driverStub);

				if (vectorIterationSize > 1) {
					// may need prealign and remainder

					subphase_ptr = b.CreateConstGEP1_32(b.CreateBitCast(self_ptr, b.getInt8PtrTy()->getPointerTo()), vectorSubphaseOffset);
					auto load = b.CreateLoad(subphase_ptr, false);
					subphase = b.CreatePtrToInt(load, b.getInt32Ty());
					subphase = b.CreateURem(subphase, b.getInt32(vectorIterationSize));
					subphase->setName("sub_phase");
					b.CreateStore(b.CreateIntToPtr(b.CreateAdd(subphase, loopCount), b.getInt8PtrTy()), subphase_ptr);

					prealign_count = b.CreateSelect(b.CreateICmpNE(subphase, b.getInt32(0)), b.CreateSub(b.getInt32(vectorIterationSize), subphase), b.getInt32(0));
					prealign_count = b.CreateSelect(b.CreateICmpUGT(prealign_count, loopCount), loopCount, prealign_count);
					prealign_out = output;

					vector_count = b.CreateUDiv(b.CreateSub(loopCount, prealign_count), b.getInt32(vectorIterationSize));
					vector_out = b.CreateGEP(prealign_out, b.CreateMul(prealign_count, b.getInt32(GetResultType().GetSize())));

					remainder_count = b.CreateSub(loopCount, b.CreateAdd(prealign_count, b.CreateMul(vector_count, b.getInt32(vectorIterationSize))));
					remainder_out = b.CreateGEP(output, b.CreateMul(b.CreateSub(loopCount, remainder_count), b.getInt32(GetResultType().GetSize())));

					auto blockPrealign = llvm::BasicBlock::Create(Context, "prealign_loop", driverStub, blockVectorLoop);

					b.CreateCondBr(
						b.CreateICmpNE(prealign_count, b.getInt32(0)),
						blockPrealign, blockVectorLoop);

					b.SetInsertPoint(blockPrealign);

					b.CreateCall(scalarDriver, { instance, prealign_out, prealign_count, subphase })->setCallingConv(llvm::CallingConv::Fast);

					b.CreateBr(blockVectorLoop);
					b.SetInsertPoint(blockVectorLoop);
					output = b.CreateGEP(output, b.CreateMul(prealign_count, b.getInt32(GetResultType().GetSize())));
				} else {
					vector_count = loopCount;
					vector_out = output;
					b.CreateBr(blockVectorLoop);
					b.SetInsertPoint(blockVectorLoop);

				}

				b.CreateCall(vectorDriver, { instance, vector_out, vector_count })->setCallingConv(llvm::CallingConv::Fast);

				if (vectorIterationSize > 1) {
					auto blockRemainder = llvm::BasicBlock::Create(Context, "remainder_loop", driverStub);
					auto blockFooter = llvm::BasicBlock::Create(Context, "footer", driverStub);
		
					b.CreateCondBr(
						b.CreateICmpNE(remainder_count, b.getInt32(0)),
						blockRemainder,
						blockFooter);

					b.SetInsertPoint(blockRemainder);

					b.CreateCall(scalarDriver, { instance, remainder_out, remainder_count, b.getInt32(0) })->setCallingConv(llvm::CallingConv::Fast);
					b.CreateRetVoid();
					b.SetInsertPoint(blockFooter);
				}

				b.CreateRetVoid();
			}

#ifndef NDEBUG
			if (llvm::verifyFunction(*driverStub, &llvm::outs())) {
				llvm::outs() << "*** Broken Function Starts ***\n";
				llvm::outs() << *driverStub;
				llvm::outs() << "*** Broken Function Ends   ***\n";
				INTERNAL_ERROR("Compiler frontend emitted invalid IR");
			}
#endif
			return driverStub;
		}

		static llvm::LLVMContext& AcquireContext() {
			static const char *Key = "LLVMContext";
			Ref<ManagedObject> llvmContext = TLS::GetCurrentInstance()->Get(Key);
			LLVMContextHolder* cx;
			if (llvmContext.NotNull()) {
				llvmContext->Cast(cx);
				assert(cx && "TLS 'LLVMContext' is of wrong type");
				return cx->context;
			}
			else {
				llvmContext = new LLVMContextHolder;
				TLS::GetCurrentInstance()->Set(Key, llvmContext);
				llvm::remove_fatal_error_handler();


				return llvmContext->Cast<LLVMContextHolder>()->context;
			}
		}

		LLVM::LLVM(CTRef AST, const Type& argType, const Type& resType)
			:CodeGenModule(argType, resType),
			Context(AcquireContext()),
			M(new llvm::Module("kronos", Context)) 
		{
			StandardBuild(AST, argType, resType);
		}

		void LLVM::Build(Kronos::BuildFlags flags) {
			RegionAllocator alloc;

			intermediateAST = Graph<Typed>(Backends::SideEffectTransform::Compile(
				*this, intermediateAST, GetArgumentType(), GetResultType()));
			//cout << "[Generating LLVM IR]\n";
			MakeIR(flags);
		}
      
		int LLVMAoT(
			const char* prefix,
			const char* fileType,
			std::ostream& object,
			const char* engine,
			const Kronos::ITypedGraph* itg,
			const char* triple,
			const char* mcpu,
			const char* march,
			const char* targetFeatures,
			Kronos::BuildFlags flags) {
			LLVM compiler(itg->Get(), *itg->_InternalTypeOfArgument(), *itg->_InternalTypeOfResult());
			compiler.AoT(prefix, fileType, object, flags, triple, mcpu, march, targetFeatures);
			return 1;
		}

		krt_class* LLVMJiT(const char* engine,
						   const Kronos::ITypedGraph* itg,
						   Kronos::BuildFlags flags) {
			K3::Backends::LLVM compiler(itg->Get(), *itg->_InternalTypeOfArgument(), *itg->_InternalTypeOfResult());			
			return compiler.JIT(flags);
		}        
	}
}
