#include "binaryen-c.h"
#include "wasm.h"

#include "BinaryenModule.h"
#include "Binaryen.h"
#include "Native.h"
#include "driver/picojson.h"

#include "LLVMCmdLine.h"

void BinaryenEmitWAST(BinaryenModuleRef M, std::ostream&);
void BinaryenEmitAsmJS(BinaryenModuleRef M, std::ostream&);

namespace K3 {
	namespace Backends {
		DynamicScope<BinaryenDataSegment*> DataSegment{ nullptr };

		int BinaryenAoT(
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
			
			Binaryen compiler(BinaryenModuleCreate(), itg->Get(), *itg->_InternalTypeOfArgument(), *itg->_InternalTypeOfResult());
			compiler.AoT(prefix, fileType, object, flags, triple, mcpu, march, targetFeatures);
			return 1;
		}

		void BinaryenSpec::AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char* triple, const char *mcpu, const char *march, const char *mfeat) {
			bool StandaloneModule = false;
			BinaryenExpressionRef StaticData = nullptr;
#ifdef EMSCRIPTEN
			std::clog << "Generating code...\n";
#endif

			if (flags & Kronos::BuildFlags::WasmStandaloneModule) {
				StandaloneModule = true;
			} else {
				BinaryenAddGlobalImport(M, STATIC_DATA, "DS", "addr", BinaryenTypeInt32(), false);
				StaticData = BinaryenGlobalGet(M, STATIC_DATA, BinaryenTypeInt32());
			}

			RegionAllocator alloc;
			StandardBuild(AST, GetArgumentType(), GetResultType());

			intermediateAST = Graph<Typed>(Backends::SideEffectTransform::Compile(
				*this, intermediateAST, GetArgumentType(), GetResultType()));

			FunctionTy evalProc, initProc, sizeProc;

			Backends::AnalyzeCallGraph(0, intermediateAST, cgmap);


			picojson::object slotMetadata;
			auto bind{ DataSegment = &dataSegment };
			auto AddMetadata = [&](const std::string& sym, const picojson::value& data) {
				slotMetadata[sym] = data;
			};

			BinaryenType slotSetterArgTys[] = { BinaryenTypeInt32() };
			BinaryenType slotSetterArgTy = BinaryenTypeInt32();
//			auto slotSetterTy = BinaryenAddFunctionType(M, "SlotSetterTy", BinaryenTypeNone(), slotSetterArgTys, 1);
//			auto slotGetterTy = BinaryenAddFunctionType(M, "SlotGetterTy", BinaryenTypeInt32(), nullptr, 0);

			for (auto &gk : globalKeyTable) {
				auto uid = gk.second.uid;
				auto idx = globalSymbolTable[uid];
				auto nm = "slot" + std::to_string(idx);
				std::stringstream key;
				key << gk.first;
				BinaryenAddGlobal(M, nm.c_str(), BinaryenTypeInt32(), 1, BinaryenConst(M, BinaryenLiteralInt32(0)));

				std::string setter = "set_" + key.str(), getter = "get_" + key.str();

				BinaryenAddFunction(M, setter.c_str(), BinaryenTypeInt32(), BinaryenTypeNone(),
									nullptr, 0,
									BinaryenGlobalSet(M, nm.c_str(),
										BinaryenLocalGet(M, 0, BinaryenTypeInt32())));

				BinaryenAddFunction(M, getter.c_str(), BinaryenTypeNone(), BinaryenTypeInt32(), 
									nullptr, 0,
									BinaryenGlobalGet(M, nm.c_str(), BinaryenTypeInt32()));

				BinaryenAddFunctionExport(M, setter.c_str(), setter.c_str());
				BinaryenAddFunctionExport(M, getter.c_str(), getter.c_str());

				std::stringstream tsig;
				gk.second.data.OutputJSONTemplate(tsig);
				
				AddMetadata(key.str(), 
							picojson::array{
								(double)gk.second.data.GetSize(), tsig.str(), 
								Nodes::GlobalVarTypeName[gk.second.varType]});
			}

			static const char *HEAP_TOP = "HEAP_TOP";

			DriverSet initDrv;
			initDrv.insert(DriverSignature(Type(&Reactive::InitializationDriver)));
			initProc = CompilePass("Init", BuilderPass::Initialization, initDrv);
			sizeProc = CompilePass("SizeOf", BuilderPass::Sizing, initDrv);

			if ((flags & Kronos::OmitEvaluate) == 0) {
				DriverSet evalDrv;
				evalDrv.insert(DriverSignature(Type(&Reactive::ArgumentDriver)));
				evalProc = CompilePass("Eval", BuilderPass::Evaluation, evalDrv);
			}

			using namespace std::string_literals;

			if (evalProc) {
				auto evalExport = CreateFunction(prefix + "Evaluate"s, VoidTy(), { PtrTy(), PtrTy(), PtrTy() }, true);
				{
					BuilderTy b{ evalExport };
					b.Call(evalProc, { b.FnArg(0), b.FnArg(1), b.FnArg(2) }, true);
				}
				evalExport.Complete();
				BinaryenAddFunctionExport(M, evalExport.d->name.c_str(), evalExport.d->name.c_str());
			}

			auto sizeExport = CreateFunction(prefix + "GetSize"s, Int32Ty(), {}, true);
			{
				BuilderTy b{ sizeExport };
				// prepare size variables
				auto null = b.TmpVar(b.Const(0));
				b.Ret(b.PtrToInt(b.PureCall(sizeProc, { null, null, null }, true)));
			}

			auto initExport = CreateFunction(prefix + "Initialize"s, VoidTy(), { PtrTy(), PtrTy(), PtrTy() }, true);
			{
				BuilderTy b{ initExport };
				b.Call(initProc, { b.FnArg(0), b.FnArg(1), b.FnArg(2) }, true);
			}

			if (StandaloneModule) {
				auto addInstance = CreateFunction("AddInstance", Int32Ty(), { }, true);
				{
					BuilderTy b{ addInstance };
					auto inst = b.TmpVar(BinaryenGlobalGet(M, HEAP_TOP, BinaryenTypeInt32()));
					auto sz = b.TmpVar(b.PureCall(sizeExport, {}, false));
					b.Sfx(BinaryenGlobalSet(M, HEAP_TOP, b.AddInt32(inst, sz)));
					auto newSize = b.AddInt32(b.Const(16), b.DivSInt32(sz, b.Const(0x10000)));
					b.Sfx(BinaryenMemoryGrow(M, newSize));
					b.Ret(inst);
				}

				auto unwindStack = CreateFunction("UnwindStack", VoidTy(), {}, true);
				{
					BuilderTy b{ unwindStack };
					b.Set(0, BinaryenGlobalGet(M, HEAP_TOP, BinaryenTypeInt32()));
					b.Ret();
				}

				addInstance.Complete();
				unwindStack.Complete();
				BinaryenAddFunctionExport(M, "AddInstance", "AddInstance");
				BinaryenAddFunctionExport(M, "UnwindStack", "UnwindStack");
				BinaryenAddGlobal(M, HEAP_TOP, BinaryenTypeInt32(), 1, BinaryenConst(M, BinaryenLiteralInt32((int)dataSegment.staticDataOffset)));
			} else {
				BinaryenAddMemoryImport(M, "0", "import", "heap", 0);
			}

			auto getStackPointer = CreateFunction("GetStackPointer", Int32Ty(), {}, true);
			{
				BuilderTy b{ getStackPointer };
				b.Ret(BinaryenGlobalGet(M, STACK_PTR, BinaryenTypeInt32()));
			}

			auto setStackPointer = CreateFunction("SetStackPointer", VoidTy(), { Int32Ty() }, true);
			{
				BuilderTy b{ setStackPointer };
				b.Sfx(BinaryenGlobalSet(M, STACK_PTR, b.FnArg(0)));
				b.RetNoUnwind();
			}

			auto allocaExport = CreateFunction("Alloca", Int32Ty(), { Int32Ty() }, true);
			{				
				BuilderTy b{ allocaExport };
				auto mem = b.TmpVar(b.Alloca(b.FnArg(0), 8));
				b.RetNoUnwind(mem);
			}

			for (auto &exp : { sizeExport, initExport, allocaExport, getStackPointer, setStackPointer }) {
				exp.Complete();
				BinaryenAddFunctionExport(M, exp.d->name.c_str(), exp.d->name.c_str());
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
					name << dn;

					FunctionTy callable{ GetActivation(name.str(), intermediateAST, driver) };

					std::stringstream exportName;
					exportName << driver;
					BinaryenAddFunctionExport(M, name.str().c_str(), ("tick_" + exportName.str()).c_str());
				}			
			}


