#pragma once

#include "binaryen-c.h"
#include "Native.h"
#include "CodeGenCompiler.h"

#ifdef __EMSCRIPTEN__
void jsStackTrace();
#endif

namespace K3 {
	namespace Backends {
		static const char* STACK_PTR = "STACK_PTR";
		static const char* TCO_RECURSION = "TCO_RECURSION";
		static const char* STATIC_DATA = "STATIC_DATA";
		struct BinaryenFunction;
		struct BinaryenBlock {
			std::string name;
			std::vector<BinaryenExpressionRef> instr;
			BinaryenBlock(const std::string& nm) : name(nm) {}
			BinaryenBlock() {}
			BinaryenExpressionRef GetBlock(BinaryenModuleRef M, const char *nm = nullptr) const;
		};

		struct FnData {
			std::string name;
			bool exported;
			BinaryenFunctionTypeRef ty;
			BinaryenFunctionRef fn;
			BinaryenModuleRef M = nullptr;
			std::vector<BinaryenType> lvars = { BinaryenTypeInt32() }; // stack frame is always index 0
			BinaryenBlock body;
			mutable bool emitted = false;
			bool hasTco = false;
			std::unordered_set<BinaryenExpressionRef> seen;
		};

		struct BinaryenFunction {
			std::shared_ptr<FnData> d;

			void NoInline() {}
			void NoThrow() {}
			void FastCConv() {}

			BinaryenFunction(BinaryenModuleRef M, const std::string& nm, bool exp, BinaryenFunctionTypeRef ty);
			BinaryenFunction() {}
			void Complete() const;

			BinaryenFunctionTypeRef TypeOf() {
				return d->ty;
			}

			BinaryenType TypeOfVar(BinaryenIndex i) {
				return d->lvars[i];
			}

			BinaryenIndex LVar(const std::string&, BinaryenType ty);
			BinaryenIndex LVar(BinaryenExpressionRef);

			operator bool() const {
				return d.operator bool();
			}
		};

		struct BinaryenValue {
			BinaryenIndex idx;
			BinaryenModuleRef M;
			BinaryenLiteral L;
			BinaryenValue() {}
			BinaryenValue(BinaryenFunction& fn, BinaryenExpressionRef expr) :BinaryenValue(fn, fn.d->body, expr) {}
			BinaryenValue(BinaryenFunction& fn, BinaryenBlock& b, BinaryenExpressionRef expr) : M(fn.d->M) {
				L.type = BinaryenExpressionGetType(expr);
				assert(L.type != BinaryenTypeNone());
				if (BinaryenExpressionGetId(expr) == BinaryenConstId()) {
					idx = (BinaryenIndex)-1;
					if (L.type == BinaryenTypeInt32()) {
						L.i32 = BinaryenConstGetValueI32(expr);
						return;
					} else if (L.type == BinaryenTypeInt64()) {
						L.i64 = BinaryenConstGetValueI64(expr);
						return;
					} else if (L.type == BinaryenTypeFloat32()) {
						L.f32 = BinaryenConstGetValueF32(expr);
						return;
					} else if (L.type == BinaryenTypeFloat64()) {
						L.f64 = BinaryenConstGetValueF64(expr);
						return;
					}
					KRONOS_UNREACHABLE;
				} else if (BinaryenExpressionGetId(expr) == BinaryenGetLocalId() && 
						   BinaryenGetLocalGetIndex(expr) < BinaryenFunctionTypeGetNumParams(fn.d->ty)) {
					idx = BinaryenGetLocalGetIndex(expr);
				} else {
					idx = fn.LVar("", L.type);
					b.instr.emplace_back(BinaryenSetLocal(fn.d->M, idx, expr));
				}
			}

			operator BinaryenExpressionRef() const {
				if (idx == BinaryenIndex{ (BinaryenIndex)-1 }) {
					return BinaryenConst(M, L);
				}
				assert(L.type != BinaryenTypeNone());
				return BinaryenGetLocal(M, idx, L.type);
			}
		};

		class BinaryenEmitter {
			BinaryenFunction fn;
			BinaryenModuleRef M;
			BinaryenBlock* b;

#ifndef NDEBUG
#define Use(expr) UseDebug(expr, __FILE__, __LINE__, #expr )
#else 
#define Use(expr)
#endif

