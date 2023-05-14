#pragma once

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"

#include <functional>
#include <unordered_map>
#include <mutex>

namespace LLVMUtil {
	using namespace llvm;

	enum Linkage {
		Export = GlobalValue::LinkageTypes::ExternalLinkage,
		Internal = GlobalValue::LinkageTypes::InternalLinkage
	};

	class TypeRegistry {
		std::unordered_map<const void*, llvm::StructType*> uniqueTypes;
		LLVMContext& ctx;
	public:
		TypeRegistry(LLVMContext& c) :ctx(c) { }
		llvm::StructType* Register(const void* uid) {
			auto f = uniqueTypes.find(uid);
			if (f == uniqueTypes.end())
				f = uniqueTypes.emplace(uid, StructType::create(ctx)).first;

			return f->second;
		}

		LLVMContext& operator*() {
			return ctx;
		}
	};
	
	template <typename T> struct TypeResolver {
		static llvm::StructType* get(TypeRegistry& ctx) {
			static char uid = 0;
			return ctx.Register(&uid);
		}
	};

	template <typename T> struct TypeResolver<T*> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return TypeResolver<T>::get(ctx)->getPointerTo();
		}
	};

	template <typename T> struct TypeResolver<const T> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return TypeResolver<T>::get(ctx);
		}
	};

	template <> struct TypeResolver<void> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getVoidTy(*ctx);
		}
	};

	template <typename T, size_t SZ> struct TypeResolver<std::array<T,SZ>> {
		static llvm::ArrayType* get(TypeRegistry& ctx) {
			return llvm::ArrayType::get(
				TypeResolver<T>::get(ctx),
				SZ);
		}
	};

	template <typename T, size_t SZ> struct TypeResolver<T[SZ]> {
		static llvm::ArrayType* get(TypeRegistry& ctx) {
			return llvm::ArrayType::get(
				TypeResolver<T>::get(ctx),
				SZ);
		}
	};

	// map void* to i8*
	template <> struct TypeResolver<void*> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt8PtrTy(*ctx);
		}
	};

	template <> struct TypeResolver<const void*> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt8PtrTy(*ctx);
		}
	};

	template <> struct TypeResolver<std::int8_t> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt8Ty(*ctx);
		}
	};

	template <> struct TypeResolver<char> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt8Ty(*ctx);
		}
	};

	template <> struct TypeResolver<std::uint8_t> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt8Ty(*ctx);
		}
	};

	template <> struct TypeResolver<std::int32_t> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt32Ty(*ctx);
		}
	};

	template <> struct TypeResolver<std::int64_t> {
		static llvm::Type* get(TypeRegistry& ctx) {
			return llvm::Type::getInt64Ty(*ctx);
		}
	};

	template <typename RET, typename... ARGS> struct TypeResolver<RET(ARGS...)> {
		static FunctionType* get(TypeRegistry& ctx) {
			return FunctionType::get(
				TypeResolver<RET>::get(ctx), {
					(TypeResolver<ARGS>::get(ctx))...
				}, false);
		}
	};

	class LLVMGen : public TypeRegistry {
		LLVMContext& ctx;
		llvm::Module* mod;
	public:
		llvm::Module* GetModule() { return mod; }
		struct TypeGen {
			LLVMGen& parent;
			using Type = llvm::Type;
			template <typename T> using helper = TypeResolver<T>;
			template <typename T> auto operator()(T) { return helper<T>::get(parent); }
			template <typename... MEMBERS> StructType* getStruct(bool packed) {
				return StructType::get(parent.ctx, { helper<MEMBERS>::get(parent)... }, packed);
			}
			TypeGen(LLVMGen& p) :parent(p) { }
		};

		struct IRGen {
			LLVMContext& ctx;
			IRBuilder<> cg;
			IRGen(LLVMContext& ctx, Function* fn):ctx(ctx), cg(BasicBlock::Create(ctx, "Top", fn))  { }
			IRBuilder<>& operator->() { return cg; }
		};

		TypeGen ty;

		LLVMGen(LLVMContext& c, llvm::Module *m) :TypeRegistry(c), ctx(c), mod(m), ty(*this) {}

		template<typename FNTY>
		Function* declare(Linkage link, const std::string& label) {
			auto fnTy = TypeGen::helper<typename std::remove_pointer<FNTY>::type>::get(*this);
			return Function::Create(fnTy, (GlobalValue::LinkageTypes)link, label, mod);
		}

		using BodyGen = std::function<void(IRBuilder<>&, iterator_range<Function::arg_iterator>)>;

		Function* implement(Function *fn, BodyGen body) {
			IRGen gen(ctx, fn);
			body(gen.cg, fn->args());
			return fn;
		}


		template<typename FNTY>
		Function* defn(Linkage link, const std::string& label, BodyGen body) {
			auto fn = declare<FNTY>(link, label);
			return implement(fn, body);
		}

		llvm::GlobalVariable* def(Linkage link, llvm::Type* ty, const std::string& label, bool constant = true) {
			mod->getOrInsertGlobal(label, ty);
			auto gv = mod->getGlobalVariable(label);
			gv->setLinkage((GlobalVariable::LinkageTypes)link);
			gv->setConstant(constant);
			return gv;
		}

		llvm::GlobalValue* def(Linkage link, const std::string& stringVar, const std::string& stringVal, bool constant = true) {
			auto arrayTy = ArrayType::get(ty(char()), stringVal.size() + 1);
			auto str = def(link, arrayTy, stringVar, constant);
			str->setInitializer(ConstantDataArray::getString(ctx, stringVal));
			return str;
		}

		llvm::Constant* constant(std::int8_t val) {
			return Constant::getIntegerValue(ty(val), APInt(8, val));
		}

		llvm::Constant* constant(std::int32_t val) {
			return Constant::getIntegerValue(ty(val), APInt(32, val));
		}

		llvm::Constant* constant(std::int64_t val) {
			return Constant::getIntegerValue(ty(val), APInt(64, val));
		}

		template <typename T> llvm::Constant* nullConstant() {
			return Constant::getNullValue(TypeResolver<T>::get(*this));
		}

		std::unordered_map<std::string, llvm::Constant*> stringPool;		
		llvm::Constant* constant(const std::string& str) {
			auto f = stringPool.find(str);
			if (f == stringPool.end()) {
				auto stringConstant = ConstantDataArray::getString(ctx, str);
				auto gv = new GlobalVariable(*mod, stringConstant->getType(), true, GlobalValue::InternalLinkage, stringConstant);
				f = stringPool.emplace(str, ConstantExpr::getBitCast(gv, ty(""))).first;
			}
			return f->second;
		}

		llvm::Function* API(llvm::Function *f) {
			f->setCallingConv(CallingConv::C);
			f->setDoesNotThrow();
			return f;
		}

		template<typename STRUCT, typename... ARGS>
		llvm::StructType* finalizeStruct(const std::string& name = "", bool packed = false) {
			auto st = TypeResolver<STRUCT>::get(*this);
			if (!name.empty()) st->setName(name);
			st->setBody({
				TypeResolver<ARGS>::get(*this)...
			}, packed);
			return st;
		}
	};

}