			size_t dsSize = dataSegment.GenerateStaticMemory(M, StaticData, StandaloneModule ? "mem" : nullptr);
			std::stringstream outSig;
			GetResultType().OutputJSONTemplate(outSig);
			picojson::object metadata{
				{ "Slots", slotMetadata },
				{ "Output", picojson::array{(double)GetResultType().GetSize(), outSig.str() } },
				{ "DS", (double)dsSize }
			};

			auto mod = (wasm::Module*)M;
			wasm::UserSection slotMetadataSection;
			slotMetadataSection.name = "MetaData";
			auto slotMetadataString = picojson::value{ metadata }.serialize();
			slotMetadataSection.data.insert(slotMetadataSection.data.end(), slotMetadataString.begin(), slotMetadataString.end());
			mod->userSections.emplace_back(std::move(slotMetadataSection));

			BinaryenModuleAutoDrop(M);
#ifndef NDEBUG
			if (!BinaryenModuleValidate(M)) {
				std::cerr << "Invalid code emitted!";
			}
			BinaryenModulePrint(M);
#endif
#ifdef EMSCRIPTEN
			std::clog << "Optimizing code...\n";
#endif
			if (CL::OptLevel() > 0) {
				BinaryenSetOptimizeLevel(CL::OptLevel());
				BinaryenSetFastMath(true);
				BinaryenSetAllowInliningFunctionsWithLoops(true);
				BinaryenModuleOptimize(M);
			}

#ifndef NDEBUG
			if (!BinaryenModuleValidate(M)) {
				std::cerr << "Invalid code after optimizer!";
			}
#endif

#ifdef EMSCRIPTEN
			std::clog << "Emitting...\n";
#endif
			if (!strcmp(fileType, ".wast") || !strcmp(fileType, ".s")) {
				BinaryenEmitWAST(M, writeToStream);
			} else if (!strcmp(fileType, ".js")) {
				BinaryenEmitAsmJS(M, writeToStream);
			} else {
				auto wm = BinaryenModuleAllocateAndWrite(M, nullptr);
				std::unique_ptr<void, void(*)(void*)> wmp{ wm.binary, free };
				if (wmp) {
					writeToStream.write((const char*)wmp.get(), wm.binaryBytes);
				}
			}
			std::clog << "Done\n";
		}

		BinaryenFunctionTypeRef BinaryenModule::CreateFunctionTy(BinaryenType retTy, const std::vector<BinaryenType>& params) {
			BinaryenFunctionTypeRef ty;
			ty.argumentType = params;
			ty.returnType = retTy;
			return ty;
		}

		BinaryenSpec::FunctionTy BinaryenSpec::CompilePass(const std::string& name, Backends::BuilderPass passCategory, const DriverSet& drivers) { 
			CounterIndiceSet emptySet;
			return CompilePass(name, passCategory, drivers, emptySet);
		}

		template <typename T>
		static size_t get_hash(T const& v) {
			return std::hash<T>()(v);
		}

		BinaryenSpec::FunctionTy BinaryenSpec::CompilePass(const std::string& name, Backends::BuilderPass passCategory, const DriverSet& drivers, const CounterIndiceSet& counters) {
			using FunctionKey = std::tuple<Graph<const Typed>, FunctionTyTy>;

			struct FunctionKeyHash {

				size_t operator()(const FunctionKey& fk) const {
					auto fty = std::get<FunctionTyTy>(fk);

					auto h = std::get<Graph<const Typed>>(fk)->GetHash() ^ get_hash(fty);

					return h;
				}
			};

			struct Pass : BinaryenTransform::IGenericCompilationPass, CodeGenPass {
				BinaryenSpec& build;
				Backends::BuilderPass passType;

				using FunctionCacheTy = std::unordered_map<FunctionKey, FunctionTy, FunctionKeyHash>;
				FunctionCacheTy cache;
				FunctionTyTy SizingFunctionCacheTy;

				Pass(CTRef ast, BinaryenSpec& s, const std::string& l, Backends::BuilderPass pt, const CounterIndiceSet& counters) :build(s), CodeGenPass(l, ast, counters), passType(pt) {
//					SizingFunctionCacheTy = BinaryenAddFunctionType(s.M, nullptr, BinaryenTypeInt32(), nullptr, 0);
				}

				ModuleTy& GetModule() override {
					return build.M;
				}

				FunctionTy GetMemoized(CTRef body, FunctionTyTy fty) override {
					auto f{ cache.find(FunctionKey{body, fty}) };
					return f != cache.end() ? f->second : FunctionTy{};
				}

				void Memoize(CTRef body, FunctionTyTy fty, FunctionTy fn) override {
					cache.emplace(FunctionKey{ Graph<const Typed>{body}, fty }, fn);
				}

				DriverActivity IsDriverActive(const K3::Type& driverID) override {
					return CodeGenPass::IsDriverActive(driverID);
				}

				Backends::BuilderPass GetPassType() override {
					return passType;
				}

				void SetPassType(Backends::BuilderPass bp) override {
					passType = bp;
				}

				const Backends::CallGraphNode* GetCallGraphAnalysis(const Subroutine* subr) override { return build.GetCallGraphData(subr); }

				const std::string& GetCompilationPassName() override { return label; }

			} codeGenPass{ intermediateAST, *this, name, passCategory, counters };


			drivers.for_each([&codeGenPass](const Type& d) {
				codeGenPass.insert(d);
			});

			auto binding{ Backends::DataSegment = &dataSegment };
			BinaryenTransform codeGen{M, intermediateAST, codeGenPass};
			std::vector<TypeTy> paramTys(3, BinaryenTypeInt32());
			return codeGen.Build(name.c_str(), intermediateAST, paramTys);
		}

		BinaryenModule::BinaryenModule(BinaryenModuleRef M) :M(M) {		
		}

		BinaryenExpressionRef BinaryenEmitter::Alloca(BinaryenExpressionRef sz, int align) {
			Use(sz);
			if (BinaryenExpressionGetType(sz) == BinaryenTypeInt64()) {
				sz = BinaryenUnary(M, BinaryenWrapInt64(), sz);
			}		
			
			auto sp = BinaryenGlobalGet(M, STACK_PTR, BinaryenTypeInt32());

			if (align > 4) {
				sp =  BinaryenBinary(M, BinaryenAndInt32(), 
									 BinaryenBinary(M, BinaryenAddInt32(), sp, BinaryenConst(M, BinaryenLiteralInt32(align - 1))),
									 BinaryenConst(M, BinaryenLiteralInt32(-align)));
			}

			auto mem = TmpVar(sp);
			
			sp = BinaryenBinary(M, BinaryenAddInt32(), mem, sz);
			b->instr.emplace_back(BinaryenGlobalSet(M, STACK_PTR, sp));
			return mem;
		}

		BinaryenDataSegment::StaticDataTy::const_iterator BinaryenDataSegment::AddDataSegment(StaticDataBlob&& blob) {
			int align = 4;
			while (align < 32 && align < blob.data.size()) align *= 2;
			staticDataOffset = (staticDataOffset + align) & (-align);
			size_t bytes = blob.data.size();
			auto seg = staticDataSegments.emplace(std::move(blob), staticDataOffset).first;
			staticDataOffset += bytes;
			return seg;
		}

		static BinaryenExpressionRef Relocatable(BinaryenModuleRef M, BinaryenExpressionRef offset) {
			wasm::Module *mod = (wasm::Module*)M;
			if (auto ds = mod->getGlobalOrNull(STATIC_DATA)) {
				return BinaryenBinary(M, BinaryenAddInt32(), offset, BinaryenGlobalGet(M, STATIC_DATA, BinaryenTypeInt32()));
			}
			return offset;
		}

		BinaryenExpressionRef BinaryenModule::InternZeroBytes(const void *uid, size_t numBytes) {
			auto f = DataSegment()->mutableBlobOffset.find(uid);
			if (f == DataSegment()->mutableBlobOffset.end()) {
				int align = 4;
				while (align < 32 && align < numBytes) align *= 2;
				DataSegment()->staticDataOffset = (DataSegment()->staticDataOffset + align) & (-align);
				f = DataSegment()->mutableBlobOffset.emplace(uid, DataSegment()->staticDataOffset).first;
				DataSegment()->staticDataOffset += numBytes;
			}
			return Relocatable(M, BinaryenConst(M, BinaryenLiteralInt32((int)f->second)));
		}

		BinaryenExpressionRef BinaryenModule::InternConstantBlob(const void* data, size_t numBytes) {
			std::uint8_t *ptr = (std::uint8_t*)data;
			BinaryenDataSegment::StaticDataBlob blob{ { ptr, ptr + numBytes } };
			BinaryenDataSegment::StaticDataTy::const_iterator seg = DataSegment()->staticDataSegments.find(blob);
			
			if (seg == DataSegment()->staticDataSegments.end()) {
				seg = DataSegment()->AddDataSegment(std::move(blob));
			}
			return Relocatable(M, BinaryenConst(M, BinaryenLiteralInt32((int)seg->second)));
		}

		BinaryenExpressionRef BinaryenModule::Intern(const char *str) {
			return InternConstantBlob(str, strlen(str) + 1);
		}

		size_t BinaryenDataSegment::GenerateStaticMemory(BinaryenModuleRef M, BinaryenExpressionRef DataSegmentAddress, const char *memExportName) {
			std::vector<char> dataBlob( staticDataOffset );
			memset(dataBlob.data(), 0, dataBlob.size());
			for (auto &seg : staticDataSegments) {
				auto &data{ seg.first.data };
				auto offset = seg.second;
				memcpy(dataBlob.data() + offset, data.data(), data.size());
			}

			const char *segData[] = {
				dataBlob.data()
			};

			wasm::Module *mod = (wasm::Module*)M;
			BinaryenExpressionRef segOffset[] = {
				mod->getGlobalOrNull(STATIC_DATA) ? BinaryenGlobalGet(M, STATIC_DATA, BinaryenTypeInt32()) : BinaryenConst(M, BinaryenLiteralInt32(0))
			};

			BinaryenIndex segSize[] = {
				(BinaryenIndex)dataBlob.size()
			};

			bool segPassive[] = {
				false
			};

			BinaryenSetMemory(M, (((int)dataBlob.size() + 65536) / 65536), 65535, memExportName, segData, segPassive, segOffset, segSize, 1, 0);
			return dataBlob.size();
		}

		bool BinaryenDataSegment::StaticDataBlob::operator==(const BinaryenDataSegment::StaticDataBlob& rhs) const {
			return std::equal(data.begin(), data.end(), rhs.data.begin(), rhs.data.end());
		}

		size_t BinaryenDataSegment::StaticDataBlob::Hash::operator()(const BinaryenDataSegment::StaticDataBlob& blob) const {
			size_t h(0);
			for (auto b : blob.data) {
				HASHER(h, b);
			}
			return h;
		}

		BinaryenTransform::FunctionTy BinaryenTransform::Build(const char *label, CTRef body, const std::vector<TypeTy>& params) {
			auto buildType{ CreateFunctionTy(PtrTy(), params) };

			auto build = CreateFunction(GetCompilationPass().GetCompilationPassName() + "_" + label, buildType, true);
			
			auto memoized{ GetCompilationPass().GetMemoized(body, buildType) };
			if (memoized) return memoized;

			DriverFilterTy knownActiveMasks{ compilation, currentActivityMask };

			BinaryenTransform subroutineBuilder{
				compilation.GetModule(),
				body,
				(currentActivityMask && currentActivityMask->size()) ? knownActiveMasks : GetCompilationPass()
			};

			return TLS::WithNewStack([&]() {
				subroutineBuilder.BuildSubroutineBody(build, label, body, params);
				return build;
			});
		}

		BinaryenTransform::BinaryenTransform(BinaryenModuleRef M, CTRef root, IGenericCompilationPass& pass)
			:GenericEmitterTransform(pass, root, M) {
		}
	}
}