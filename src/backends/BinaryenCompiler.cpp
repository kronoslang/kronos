#include "GenericCompiler.h"
#include "BinaryenCompiler.h"
#include "Native.h"
#include "NativeVector.h"

#include <sstream>

static size_t AlignPowerOf2(size_t align) {
	if (align < 4) return 4;
	return (align & ~(align - 1));
}

namespace K3 {

	template <typename TXfm> auto static NativeType(TXfm& xfm, const Type& t) {
		if (t.IsFloat32()) return xfm.Float32Ty();
		else if (t.IsFloat64()) return xfm.Float64Ty();
		else if (t.IsInt32()) return xfm.Int32Ty();
		else if (t.IsInt64()) return xfm.Int64Ty();
		else if (t.IsArrayView()) return xfm.PtrTy();
		INTERNAL_ERROR("Illegal unlowered type in generic codegen");
	}

	std::string PassiveDescr(CTRef n) {
		std::stringstream ss;
		ss << *n << "\n" << (void*)n << "\n";
		return ss.str();
	}

	namespace Nodes {
		CODEGEN_EMIT(SubroutineArgument) {
			auto v = xfm->FnArg((int)ID - 1);
			return v;
		}

		CODEGEN_EMIT(BoundaryBuffer) {
			if (avm) {
				return xfm(GetUp(0), avm);
			} else {
				return xfm(GetUp(1), avm);
			}
		}

		CODEGEN_EMIT(SignalMaskSetter) {
			if (avm) {
				auto gateSig{ xfm(GetUp(0)) };
				xfm.StoreSignalMaskBit(bitIdx, xfm->NonZero(gateSig));
			}
			return xfm->UndefConst(xfm.Int32Ty());
		}

		static std::string GetFunctionSizeVarName(const Subroutine* which) {
			// must match GenericEmitterTransform::BuildSubroutineBody
			return "sizeof_" + std::to_string((std::uintptr_t)which->GetBody());
		}

		CODEGEN_EMIT(SubroutineStateAllocation) {
			if (xfm.IsSizingPass()) {
				return (decltype(xfm(this, avm)))subr->Compile(xfm, avm);
			} else {
				return 
					xfm->Offset(
						xfm(GetUp(0)),
						xfm->GVar(GetFunctionSizeVarName(subr), xfm.Int64Ty()));
			}
		}

		CODEGEN_EMIT(Subroutine) {
			using ValueTy = decltype(xfm(GetUp(0)));
			using VarTy = decltype(xfm->TmpVar(xfm(GetUp(0))));
			using TypeTy = decltype(xfm->TypeOf(xfm->Const(0)));

			bool tailCallSafe{ true };
			std::vector<VarTy> params;
			std::vector<TypeTy> paramTypes;

			for (auto up : Upstream()) {
				if (tailCallSafe && xfm.RefersLocalBuffers(up)) {
					tailCallSafe = false;
				}
				params.emplace_back(xfm->TmpVar(xfm(up, avm)));
				paramTypes.emplace_back(xfm->TypeOf(params.back()));
			}

			if (!conditionalRecursionLoopCount) {
				if (avm) {
					auto func{ xfm.Build(GetLabel(), compiledBody, paramTypes) };

					std::vector<ValueTy> pass(params.size());
					for (int i = 0;i < params.size();++i) pass[i] = params[i];

					return xfm->PureCall(func, pass, true);
				} else {
					// just skip state
					auto sz = xfm->GVar(GetFunctionSizeVarName(this), xfm.Int64Ty());
					return xfm->Offset(params[0], sz);
				}
			} else {
				// loop transformation, params will be double-used
				int counterIndex = (int)params.size() - 1;
				auto newIndex = xfm->TmpVar(xfm->AddInt32(xfm->FnArg(counterIndex), xfm->Const(1)));
				auto recurP = xfm->LtSInt32(newIndex, xfm->Const(conditionalRecursionLoopCount));
				params[counterIndex] = newIndex;

				if (tailCallSafe) {
					xfm->TCO(recurP, params);

					params.pop_back();
					paramTypes.pop_back();

					auto recursionEndFn{ xfm.Build("tail", compiledBody, paramTypes) };
					std::vector<ValueTy> pass{ params.size() };
					for (int i = 0;i < params.size();++i) pass[i] = params[i];
					return xfm->PureCall(recursionEndFn, pass, true);
				} else {
					auto stOut = xfm->LVar("", BinaryenTypeInt32());
					xfm->If(recurP, [&,params](auto& then_) mutable {
						std::vector<ValueTy> pass(params.size());
						for (int i = 0;i < params.size();++i) pass[i] = params[i];
						then_.Set(stOut, then_.PureCall(xfm.CurrentFunction(), pass, true));
					}, [&,params](auto& else_) mutable {						
						params.pop_back();
						paramTypes.pop_back();

						auto recursionEndFn{ xfm.Build("tail", compiledBody, paramTypes) };
						std::vector<ValueTy> pass(params.size());
						for (int i = 0;i < params.size();++i) pass[i] = params[i];

						else_.Set(stOut, else_.PureCall(recursionEndFn, pass, true));
					});
					return xfm->Get(stOut, BinaryenTypeInt32());
				}
			}
		}

