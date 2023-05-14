#include "BinaryenEmitter.h"
#include "wasm.h"

#include <string>

namespace K3 {
	namespace Backends {
		BinaryenExpressionRef BinaryenEmitter::FnArg(int i, BinaryenType ty) {
			return BinaryenGetLocal(M, i, ty);
		}

		BinaryenExpressionRef BinaryenEmitter::FnArg(int i) {
			return BinaryenGetLocal(M, i, BinaryenFunctionTypeGetParam(fn.d->ty, i));
		}

		int BinaryenEmitter::NumFnArgs() {
			return BinaryenFunctionTypeGetNumParams(fn.d->ty);
		}

		BinaryenExpressionRef BinaryenEmitter::GetSlot(int index) {
			wasm::Module* Mod = (wasm::Module*)M;
			auto name = "slot" + std::to_string(index);
			if (!Mod->getGlobalOrNull(name)) {
				BinaryenAddGlobal(M, name.c_str(), BinaryenTypeInt32(), 1, Const(0));
			}
			return BinaryenGetGlobal(M, name.c_str(), BinaryenTypeInt32());
		}

		static void DeclareGVar(BinaryenModuleRef M, const std::string& name, BinaryenType ty, BinaryenExpressionRef constant = nullptr) {
			wasm::Module* Mod = (wasm::Module*)M;
			if (!Mod->getGlobalOrNull(name)) {
				if (constant) {
					BinaryenAddGlobal(M, name.c_str(), ty, 0, constant);
				} else {
					BinaryenLiteral zeroLit;
					memset(&zeroLit, 0, sizeof(BinaryenLiteral));
					zeroLit.type = ty;
					BinaryenAddGlobal(M, name.c_str(), ty, 1, BinaryenConst(M, zeroLit));
				}
			}
		}

		BinaryenExpressionRef BinaryenEmitter::GVar(const std::string& name, BinaryenType ty) {
			DeclareGVar(M, name, ty);
			b->instr.push_back(BinaryenGetGlobal(M, name.c_str(), ty));
			return b->instr.back();
		}

		void BinaryenEmitter::SetGVar(const std::string& name, BinaryenExpressionRef value, bool soleAssignment) {
			BinaryenExpressionRef constant = nullptr;
			if (soleAssignment && BinaryenExpressionGetId(value) == BinaryenConstId()) {
				DeclareGVar(M, name, BinaryenExpressionGetType(value), value);
			} else {
				DeclareGVar(M, name, BinaryenExpressionGetType(value), constant);
				b->instr.push_back(BinaryenSetGlobal(M, name.c_str(), value));
			}
		}


		void BinaryenEmitter::SetSlot(int index, BinaryenExpressionRef val) {
			Use(val);
			b->instr.emplace_back(BinaryenSetGlobal(M, ("slot" + std::to_string(index)).c_str(), val));
		}

		BinaryenExpressionRef BinaryenEmitter::PtrToInt(BinaryenExpressionRef ptr) { return ptr; }
		BinaryenExpressionRef BinaryenEmitter::IntToPtr(BinaryenExpressionRef int32) { return int32; }

		BinaryenExpressionRef BinaryenEmitter::Offset(BinaryenExpressionRef ptr, BinaryenExpressionRef offset) {
			if (BinaryenExpressionGetType(offset) == BinaryenTypeInt64()) {
				Use(offset);
				if (BinaryenExpressionGetId(offset) == BinaryenConstId()) {
					offset = Const((int32_t)BinaryenConstGetValueI64(offset));
				} else {
					offset = BinaryenUnary(M, BinaryenWrapInt64(), offset);
				}
			}
			return AddInt32(ptr, offset);
		}

		BinaryenExpressionRef BinaryenEmitter::Call(const BinaryenFunction& fn, const std::vector<BinaryenExpressionRef>& params, bool internalCall) {
			if (fn.d != this->fn.d) fn.Complete();
			for (auto &p : params) Use(p);
			auto fncall = BinaryenCall(M, fn.d->name.c_str(), (BinaryenExpressionRef*)params.data(), (int)params.size(), BinaryenFunctionTypeGetResult(fn.d->ty));
			b->instr.emplace_back(fncall);
			return fncall;
		}