			void UseDebug(BinaryenExpressionRef u, const char *file, int line, const char *var) {
				if (fn.d->seen.count(u)) {
					std::clog << "Expression reuse " << file << "(" << line << ") " << var << "\n";
					for (auto i : b->instr) {
						BinaryenExpressionPrint(i);
					}
					std::clog << "\n...................\n";
					BinaryenExpressionPrint(u);
					std::clog << "\n^^^^^^^^^^^^^^^^^^^\n";
#ifdef __EMSCRIPTEN__
					jsStackTrace();
#endif
					INTERNAL_ERROR("Binaryen expression reused");
				}
				fn.d->seen.emplace(u);
			}

			BinaryenExpressionRef Op(BinaryenOp op, BinaryenExpressionRef lhs, BinaryenExpressionRef rhs) {
				Use(lhs);
				Use(rhs);
				return BinaryenBinary(M, op, lhs, rhs);
			}

			BinaryenExpressionRef Op(BinaryenOp op, BinaryenExpressionRef a) {
				Use(a);
				return BinaryenUnary(M, op, a);
			}

		public:
			BinaryenEmitter() :b(nullptr), M(nullptr) {}
			BinaryenEmitter(BinaryenFunction& fn) :fn(fn), b(&fn.d->body), M(fn.d->M) {}
			BinaryenEmitter(BinaryenFunction& fn, BinaryenBlock& b) :fn(fn), M(fn.d->M), b(&b) {}

			void Sfx(BinaryenExpressionRef fx) { b->instr.emplace_back(fx); }

			BinaryenExpressionRef EmitBlock() { return b->GetBlock(M); }

#define IB(SYM) B(SYM ## Int32) B(SYM ## Int64)
#define B(SYM) BinaryenExpressionRef SYM(BinaryenExpressionRef lhs, BinaryenExpressionRef rhs) { return Op(Binaryen ## SYM (), lhs, rhs ); } 
			B(ExtendSInt32)
			IB(Add)
			IB(Sub)
			IB(Mul)
			IB(DivS)
			IB(RemS)
			IB(RemU)
			IB(And)
			IB(Or)
			IB(Xor)
			IB(Shl)
			IB(ShrS)
			IB(ShrU)
			IB(Eq)
			IB(Ne)
			IB(LtS)
			IB(LeS)
			IB(GtS)
			IB(GeS)
#undef B
			BinaryenExpressionRef And(BinaryenExpressionRef lhs, BinaryenExpressionRef rhs) {
				return AndInt32(lhs, rhs);
			}

			BinaryenExpressionRef Or(BinaryenExpressionRef lhs, BinaryenExpressionRef rhs) {
				return OrInt32(lhs, rhs);
			}

			BinaryenExpressionRef LogicalNot(BinaryenExpressionRef x) {
				return XorInt32(AllOnesConst(BinaryenTypeInt32()), x);
			}

			BinaryenExpressionRef Select(BinaryenExpressionRef which, BinaryenExpressionRef whenTrue, BinaryenExpressionRef whenFalse) {
				Use(which); Use(whenTrue); Use(whenFalse);
				return BinaryenSelect(M, which, whenTrue, whenFalse);
			}

			BinaryenExpressionRef NonZero(BinaryenExpressionRef expr);

			BinaryenExpressionRef Const(std::int32_t v) {
				return BinaryenConst(M, BinaryenLiteralInt32(v));
			}

			BinaryenExpressionRef Const64(std::int64_t v) {
				return BinaryenConst(M, BinaryenLiteralInt64(v));
			}

			template <typename TConst>
			BinaryenExpressionRef Const(const std::string& name, TConst c) {
				auto v = Const(c);
				auto ty = BinaryenExpressionGetType(v);
				BinaryenAddGlobal(M, name.c_str(), ty, 0, v);
				return BinaryenGlobalGet(M, name.c_str(), ty);
			}

			BinaryenExpressionRef NullConst(BinaryenType ty) {
				BinaryenLiteral lt;
				memset(&lt, 0, sizeof(lt));
				lt.type = ty;
				return BinaryenConst(M, lt);
			}

			BinaryenExpressionRef AllOnesConst(BinaryenType ty) {
				BinaryenLiteral lt;
				memset(&lt, 0xffffffff, sizeof(lt));
				lt.type = ty;
				return BinaryenConst(M, lt);
			}

			BinaryenExpressionRef UndefConst(BinaryenType ty) {
				BinaryenLiteral lt;
#ifdef NDEBUG
				memset(&lt, 0, sizeof(lt));
#else 
				memset(&lt, 0xcd, sizeof(lt));
#endif
				lt.type = ty;
				return BinaryenConst(M, lt);
			}

			BinaryenExpressionRef PassiveValue(BinaryenType ty, const std::string& label) {
				return UndefConst(ty);
				BinaryenBlock labeled{ label };
				labeled.instr.push_back(UndefConst(ty));
				return labeled.GetBlock(M);
			}

