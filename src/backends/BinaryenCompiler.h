#pragma once

#include "BinaryenEmitter.h"
#include "backends/GenericCompiler.h"
#include "Transform.h"

#include "common/DynamicScope.h"

namespace K3 {
	namespace Backends {
		struct BinaryenDataSegment {
			struct StaticDataBlob {
				std::vector<std::uint8_t> data;
				bool operator==(const StaticDataBlob&) const;
				struct Hash {
					size_t operator()(const StaticDataBlob&) const;
				};
			};

			size_t staticDataOffset = 0;
			using StaticDataTy = std::unordered_map<StaticDataBlob, size_t, StaticDataBlob::Hash>;
			std::unordered_map<const void*, size_t> mutableBlobOffset;
			StaticDataTy staticDataSegments;
			StaticDataTy::const_iterator AddDataSegment(StaticDataBlob&&);
			size_t GenerateStaticMemory(BinaryenModuleRef M, BinaryenExpressionRef dataSegmentAddress, const char* memExportName);
		};

		struct BinaryenModule {
			using ModuleTy = BinaryenModuleRef;
			using FunctionTyTy = BinaryenFunctionTypeRef;
			using FunctionTy = BinaryenFunction;
			using TypeTy = BinaryenType;
			using ValueTy = BinaryenExpressionRef;
			using VariableTy = BinaryenValue;
			using BuilderTy = BinaryenEmitter;
			using BlockTy = BinaryenBlock;

			BinaryenModuleRef M;

			BinaryenModule(BinaryenModuleRef M);

			FunctionTyTy CreateFunctionTy(TypeTy ret, const std::vector<TypeTy>& params);

			FunctionTy CreateFunction(const std::string& name, FunctionTyTy ty, bool externalLinkage) {
				return { M, name, externalLinkage, ty };
			}

			FunctionTy CreateFunction(const std::string& name, TypeTy ret, const std::vector<TypeTy>& params, bool externalLinkage) {
				return{ M, name, externalLinkage, CreateFunctionTy(ret, params) };
			}

			FunctionTyTy GetFunctionTy(FunctionTy fn) {
				return fn.d->ty;
			}

			BinaryenExpressionRef Intern(const char*);
			BinaryenExpressionRef InternZeroBytes(const void* uid, size_t numBytes);
			BinaryenExpressionRef InternConstantBlob(const void* data, size_t numBytes);


			TypeTy PtrTy() { return BinaryenTypeInt32(); }
			TypeTy VoidTy() { return BinaryenTypeNone(); }
			TypeTy Int32Ty() { return BinaryenTypeInt32(); }
			TypeTy Int64Ty() { return BinaryenTypeInt64(); }
			TypeTy Float32Ty() { return BinaryenTypeFloat32(); }
			TypeTy Float64Ty() { return BinaryenTypeFloat64(); }
			TypeTy BoolTy() { return Int32Ty(); }
		};

		struct BinaryenCompilerBase : public BinaryenModule, public CodeGenTransformBase {
			BinaryenCompilerBase(CTRef root, BinaryenModuleRef M, ICompilationPass& gp) :BinaryenModule(M), CodeGenTransformBase(gp) { }
			virtual ValueTy operator()(CTRef n) = 0;
			virtual void OpenBranch() = 0;
			virtual void CloseBranch() = 0;
		};

		class BinaryenTransform : public GenericEmitterTransform<BinaryenCompilerBase> {
			struct BinaryenValueState {
				BinaryenValue var;
				bool completed[2] = { false,false };
			};

			using StateMapTy = std::unordered_map<CTRef, BinaryenValueState>;
			StateMapTy values;
			bool inBranch = false;
		public:
			using ModuleTy = BinaryenModuleRef;
			using FunctionTyTy = BinaryenFunctionTypeRef;
			using FunctionTy = BinaryenFunction;
			using TypeTy = BinaryenType;
			using ValueTy = BinaryenExpressionRef;
			using BuilderTy = BinaryenEmitter;
			using BlockTy = BinaryenBlock;
			using DriverFilterTy = GenericEmitterTransform<BinaryenCompilerBase>::GenericDriverActivityFilter;

			void OpenBranch() override {
				assert(!inBranch);
				inBranch = true;
				for (auto& v : values) {
					v.second.completed[0] = v.second.completed[1] = true;
				}
			}

			void CloseBranch() override {
				inBranch = false;
				for (auto& v : values) {
					v.second.completed[0] = v.second.completed[1] = true;
				}
			}

			ValueTy operator()(CTRef n) override { 
				// flatten deps to avoid redundant lvar chains
				if (IsOfExactType<Deps>(n)) {
					n = n->GetUp(0);
				}

				auto& state{ values[n] };

				int activeI = (inBranch && currentActivityMask != nullptr) ? 1 : 0;

				if (state.completed[activeI]) {
					return state.var;
				}

				BinaryenExpressionRef val = (BinaryenExpressionRef)n->Compile(*this, currentActivityMask);
				if (!val) return val;

				if (inBranch) {
					if (!state.completed[0] && !state.completed[1]) {
						auto ty = BinaryenExpressionGetType(val);
						state.var.idx = current.LVar(n->GetLabel(), ty);
						state.var.L.type = ty;
						state.var.M = M;
					} 
					current.Set(state.var.idx, val);
				} else {
					state.var = current.TmpVar(val); //current.LVar(n->GetLabel(),BinaryenExpressionGetType(val));
				}
				
				state.completed[activeI] = true;
				return state.var;

/*
				if (f == values.end() || f->completed.count(currentActivityMask) == 0) {
#if defined(__cpp_exceptions) && __cpp_exceptions == 199711 && !defined(NDEBUG)
					try {
#endif
						BinaryenExpressionRef val = n->Compile(*this, currentActivityMask);
						if (val) {
							values[n] = 
							f = values.emplace(ck, current.TmpVar(val)).first;
						}
						else {
							return val;
						}
#if defined(__cpp_exceptions) && __cpp_exceptions == 199711 && !defined(NDEBUG)
					} catch (std::exception& e) {
						std::cerr << e.what() << " while compiling " << *n << "\n";
						throw;
					}
#endif
				}
				return f->second.value;*/
			}

			ValueTy operator()(CTRef n, ActivityMaskVector* avm) {
				currentActivityMask = avm; auto tmp = (*this)(n);
				return tmp;
			}

			BinaryenTransform(BinaryenModuleRef, CTRef root, IGenericCompilationPass& pass);

			FunctionTy Build(const char *label, CTRef body, const std::vector<TypeTy>& params) override;
		};
	}
}