		BinaryenExpressionRef BinaryenEmitter::PureCall(const BinaryenFunction& fn, const std::vector<BinaryenExpressionRef>& params, bool internalCall) {
			if (fn.d != this->fn.d) fn.Complete();
#ifndef NDEBUG
			assert(BinaryenFunctionTypeGetNumParams(fn.d->ty) == params.size());
			for (int i = 0;i < params.size();++i) {
				Use(params[i]);
				assert(BinaryenFunctionTypeGetParam(fn.d->ty, i) == BinaryenExpressionGetType(params[i]));
			}
#endif
			auto fncall = BinaryenCall(M, fn.d->name.c_str(), (BinaryenExpressionRef*)params.data(), (int)params.size(), BinaryenFunctionTypeGetResult(fn.d->ty));
			return fncall;
		}



		void BinaryenEmitter::Switch(BinaryenExpressionRef c, const std::vector<BinaryenBlock>& blocks) {
			if (blocks.empty()) return;
			Use(c);

			auto self = BlockName("switch");

			std::vector<std::string> blockName(blocks.size());
			std::vector<const char*> namePtr(blocks.size());
			for (int i = 0; i < blocks.size(); ++i) {
				blockName[i] = self + "_case_" + std::to_string(i);
				namePtr[i] = blockName[i].c_str();
			}

			BinaryenExpressionRef inner = BinaryenSwitch(fn.d->M, namePtr.data(), (int)namePtr.size(), self.c_str(), c, nullptr);
			inner = ::BinaryenBlock(fn.d->M, namePtr[0], &inner, 1, BinaryenTypeNone());

			for (int i = 0;i < blocks.size();++i) {
				BinaryenExpressionRef caseBody[] = {
					inner,
					blocks[i].GetBlock(M, (blockName[i] + "_body").c_str())
				};
				if (i < blocks.size() - 1) {
					inner = ::BinaryenBlock(fn.d->M, namePtr[i + 1], caseBody, 2, BinaryenTypeNone());
				} else {
					inner = ::BinaryenBlock(fn.d->M, self.c_str(), caseBody, 2, BinaryenTypeNone());
				}
			}
			b->instr.emplace_back(inner);
		}

		BinaryenExpressionRef BinaryenEmitter::GetSignalMaskWord(int bitIdx, int& outSubIdx) {
			int wordIdx = bitIdx / 32;
			outSubIdx = bitIdx % 32;
			auto sigmask = "sigmask" + std::to_string(wordIdx);
			DeclareGVar(M, sigmask, BinaryenTypeInt32());
			return BinaryenGetGlobal(M, sigmask.c_str(), BinaryenTypeInt32());
		}

		void BinaryenEmitter::StoreSignalMaskWord(int bitIdx, BinaryenExpressionRef word) {
			int wordIdx = bitIdx / 32;
			auto sigmask = "sigmask" + std::to_string(wordIdx);
			DeclareGVar(M, sigmask, BinaryenTypeInt32());
			b->instr.push_back(BinaryenSetGlobal(M, sigmask.c_str(), word));
		}

		BinaryenExpressionRef BinaryenEmitter::NonZero(BinaryenExpressionRef v) {
			auto ty = BinaryenExpressionGetType(v);
			if (ty == BinaryenTypeInt32()) {
				return NeInt32(v, Const(0));
			}
			if (ty == BinaryenTypeInt64()) {
				return NeInt64(v, Const64(0));
			}
			Use(v);
			if (ty == BinaryenTypeFloat32()) {
				return BinaryenBinary(M, BinaryenNeFloat32(), v, BinaryenConst(M, BinaryenLiteralFloat32(0.f)));
			}
			if (ty == BinaryenTypeFloat64()) {
				return BinaryenBinary(M, BinaryenNeFloat64(), v, BinaryenConst(M, BinaryenLiteralFloat64(0.f)));
			}
			KRONOS_UNREACHABLE;
		}