			void Ret(BinaryenExpressionRef v = nullptr) {
				auto retVal = v ? (BinaryenExpressionRef)TmpVar(v) : nullptr;
				b->instr.emplace_back(
					BinaryenSetGlobal(M, STACK_PTR, BinaryenGetLocal(M, BinaryenFunctionTypeGetNumParams(fn.d->ty), BinaryenTypeInt32())));
				b->instr.emplace_back(BinaryenReturn(M, retVal));
			}

			void RetNoUnwind(BinaryenExpressionRef v = nullptr) {
				if (v) { Use(v); }
				b->instr.emplace_back(BinaryenReturn(M, v));
			}

			BinaryenExpressionRef Constant(const void* data, BinaryenType ty) {
				BinaryenLiteral lit;
				memcpy(&lit.i64, data, 8); // this is dodgy, but should work
				lit.type = ty;
				return BinaryenConst(M, lit);
			}

			BinaryenExpressionRef FnArg(int index);
			BinaryenExpressionRef FnArg(int index, BinaryenType ty);
			int NumFnArgs();

			BinaryenType FnArgTy(BinaryenFunctionTypeRef, int index);
			int NumFnArgs(BinaryenFunctionTypeRef);


			BinaryenExpressionRef GetSlot(int index);
			void SetSlot(int index, BinaryenExpressionRef val);

			BinaryenExpressionRef PtrToInt(BinaryenExpressionRef ptr);
			BinaryenExpressionRef IntToPtr(BinaryenExpressionRef i);
			BinaryenExpressionRef Offset(BinaryenExpressionRef ptr, BinaryenExpressionRef byteOffset);
			BinaryenExpressionRef Call(const BinaryenFunction& fn, const std::vector<BinaryenExpressionRef>& params, bool internalCconv);
			BinaryenExpressionRef PureCall(const BinaryenFunction& fn, const std::vector<BinaryenExpressionRef>& params, bool internalCconv);
			void TCO(BinaryenExpressionRef cond, const std::vector<BinaryenValue>& params);

			template <typename TTrue, typename TFalse> BinaryenExpressionRef PureIf(BinaryenExpressionRef pred, TTrue t, TFalse f) {
				BinaryenBlock trueBlk{""}, falseBlk{""};
				auto trueB = BinaryenEmitter{ fn, trueBlk };
				auto falseB = BinaryenEmitter{ fn, falseBlk };
				t(trueB);
				f(falseB);
				return BinaryenIf(M, pred, trueB.EmitBlock(), falseB.b->instr.empty() ? nullptr : falseB.EmitBlock());
			}

			template <typename TTrue, typename TFalse> void If(BinaryenExpressionRef pred, TTrue t, TFalse f) {
				b->instr.push_back(PureIf(pred, t, f));
			}

			template <typename TTrue> void If(BinaryenExpressionRef pred, TTrue t) {
				If(pred, t, [](auto) {});
			}



			std::string BlockName(const std::string& tmpl) {
				static std::atomic<int> blockIdCounter;
				return tmpl + "_" + std::to_string(blockIdCounter.fetch_add(1, std::memory_order_relaxed));
			}

			BinaryenBlock* CurrentBlock() {
				return b;
			}

			template <typename TBody> BinaryenExpressionRef Loop(TBody tb) {
				BinaryenBlock body{ BlockName("body") };
				auto bodyB = BinaryenEmitter{ fn, body };
				auto loopLabel = BlockName("loop");
				auto breakLabel = BlockName("break");
				tb(breakLabel, bodyB);
				bodyB.b->instr.emplace_back(BinaryenBreak(M, loopLabel.c_str(), nullptr, nullptr));
				auto l = BinaryenLoop(M, loopLabel.c_str(), bodyB.EmitBlock());
				BinaryenBlock breakBlock{ breakLabel };
				breakBlock.instr.push_back(l);
				b->instr.push_back(breakBlock.GetBlock(M));
				return l;
			}