		CODEGEN_EMIT(SequenceCounter) {
			auto counterIndex = xfm->NumFnArgs(xfm->TypeOf(xfm.CurrentFunction())) - 1;
			return xfm->Coerce(xfm.Int64Ty(), xfm->AddInt32(xfm->FnArg(counterIndex), xfm->Const((int)counter_offset)));
		}

		CODEGEN_EMIT(SizeOfPointer) {
			return xfm->SizeOfPointer();
		}

		CODEGEN_EMIT(Deps) {
			return xfm(GetUp(0));
		}

		CODEGEN_EMIT(PackVector) { INTERNAL_ERROR("Binaryen backend does not support vector instructions"); }
		CODEGEN_EMIT(ExtractVectorElement) { INTERNAL_ERROR("Binaryen backend does not support vector instructions"); }
		CODEGEN_EMIT(MultiDispatch) { INTERNAL_ERROR("Binaryen backend does not support discriminated unions"); }

		// externalasset

		CODEGEN_EMIT(Copy) {
			using TypeTy = decltype(xfm->TypeOf(xfm->Const(0)));
			{
				auto dst = xfm(GetUp(0), avm);
				auto src = xfm(GetUp(1), avm);
				auto sz  = xfm(GetUp(2), avm);
				auto dstVar = xfm->TmpVar(dst);

				if (src && dst && sz) {
					if (!xfm.IsSizingPass() && (avm || xfm.IsInitPass())) {
						switch (mode) {
							case Store:
								xfm->Store(dstVar, src, dstAlign);
								break;
							case MemCpy:
								{
									unsigned offset{ 1 };
									auto align = srcAlign && dstAlign ? AlignPowerOf2(-((-srcAlign) | (-dstAlign))) : 4;

									std::vector<TypeTy> params{ xfm.PtrTy(), xfm.PtrTy(), xfm.Int32Ty(), xfm.Int32Ty() };
									auto memcpyTy = xfm.CreateFunctionTy( xfm.VoidTy(), params );
									auto szVar = xfm->TmpVar(sz);
									xfm->MemCpy(dstVar, src, szVar, (int)align);

									Native::Constant *c;
									if (GetUp(3)->Cast(c) && *(int32_t*)c->GetPointer() < 2) {
										break;
									} else {
										auto repeatVar = xfm->TmpVar(xfm(GetUp(3)));

										auto dstPtrVal = xfm->Offset(dstVar, szVar);
										auto dstPtr = xfm->LVar(dstPtrVal);
										xfm->Set(dstPtr, dstPtrVal);

										auto repeatCount = xfm->SubInt32(xfm(GetUp(3)), xfm->Const(1));

										xfm->Loop(repeatCount, [&](auto &brk, auto& body, auto) {
											body.MemCpy(xfm->Get(dstPtr), dstVar, szVar, (int)align);
											body.Set(dstPtr, xfm->Offset(xfm->Get(dstPtr), szVar));
										});
									}
								}
						}
					}
				}
				return dstVar;
			}
		}

		CODEGEN_EMIT(GetSlot) {
			
			if (xfm.IsInitPass() && GetNumCons()) {
				xfm->SetSlot( index, xfm(GetUp(0)) );
			}

			return xfm->GetSlot(index);
		}

		CODEGEN_EMIT(Configuration) {
			return xfm->GetSlot(slotIndex);
		}

		CODEGEN_EMIT(DerivedConfiguration) {
			std::string cfgName = "dconf_" + std::to_string((std::uintptr_t)cfg);
			if (xfm.IsSizingPass()) {
				xfm.GetCompilationPass().SetPassType(Backends::BuilderPass::InitializationWithReturn);
				auto val = xfm->TmpVar((decltype(xfm(this, avm)))cfg->Compile(xfm, avm));
				xfm.GetCompilationPass().SetPassType(Backends::BuilderPass::Sizing);
				xfm->SetGVar(cfgName, val, true);
				return val;
			} else {
				return xfm->GVar(cfgName, xfm.Int32Ty());
			}
		}

		CODEGEN_EMIT(Buffer) {
			switch (alloc) {
			case Stack:
			case StackZeroed:
				{
					auto sz{ xfm(GetUp(0)) };
					auto buf =  xfm->Alloca(sz, alignment);
					if (alloc == StackZeroed) {
						xfm->MemSet(buf, xfm->Const(0), sz);
					}
					return buf;
				}
			case Module:
				{
					Native::Constant *c{ nullptr };
					GetUp(0)->Cast(c);
					assert(c && "Module buffers must be constant sized");
					return xfm.InternZeroBytes((void*)GUID, *(std::int64_t*)c->GetPointer());
				}
			case Empty:
				return xfm->UndefConst(xfm.PtrTy());
			default:
				std::cerr << "Buffer::alloc = " << alloc << "\n";
				INTERNAL_ERROR("Bad buffer allocation mode");
			}
		}

		static std::int64_t GetConstantOffset(CTRef off) {
			Offset* no;
			Native::Constant* c;
			if (off->Cast(no) && off->GetUp(1)->Cast(c) && c->FixedResult().IsInt64()) {
				return *(std::int64_t*)c->GetPointer();
			} else {
				return -1;
			}
		}