		BinaryenType BinaryenEmitter::FnArgTy(BinaryenFunctionTypeRef fnTy, int index) {
			return BinaryenFunctionTypeGetParam(fnTy, index);
		}

		int BinaryenEmitter::NumFnArgs(BinaryenFunctionTypeRef fnTy) {
			return BinaryenFunctionTypeGetNumParams(fnTy);
		}

		void BinaryenEmitter::TCO(BinaryenExpressionRef cond, const std::vector<BinaryenValue>& params) {
			fn.d->hasTco = true;
			If(cond,
			   [&](auto& tco) {
				   for (int i = 0; i < params.size(); ++i) {
					   tco.Set(i, params[i]);
				   }
				   tco.b->instr.emplace_back(BinaryenBreak(M, TCO_RECURSION, nullptr, nullptr));
			   });
		}

		BinaryenFunction::BinaryenFunction(BinaryenModuleRef M, const std::string& nm, bool exp, BinaryenFunctionTypeRef ty)
			:d(std::make_shared<FnData>(FnData{ nm, exp, ty, nullptr, M })) {
			for (auto &c : d->name) {
				if (!isalnum(c)) c = '_';
			}
		}

		void BinaryenFunction::Complete() const {
			if (!d->emitted) {
				std::string nm = d->name;
				int count = 2;
				auto Mod = (wasm::Module*)d->M;
				while (Mod->getFunctionOrNull(nm)) {
					nm = d->name + std::to_string(count++);
				}

				d->name = nm;

				auto bl = d->body.GetBlock(d->M);

				if (d->hasTco) {
					bl = BinaryenLoop(d->M, TCO_RECURSION, bl);
				}

				if (Mod->getFunctionTypeOrNull(BinaryenFunctionTypeGetName(d->ty)) == nullptr) {
					std::clog << "Type not found\n";
					//Mod->addFunctionType((wasm::FunctionType*)d->ty);
				}

				BinaryenExpressionRef saveStack[] = {
					BinaryenSetLocal(d->M, BinaryenFunctionTypeGetNumParams(d->ty),
					BinaryenGetGlobal(d->M, STACK_PTR, BinaryenTypeInt32())),
					bl
				};

				bl = ::BinaryenBlock(d->M, nullptr, saveStack, 2, BinaryenTypeAuto());

				d->fn = BinaryenAddFunction(d->M, nm.c_str(), d->ty, d->lvars.data(), (int)d->lvars.size(), bl);
				d->emitted = true;
			}
		}

		BinaryenIndex BinaryenFunction::LVar(const std::string& nm, BinaryenType vty) {
			assert(vty != BinaryenTypeNone());
			d->lvars.push_back(vty);
			return (int)(BinaryenFunctionTypeGetNumParams(d->ty) + d->lvars.size() - 1);
		}

		BinaryenIndex BinaryenFunction::LVar(BinaryenExpressionRef expr) {
			return LVar("", BinaryenExpressionGetType(expr));
		}

		BinaryenExpressionRef BinaryenBlock::GetBlock(BinaryenModuleRef M, const char *label) const {
			const char *l = (label || name.empty()) ? label : name.c_str();

			return ::BinaryenBlock(M, l, (BinaryenExpressionRef*)instr.data(), (int)instr.size(), BinaryenTypeAuto());
		}

		BinaryenExpressionRef BinaryenEmitter::MathFn(const char* name, BinaryenExpressionRef a, BinaryenExpressionRef b) {
			auto ty = BinaryenExpressionGetType(a);
			std::string nm{ name };
			if (ty == BinaryenTypeInt32()) nm += ".i32";
			if (ty == BinaryenTypeInt64()) nm += ".i64";
			if (ty == BinaryenTypeFloat32()) nm += ".f32";
			if (ty == BinaryenTypeFloat64()) nm += ".f64";
			BinaryenType paramTys[] = { ty,ty };
			return CallExternal(ty, nm, { a, b });
		}

