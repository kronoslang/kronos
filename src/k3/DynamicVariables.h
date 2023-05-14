#pragma once
#include "NodeBases.h"

namespace K3 {
	namespace Nodes{
			TYPED_NODE(SetGlobalVariable,TypedUnary)
				const void *uid;
				const bool byRef;
				SetGlobalVariable(const void *id, CTRef value, bool byRef):TypedUnary(value),uid(id),byRef(byRef) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedUnary,uid,byRef);
				unsigned ComputeLocalHash() const override {size_t h(TypedUnary::ComputeLocalHash());HASHER(h,(uintptr_t)uid);HASHER(h,byRef?1:0);return (unsigned)h;}
				static SetGlobalVariable* New(const void *varId, CTRef newValue, bool byRef = true) {return new SetGlobalVariable(varId,newValue,byRef);}
				Type Result(ResultTypeTransform& rt) const  override {INTERNAL_ERROR("No result");}
				CTRef SideEffects(Backends::SideEffectTransform&) const override;
				const void* GetUID() const {return uid;}
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			END

			enum GlobalVarType {
				Configuration,
				Internal,
				External,
				Stream,
				NumVarTypes
			};

			extern const char* GlobalVarTypeName[NumVarTypes];

			TYPED_NODE(GetSlot, TypedPolyadic)
				int index;
				GetSlot(int i, CTRef init) :index(i) { if (init) Connect(init); }
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedPolyadic, index);
				unsigned ComputeLocalHash() const  override {
					auto h = TypedPolyadic::ComputeLocalHash();
					HASHER(h, index);
					return h;
				}
				Type Result(ResultTypeTransform&) const  override { KRONOS_UNREACHABLE; }
				static GetSlot* New(int index, CTRef initializer = nullptr) { return new GetSlot( index, initializer ); }
				CODEGEN_EMITTER
			END

			TYPED_NODE(GetGlobalVariable,TypedPolyadic,IFixedResultType)
				const Type t;
				const Type key, clock;
				const std::pair<int, int> vectorRate;
				const void *uid;
				GlobalVarType  k;
				GetGlobalVariable(const void *id, const Type& t, const Type& key, CTRef initializer, GlobalVarType e, std::pair<int, int> rate, const Type& clock):
					t(t), key(key), clock(clock), vectorRate(rate), uid(id), k(e) {
					if (initializer) Connect(initializer);
				}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(TypedPolyadic,uid,t,k);
				unsigned ComputeLocalHash() const  override {
					size_t h(TypedPolyadic::ComputeLocalHash());HASHER(h,(uintptr_t)uid);HASHER(h,t.GetHash());return (unsigned)h;
				}


				static GetGlobalVariable* New(const void *varId, const Type& t, const Type& key, std::pair<int, int> vectorRate, CTRef initializer = 0, GlobalVarType kind = Internal, const Type& clock = Type::Nil) {
					return new GetGlobalVariable(varId,t,key,initializer,kind,vectorRate, clock);
				}

				Type Result(ResultTypeTransform&) const override {return t;}
				Type FixedResult() const override {return t;}
				CTRef SideEffects(Backends::SideEffectTransform&) const override;
				const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
				CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
				bool IsExternal( ) const { return k == External; }
				bool IsConfigurationSlot() const { return k == Configuration; }
				bool IsStream() const { return k == Stream; }
				Type GetKey() const { return key; }
				int GetWeight() const  override { return 2; }
			END

			GENERIC_NODE(GenericRebindSymbol, GenericTernary)
				GenericRebindSymbol(CGRef sym, CGRef binding, CGRef closure) 
					:GenericTernary(sym, binding, closure) { }
			PUBLIC
				static GenericRebindSymbol* New(CGRef symbolString, CGRef newBinding, CGRef closure) { return new GenericRebindSymbol(symbolString, newBinding, closure); }
			END

			GENERIC_NODE(GenericGetGlobalVariable,DisposableGenericLeaf)
				Type t;
				const void *uid;
				GenericGetGlobalVariable(const void *id, const Type& t) :t(t.Fix( )), uid(id) { }
			PUBLIC
				DEFAULT_LOCAL_COMPARE(DisposableGenericLeaf,t,uid);
				static GenericGetGlobalVariable* New(const void *varId, const Type& t) {return new GenericGetGlobalVariable(varId,t);}
			END

			GENERIC_NODE(GenericExternalVariable,GenericBinary)
			GenericExternalVariable(CGRef key, CGRef initializer):GenericBinary(key,initializer){}
			PUBLIC
				static GenericExternalVariable* New(CGRef key, CGRef initializer) {return new GenericExternalVariable(key,initializer);}
			END

			GENERIC_NODE(GenericStreamInput,GenericTernary)
				GenericStreamInput(CGRef key, CGRef init, CGRef clock) : GenericTernary(key,init,clock) { }
			PUBLIC
				static GenericStreamInput* New(CGRef key, CGRef init, CGRef clock) { return new GenericStreamInput(key,init,clock); }
			END
       
			GENERIC_NODE(GenericAsset, GenericUnary)
				GenericAsset(CGRef uri) : GenericUnary(uri) {}
			PUBLIC
				static GenericAsset* New(CGRef uri) { return new GenericAsset(uri); }
			END

			TYPED_NODE(ExternalAsset, DisposableTypedLeaf, IFixedResultType)
				Type dataType;
				std::string dataUri;
				ExternalAsset(std::string uri, Type ty) :dataType(ty), dataUri(uri) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf, dataType, dataUri);

				static ExternalAsset* New(std::string uri, Type type) {
					return new ExternalAsset(uri, type);
				}

				Type FixedResult() const override {
					return dataType;
				}

				Type Result(ResultTypeTransform&) const override {
					return FixedResult();
				}

				CTRef SideEffects(Backends::SideEffectTransform&) const override;
				CODEGEN_EMITTER
			END

	};
};
