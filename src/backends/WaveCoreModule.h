#pragma once

#include <ModuleBuilder.h>
#include <UserErrors.h>

#include "LLVMModule.h"

namespace K3 {
	namespace Backends {
		enum PAType {
			ADD,
			CMP,
			DIV,
			LGF,
			LUT,
			MUL,
			MAD,
			AMP,
			RND,
			CONSTANT,
			HYSTERESIS,
			VOLATILE_CONSTANT,
			UNDEF
		};

		struct WCInstr : public std::tuple<PAType, int, WCInstr*, WCInstr*, float, WCInstr*, int> {
			WCInstr(PAType t, int out, WCInstr* op1, WCInstr *op2, float p, WCInstr *tau, int dly):
				tuple(t, out, op1, op2, p, tau, dly) { }

			PAType& Type() { return std::get<0>(*this); }
			int& OutID() { return std::get<1>(*this); }
			WCInstr*& Op1() { return std::get<2>(*this); }
			WCInstr*& Op2() { return std::get<3>(*this); }
			float& Param() { return std::get<4>(*this); }
			WCInstr*& Tau() { return std::get<5>(*this); }
			int& Delay() { return std::get<6>(*this); }
            
            const std::tuple<PAType, int, WCInstr*, WCInstr*, float, WCInstr*, int> GetTuple() const { return *this;
            }
		};

		class WCNetList : public std::list<WCInstr> {
			int counter = 0;
		public:
			WCInstr* append(PAType t, WCInstr* op1, WCInstr* op2, float p = 0, WCInstr* tau = nullptr, int dly = 0) {
				emplace_back(t, ++counter, op1, op2, p, tau, dly);
				return &back();
			}

			WCInstr* append(WCInstr from) {
				from.OutID() = ++counter;
				emplace_back(std::move(from));
				return &back();
			}

			// these inputs forced to always exist
			std::unordered_set<std::string> extVars = {
				".AudioInL",
				".AudioInR"
			};
		};

		using WCNetListMap = std::unordered_map<Type, WCNetList>;

		std::ostream& operator<<(std::ostream&, const WCInstr&);
		std::ostream& operator<<(std::ostream&, const WCNetList&);

		class WaveCoreClass : public Class {
			Ref<LLVMClass> NativeProcess;
			int NumNativeEdges = 0;
			static const int AudioRate = 48000;
			std::unordered_map<Type,WCNetList> nl;
			WCInstr *leftOut = nullptr, *rightOut = nullptr;
			void WriteProcessGraph(const char *ns, std::ostream&);
			void WriteProcess(const char *ns, std::ostream&);
			void WriteProcessPartition(Type driver, WCNetList& nl, const char *ns, std::ostream&);
		public:
			void SetOutputs(WCInstr* l, WCInstr *r) { leftOut = l; rightOut = r; }
			void SetNativeProcess(Ref<LLVMClass> np) { NativeProcess = np; }
			void SetNumNativeEdges(int ne) { NumNativeEdges = ne; }
			WaveCoreClass(const Type& result, const Type& out):Class(result, out) { }
			virtual void MakeStatic(const char *nsPrefix, const char *fileType, std::ostream& write, Kronos::BuildFlags flags, const char*, const char *, const char*);
			size_t GetSize() const { return NativeProcess->GetSize(); }
			size_t GetAlignment() const { return NativeProcess->GetAlignment(); }
			void Evaluate(Instance& i, const void* in, void* out) const { return NativeProcess->Evaluate(i, in, out); }
			void Reset(Instance& i, const void*arg) const { NativeProcess->Reset(i, arg); }
			size_t GetSlotOffset(const Type& k) const { return NativeProcess->GetSlotOffset(k); }
			int GetSlotIndex(const Type& k) const { return NativeProcess->GetSlotIndex(k); }
			TriggerCallbackTy GetTriggerCallback(const Type& k) const { return NativeProcess->GetTriggerCallback(k); }
			Type GetListOfVars() const { return NativeProcess->GetListOfVars(); }
			Type GetListOfTriggers() const { throw NativeProcess->GetListOfTriggers(); }
			std::string GetTriggerName(const Type& k) const { return NativeProcess->GetTriggerName(k); }
			WCNetList& GetNetList(Type forClock) { 
				return nl[forClock];
			}
			Type StreamClockOfVar(const K3::Type& k) const { return NativeProcess->StreamClockOfVar(k); }
			
			Type TypeOfVar(const Type& key) const {
				return NativeProcess->TypeOfVar(key);
			}
				
			virtual bool SetVar(const Type& key, const void* data) {
				return Class::SetVar(key, data) || NativeProcess->SetVar(key, data);
			}

			virtual bool HasVar(const Type& k) const {
				return Class::HasVar(k) || NativeProcess->HasVar(k);
			}
		};

		class WaveCore : public LLVM {
		public:
			WaveCore(CTRef AST, const Type& argTy, const Type& resTy);
			Class *Build(Kronos::BuildFlags);
//			Graph<Typed> BeforeReactiveAnalysis(CTRef body);
		};
	}
}