			template <typename TBody> BinaryenExpressionRef LoopNZ(BinaryenExpressionRef loopCount, TBody tb) {
				auto counter = LVar("i", BinaryenTypeInt32());
				Set(counter, Const(0));

				auto breakLoop = BlockName("break");

				BinaryenBlock body{ BlockName("body") };
				auto bodyB = BinaryenEmitter{ fn, body };

				std::string loopId = BlockName("loop");
				tb(breakLoop, bodyB, bodyB.Get(counter, BinaryenTypeInt32()));

				bodyB.Set(counter, bodyB.AddInt32(bodyB.Get(counter, BinaryenTypeInt32()), bodyB.Const(1)));
				bodyB.b->instr.push_back(
					BinaryenBreak(M, loopId.c_str(), 
								  bodyB.NeInt32(loopCount, 
												bodyB.Get(counter, BinaryenTypeInt32())), nullptr));

				BinaryenBlock breakLoopBlock{ breakLoop };
				breakLoopBlock.instr.push_back(BinaryenLoop(M, loopId.c_str(), bodyB.EmitBlock()));
				b->instr.push_back(
					breakLoopBlock.GetBlock(M));
				return b->instr.back();
			}

			template <typename TBody> BinaryenExpressionRef Loop(BinaryenExpressionRef loopCount, TBody tb) {
				auto lc = TmpVar(loopCount);
				If(NeInt32(Const(0), lc), [&](auto lb) {
					lb.LoopNZ(lc, tb);
				});
				return b->instr.back();
			}

			BinaryenExpressionRef GVar(const std::string& name, BinaryenType ty);
			void SetGVar(const std::string& name, BinaryenExpressionRef value, bool soleAssignment = false);
			BinaryenExpressionRef Alloca(BinaryenExpressionRef sz, int align);

			BinaryenExpressionRef GlobalExternal(BinaryenType ty, const std::string& importModule, const std::string& sym);
			BinaryenExpressionRef CallExternal(BinaryenType returnType, const std::string& sym, const std::vector<BinaryenExpressionRef>&);
			void MemCpy(BinaryenExpressionRef dst, BinaryenExpressionRef src, BinaryenExpressionRef sz, int align);
			void MemSet(BinaryenExpressionRef dst, BinaryenExpressionRef word, BinaryenExpressionRef sz);

			BinaryenIndex LVar(const std::string& name, BinaryenType ty) { return fn.LVar(name, ty); }
			BinaryenIndex LVar(BinaryenExpressionRef expr) { return fn.LVar(expr); }

			BinaryenValue TmpVar(BinaryenExpressionRef val) {
				Use(val);
				return { fn, *b, val };
			}

			void Set(BinaryenIndex i, BinaryenExpressionRef val) {
				Use(val);
				assert(BinaryenExpressionGetType(val) != BinaryenTypeNone());
				b->instr.push_back(BinaryenSetLocal(M, i, val));
			}

			BinaryenExpressionRef Get(BinaryenIndex i, BinaryenType ty) {
				assert(ty != BinaryenTypeNone());
				return BinaryenGetLocal(M, i, ty);
			}

			BinaryenExpressionRef Get(BinaryenIndex i) {
				return BinaryenGetLocal(M, i, fn.TypeOfVar(i - BinaryenFunctionTypeGetNumParams(fn.d->ty)));
			}

			BinaryenExpressionRef False() { return Const(0); }

			void Switch(BinaryenExpressionRef c, const std::vector<BinaryenBlock>& blocks);

			BinaryenExpressionRef GetSignalMaskWord(int bitIdx, int& outSubIdx);
			void StoreSignalMaskWord(int bitIdx, BinaryenExpressionRef word);

			BinaryenType TypeOf(BinaryenExpressionRef expr) {
				return BinaryenExpressionGetType(expr);
			}

			BinaryenFunctionTypeRef TypeOf(BinaryenFunction fn) {
				return fn.d->ty;
			}

			BinaryenExpressionRef BitCast(BinaryenType to, BinaryenExpressionRef value);
			BinaryenExpressionRef BitCastInt(BinaryenExpressionRef value);
			BinaryenExpressionRef MathFn(const char* basename, BinaryenExpressionRef lhs, BinaryenExpressionRef rhs);
			BinaryenExpressionRef MathFn(const char* basename, BinaryenExpressionRef up);

			BinaryenExpressionRef Coerce(BinaryenType to, BinaryenExpressionRef from);
			BinaryenExpressionRef LogicResult(BinaryenExpressionRef truthValue, BinaryenType resultType);

			BinaryenExpressionRef SizeOfPointer() {
				return Const64(4); // wasm32
			}

			BinaryenExpressionRef Load(BinaryenExpressionRef pointer, BinaryenType, int align = 0);
			void Store(BinaryenExpressionRef pointer, BinaryenExpressionRef value, int align = 0);

			BinaryenExpressionRef BinaryOp(Nodes::Native::Opcode, BinaryenExpressionRef lhs, BinaryenExpressionRef rhs);
			BinaryenExpressionRef UnaryOp(Nodes::Native::Opcode, BinaryenExpressionRef up);
		};
	}
}