		BinaryenExpressionRef BinaryenEmitter::MathFn(const char* name, BinaryenExpressionRef a) {
			auto ty = BinaryenExpressionGetType(a);
			std::string nm{ name };
			if (ty == BinaryenTypeInt32()) nm += ".i32";
			if (ty == BinaryenTypeInt64()) nm += ".i64";
			if (ty == BinaryenTypeFloat32()) nm += ".f32";
			if (ty == BinaryenTypeFloat64()) nm += ".f64";
			return CallExternal(ty, nm, { a });
		}

		BinaryenExpressionRef BinaryenEmitter::Load(BinaryenExpressionRef ptr, BinaryenType ty, int align) {
			int bytes = 4;
			if (ty == BinaryenTypeInt64() || ty == BinaryenTypeFloat64()) bytes = 8;
			if (align > bytes) align = bytes;
			Use(ptr);
			return BinaryenLoad(M, bytes, 1, 0, align, ty, ptr);
		}

		void BinaryenEmitter::Store(BinaryenExpressionRef ptr, BinaryenExpressionRef value, int align) {
			auto ty = BinaryenExpressionGetType(value);
			int bytes = 4;
			if (ty == BinaryenTypeInt64() || ty == BinaryenTypeFloat64()) bytes = 8;
			if (align > bytes) align = bytes;
			Use(ptr); Use(value);
			b->instr.push_back(BinaryenStore(M, bytes, 0, align, ptr, value, ty));
		}

		enum CompoundOps {
			Remainder = K3::Nodes::Native::NumOperands,
			UnsignedDivision,
			UnsignedGreater,
			CopySign,
			SynthEq,
			SynthNe,
			SynthGt,
			SynthGe,
			SynthLt,
			SynthLe
		};

		BinaryenExpressionRef BinaryenEmitter::LogicResult(BinaryenExpressionRef truthValue, BinaryenType resultType) {
			BinaryenLiteral allOnes, allZero;
			allOnes.type = allZero.type = resultType;
			allOnes.i64 = -1ll;
			allZero.i64 = 0ll;
			return BinaryenIf(M, truthValue, 
							  BinaryenConst(M, allOnes),
							  BinaryenConst(M, allZero));
		}