		CODEGEN_EMIT(Offset) {
			return xfm->Offset(xfm(GetUp(0)), xfm(GetUp(1)));
		}

		CODEGEN_EMIT(Dereference) {
			if (!xfm.IsSizingPass() && (avm || xfm.IsInitPass())) {
				if (loadType == Type::Nil) return xfm->UndefConst(xfm.Int32Ty());
				auto nty = loadPtr ? xfm.PtrTy() : NativeType(xfm, loadType);
				return xfm->Load(xfm(GetUp(0)), nty);
			}

			return xfm->PassiveValue(
				NativeType(xfm, loadType),
				PassiveDescr(this)
			);
		}

		CODEGEN_EMIT(AtIndex) {
			return xfm->Offset(
				xfm(GetUp(0)),
					xfm->MulInt32(xfm(GetUp(1)),
								  xfm->Const((int)elem.GetSize())));
		}

		CODEGEN_EMIT(BitCast) {
			return xfm->BitCast( NativeType(xfm, to), xfm(GetUp(0)) );
		}

		CODEGEN_EMIT(CStringLiteral) {
			std::stringstream strlit;
			str.OutputText(strlit);
			return xfm.Intern(strlit.str().c_str());
		}

		CODEGEN_EMIT(ReleaseBuffer) {
			return nullptr;
		}

		CODEGEN_EMIT(Reference) {
			KRONOS_UNREACHABLE;
			return nullptr;
		}

		CODEGEN_EMIT(ExternalAsset) {
			auto& asset{ TLS::GetCurrentInstance()->GetAsset(dataUri) };
			return xfm->GlobalExternal(xfm.PtrTy(), "asset", dataUri);
//			return xfm.InternConstantBlob(asset.memory.get(), asset.type.GetSize());
		}

		namespace ReactiveOperators {
			CODEGEN_EMIT(ClockEdge) {
				auto tmp{ Qxx::FromGraph(GetClock())
					.OfType<Reactive::DriverNode>()
					.Select([](const Reactive::DriverNode* dn) {return dn->GetID(); }).ToVector()
				};

				auto mask = xfm.CollectionToMask(Qxx::From(tmp), false);
				auto isActive = xfm.GenerateNodeActiveFlag(mask);

				if (!isActive) return xfm->AllOnesConst(xfm.Float32Ty());
				return xfm->Select(isActive,
								   xfm->AllOnesConst(xfm.Float32Ty()),
								   xfm->NullConst(xfm.Float32Ty()));

			}
		}

		namespace Native {

			CODEGEN_EMIT(ForeignFunction) {
				using ValueTy = decltype(xfm(GetUp(0)));
				using TypeTy = decltype(xfm->TypeOf(xfm->Const(0)));
				assert(compilerNode);

				if (avm) {
					std::vector<ValueTy> params(GetNumCons());

					for (unsigned int i(0);i < GetNumCons();++i) {
						params[i] = xfm(GetUp(i));
					}

					auto sym = Symbol;
					if (sym.back() == '!') {
						sym.pop_back();
						if (xfm.IsInitPass()) {
							sym.append("_init");
						}
					}

					return xfm->CallExternal(NativeType(xfm, FixedResult()), sym, params);

				}
				return xfm->PassiveValue(
					NativeType(xfm, FixedResult()),
					PassiveDescr(this)
				);
			}

			CODEGEN_EMIT(Select) {
				auto cond{ xfm(GetUp(0)) };
				auto true_{ xfm(GetUp(1)) };
				auto false_{ xfm(GetUp(2)) };
				return xfm->Select(xfm->NonZero(cond), true_, false_);
			}

			CODEGEN_EMIT(ITypedBinary) {
				if (!avm) {
					return xfm->PassiveValue(
						NativeType(xfm, FixedResult()),
						PassiveDescr(this)
					);
				}
				auto lhs{ xfm(GetUp(0)) }, rhs{ xfm(GetUp(1)) };
				return xfm->BinaryOp(GetOpcode(), lhs, rhs);
			}

			CODEGEN_EMIT(ITypedUnary) {
				if (!avm) {
					return xfm->PassiveValue(
						NativeType(xfm, FixedResult()),
						PassiveDescr(this)
					);
				}
				auto up{ xfm(GetUp(0)) };
				return xfm->UnaryOp(GetOpcode(), up);
			}

			CODEGEN_EMIT(Constant) {
				if (type.IsNativeType()) {
					return xfm->Constant(memory, NativeType(xfm, type));
				}
				auto ptr = xfm.InternConstantBlob(memory, type.GetSize());
				return ptr;
			}

			void* BinaryenConversion(Backends::BinaryenTransform& xfm, Backends::ActivityMaskVector* avm, 
													const Type& to, const Type& from, CTRef up) {
				if (!avm) {
					return xfm->PassiveValue(
						NativeType(xfm, to),
						PassiveDescr(up)
					);
				}
				return xfm->Coerce(NativeType(xfm, to), xfm(up));
			}
		}
	}
}