		BinaryenExpressionRef BinaryenEmitter::BinaryOp(K3::Nodes::Native::Opcode op, BinaryenExpressionRef lhs, BinaryenExpressionRef rhs) {
			auto ty = BinaryenExpressionGetType(lhs);
			using namespace K3::Nodes::Native;
			auto ty2 = BinaryenExpressionGetType(rhs);
			assert(ty == ty2);
#define OP1(S, T,O) if (ty == BinaryenType ## T ()) return Op(Binaryen ## O ## S ## T (), lhs, rhs); 
#define OPA(S, O) OP1(S, Int32, O) OP1(S, Int64, O) OP1(,Float32, O) OP1(,Float64, O) 
#define OPI(S, O) OP1(S, Int32, O) OP1(S, Int64, O)
#define OPF(O) OP1(,Float32,O) OP1(,Float64,O)
#define OPAC(KO, S, O) case K3::Nodes::Native::KO: OPA(S,O) KRONOS_UNREACHABLE;
#define LOPAC(KO, S, O) case K3::Nodes::Native::KO: \
							return LogicResult(BinaryOp((Native::Opcode)Synth ## O, lhs, rhs), ty); \
						case Synth ## O : OPA(S, O) KRONOS_UNREACHABLE;

#define OPIC(KO, S, O) case K3::Nodes::Native::KO: OPI(S,O) 
			switch ((int)op) {
				OPAC(Add, , Add)
				OPAC(Mul, , Mul)
				OPAC(Sub, , Sub)
				OPAC(Div, S, Div)
				LOPAC(Equal, , Eq)
				LOPAC(Not_Equal, , Ne)
				LOPAC(Greater, S, Gt)
				LOPAC(Greater_Equal, S, Ge)
				LOPAC(Less, S, Lt)
				LOPAC(Less_Equal, S, Le)
				OPIC(And, , And)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
				OPIC(Or, , Or)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
				OPIC(Xor, , Xor)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
				OPIC(BitShiftLeft, , Shl)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
				OPIC(BitShiftRight, S, Shr)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
				OPIC(LogicalShiftRight, U, Shr)
					return BitCast(ty, BinaryOp(op, BitCastInt(lhs), BitCastInt(rhs)));
			case Remainder:
				OPI(U, Rem) KRONOS_UNREACHABLE;
			case UnsignedDivision:
				OPI(U, Div) KRONOS_UNREACHABLE;
			case UnsignedGreater:
				OPI(U, Gt) KRONOS_UNREACHABLE;
			case Modulo:
				{
					auto x = TmpVar(rhs);
					return BinaryOp((Opcode)Remainder,
									BinaryOp(Add, lhs,
											 BinaryOp(Mul, x,
													  BinaryOp((Opcode)UnsignedDivision,
															   ty == BinaryenTypeInt32() ? Const(0x80000000) :
															   Const64(0x8000000000000000ul),
															   x))), x);
				}
			case Max:
				{
					auto a = TmpVar(lhs), b = TmpVar(rhs);
					return Select(BinaryOp((Native::Opcode)CompoundOps::SynthGt, a, b), a, b);
				}
			case Min:
				{
					auto a = TmpVar(lhs), b = TmpVar(rhs);
					return Select(BinaryOp((Native::Opcode)CompoundOps::SynthLt, a, b), a, b);
				}
			case ClampIndex:
				{
					auto a = TmpVar(lhs), b = TmpVar(rhs);
					return Select(BinaryOp((Opcode)UnsignedGreater, a, b),
								  NullConst(TypeOf(a)), b);
				}
			case AndNot:
				return BinaryOp(Native::And, UnaryOp(Not, lhs), rhs);
			case Pow:
				return MathFn("pow", lhs, rhs);
			case Atan2:
				return MathFn("atan2", lhs, rhs);
			case CopySign:
				OPF(CopySign)
					KRONOS_UNREACHABLE;
			default:
				KRONOS_UNREACHABLE;
			}
#undef OPA
#undef OPI
#undef OPIC
#undef OPAC
#undef OP1
#undef OPF
		}

		BinaryenExpressionRef BinaryenEmitter::UnaryOp(K3::Nodes::Native::Opcode op, BinaryenExpressionRef up) {
			auto ty = BinaryenExpressionGetType(up);
			using namespace K3::Nodes::Native;
#define OP1(S, T,O) if (ty == BinaryenType ## T ()) return Op(Binaryen ## O ## S ## T (), up);
#define OPA(S, O) OP1(S, Int32, O) OP1(S, Int64, O) OP1(,Float32, O) OP1(,Float64, O) 
#define OPI(S, O) OP1(S, Int32, O) OP1(S, Int64, O)
#define OPF(O) OP1(, Float32, O) OP1(, Float64, O)
#define OPAC(KO, S, O) case K3::Nodes::Native::KO: OPA(S,O) KRONOS_UNREACHABLE;
#define OPIC(KO, S, O) case K3::Nodes::Native::KO: OPI(S,O) KRONOS_UNREACHABLE;
#define FMATH(KO, O) case K3::Nodes::Native::KO: return MathFn(O, up); 
			BinaryenExpressionRef sign = nullptr;
			if (ty == BinaryenTypeInt32()) sign = Const(-1);
			if (ty == BinaryenTypeInt64()) sign = Const64(-1);
			switch (op) {
			case Neg:
				OPF(Neg)
					return BinaryOp(Xor, up, sign);
			case Abs:
				OPF(Abs)
					return BinaryOp(AndNot, sign, up);
			case Not:
				return BinaryOp(Xor, up, AllOnesConst(ty));
			case Truncate:
				OPF(Trunc)
				KRONOS_UNREACHABLE;
			case Round:
				OPF(Nearest)
				KRONOS_UNREACHABLE;
			case Ceil:
				OPF(Ceil)
				KRONOS_UNREACHABLE;
			case Floor:
				OPF(Floor)
				KRONOS_UNREACHABLE;
			FMATH(Sqrt, "sqrt")
			FMATH(Cos, "cos")
			FMATH(Sin, "sin")
			FMATH(Exp, "exp")
			FMATH(Log, "log")
			FMATH(Log10, "log10")
			FMATH(Log2, "log2")
			default:
				KRONOS_UNREACHABLE;
			}
		}

		void BinaryenEmitter::MemCpy(BinaryenExpressionRef dst, BinaryenExpressionRef src, BinaryenExpressionRef sz, int align) {
			if (BinaryenExpressionGetId(sz) == BinaryenConstId()) {
				int64_t constSz = -1;
				if (BinaryenExpressionGetType(sz) == BinaryenTypeInt64()) {
					constSz = BinaryenConstGetValueI64(sz);
				} else if (BinaryenExpressionGetType(sz) == BinaryenTypeInt32()) {
					constSz = BinaryenConstGetValueI32(sz);
				}

				if (constSz >= 0 && constSz <= 48 && (constSz & 3) == 0) {
					// inline
					while (constSz > 0) {
						auto dstVar = TmpVar(dst), srcVar = TmpVar(src);
						int movSz;
						if (constSz >= 8) {
							Store(dstVar, Load(srcVar, BinaryenTypeInt64()));
							movSz = 8;
						} else {
							Store(dstVar, Load(srcVar, BinaryenTypeInt32()));
							movSz = 4;
						}
						dst = Offset(dstVar, Const(movSz));
						src = Offset(srcVar, Const(movSz));
						constSz -= movSz;
					}
					return;
				}
			}

			if (BinaryenExpressionGetType(sz) == BinaryenTypeInt64()) {
				sz = BinaryenUnary(M, BinaryenWrapInt64(), sz);
			}


			std::string cpyId = "cpy" + std::to_string((intptr_t)dst) + std::to_string((intptr_t)src);
			auto offset = LVar("offset", BinaryenTypeInt32());
			Set(offset, Const(0));
			BinaryenBlock body{ "loop_" + cpyId };
			auto bodyB = BinaryenEmitter{ fn, body };
			bodyB.Store(bodyB.AddInt32(dst, Get(offset, BinaryenTypeInt32())),
						bodyB.Load(bodyB.Offset(src, Get(offset, BinaryenTypeInt32())), BinaryenTypeInt32(), align), align);
			bodyB.Set(offset, bodyB.AddInt32(Get(offset, BinaryenTypeInt32()), Const(4)));
			bodyB.b->instr.push_back(BinaryenBreak(M, cpyId.c_str(), bodyB.LtSInt32(Get(offset, BinaryenTypeInt32()), sz), nullptr));

			b->instr.push_back(BinaryenLoop(M, cpyId.c_str(), bodyB.EmitBlock()));
		}

		void BinaryenEmitter::MemSet(BinaryenExpressionRef dst, BinaryenExpressionRef word, BinaryenExpressionRef sz) {
			if (BinaryenExpressionGetType(sz) == BinaryenTypeInt64()) {
				sz = BinaryenUnary(M, BinaryenWrapInt64(), sz);
			}

			auto dstPtr = LVar(dst);
			auto srcWord = TmpVar(word);
			Loop(DivSInt32(sz, Const(4)), [&](auto& brk, auto& body, auto) {
				body.Store(Get(dstPtr), srcWord);
				Set(dstPtr, Offset(Get(dstPtr), Const(4)));
			});
		}

		BinaryenExpressionRef BinaryenEmitter::GlobalExternal(BinaryenType t, const std::string& mod, const std::string& sym) {
			auto Mod = (wasm::Module*)M;
			if (Mod->getGlobalOrNull(sym) == nullptr) {
				BinaryenAddGlobalImport(M, sym.c_str(), mod.c_str(), sym.c_str(), t);
			}
			return BinaryenGetGlobal(M, sym.c_str(), t);
		}

		BinaryenExpressionRef BinaryenEmitter::CallExternal(BinaryenType returnTy, const std::string& sym, const std::vector<BinaryenExpressionRef>& params) {
			std::vector<BinaryenType> paramTys(params.size());
			std::vector<BinaryenExpressionRef> pass(params.size());

			for (int i=0;i < params.size();++i) {
				pass[i] = params[i];
				auto ty = BinaryenExpressionGetType(pass[i]);
				if (ty == BinaryenTypeInt64()) {
					// int64 can't be passed as value to js
					auto buf = TmpVar(Alloca(Const(8), 8));
					Store(buf, pass[i], 8);
					pass[i] = buf;
				} else {
					Use(params[i]);
				}
				paramTys[i] = TypeOf(pass[i]);
			}

			bool returnByHeap = (returnTy == BinaryenTypeInt64());
			BinaryenValue returnBuffer;

			if (returnByHeap) {
				returnBuffer = TmpVar(Alloca(Const(8), 8));
				pass.emplace_back(returnBuffer);
				paramTys.emplace_back(BinaryenTypeInt32());
				returnTy = BinaryenTypeNone();
			}

			auto fty = BinaryenAddFunctionType(M, nullptr, returnTy, paramTys.data(), (int)paramTys.size());

			auto Mod = (wasm::Module*)M;
			if (Mod->getFunctionOrNull(sym.c_str()) == nullptr) {
				BinaryenAddFunctionImport(M, sym.c_str(), "import", sym.c_str(), fty);
			}

#ifndef NDEBUG
			for (int i = 0;i < BinaryenFunctionTypeGetNumParams(fty); ++i) {
				assert(BinaryenFunctionTypeGetParam(fty, i) == BinaryenExpressionGetType(pass[i]));
			}
#endif

			auto fncall = BinaryenCall(M, sym.c_str(), (BinaryenExpressionRef*)pass.data(), (int)pass.size(), returnTy);

			if (returnByHeap) {
				b->instr.emplace_back(fncall);
				return Load(returnBuffer, BinaryenTypeInt64(), 8);
			}

			return fncall;
		}

		BinaryenExpressionRef BinaryenEmitter::BitCast(BinaryenType to, BinaryenExpressionRef val) {
			if (to == BinaryenExpressionGetType(val)) return val;
			Use(val);
#define BC(T, F) if (to == BinaryenType ## T ()) return BinaryenUnary(M, BinaryenReinterpret ## F (), val);
			BC(Int32, Float32);
			BC(Int64, Float64)
			BC(Float32, Int32);
			BC(Float64, Int64);
#undef BC
			KRONOS_UNREACHABLE;
		}

		BinaryenExpressionRef BinaryenEmitter::BitCastInt(BinaryenExpressionRef val) {
			Use(val);
			auto ty = BinaryenExpressionGetType(val);
			if (ty == BinaryenTypeFloat32()) return BinaryenUnary(M, BinaryenReinterpretFloat32(), val);
			else if (ty == BinaryenTypeFloat64()) return BinaryenUnary(M, BinaryenReinterpretFloat64(), val);
			else return val;
		}

		BinaryenExpressionRef BinaryenEmitter::Coerce(BinaryenType to, BinaryenExpressionRef val) {
			auto from = BinaryenExpressionGetType(val);
#define CVT(FROM, TO) if (to == BinaryenType ## TO ()) return Op(Binaryen ## FROM ## To ## TO (), val)
#define CVT2(OP, TO) if (to == BinaryenType ## TO ()) return Op(Binaryen ## OP (), val)
			if (from == BinaryenTypeFloat32()) {
				CVT(TruncSFloat32, Int32);
				CVT(TruncSFloat32, Int64);
				CVT2(PromoteFloat32, Float64);
			} else if (from == BinaryenTypeFloat64()) {
				CVT(TruncSFloat64, Int32);
				CVT(TruncSFloat64, Int64);
				CVT2(DemoteFloat64, Float32);
			} else if (from == BinaryenTypeInt32()) {
				CVT(ConvertSInt32, Float32);
				CVT(ConvertSInt32, Float64);
				CVT2(ExtendSInt32, Int64);
			} else if (from == BinaryenTypeInt64()) {
				CVT(ConvertSInt64, Float32);
				CVT(ConvertSInt64, Float64);
				CVT2(WrapInt64, Int32);
			}
#undef CVT
#undef CVT2
			INTERNAL_ERROR("Invalid type conversion");
		}
	}
}