#include <sstream>
#include <memory>

#include "DynamicVariables.h"
#include "Reactive.h"
#include "TLS.h"
#include "CompilerNodes.h"
#include "TypeAlgebra.h"
#include "kronos_abi.h"
#include "Invariant.h"
#include "Evaluate.h"

#include "paf/PAF.h"

#include "config/system.h"

#ifdef HAVE_LLVM
#pragma warning(disable: 4126 4267)
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/DynamicLibrary.h"
#endif

namespace Kronos {
	IType* ConvertToABI(const K3::Type& t);
}

namespace K3 {
	namespace Nodes{

		const char* GlobalVarTypeName[NumVarTypes] = {
			"Configuration",
			"Internal",
			"External",
			"Stream"
		};

		Specialization GenericExternalVariable::Specialize(SpecializationState& spec) const {
			SPECIALIZE(spec, key, GetUp(0));
			SPECIALIZE(spec, init, GetUp(1));

			if (spec.mode == SpecializationState::Configuration) {
				return init;
			}

			return Specialization(GetGlobalVariable::New(
				TLS::GetCurrentInstance()->Memoize(key.result.Fix()),
				init.result.Fix(), key.result.Fix(), std::make_pair(1,1), init.node), init.result.Fix());
		}

		template<typename INT> static INT GCD(INT a, INT b) {
			while (true) {
				a = a % b;
				if (a) {
					b = b % a;
					if (b == 0) return a;
				} else return b;
			}
		}

		Specialization GenericStreamInput::Specialize(SpecializationState& spec) const {
			SPECIALIZE(spec, key, GetUp(0));
			SPECIALIZE(spec, init, GetUp(1));

			if (spec.mode == SpecializationState::Configuration) {
				return init;
			}

			SPECIALIZE(spec, clock, GetUp(2));

			key.result = key.result.Fix();
			init.result = init.result.Fix();
			clock.result = clock.result.Fix();

			if (clock.result.IsPair() == false) {
				return SpecializationFailure();
			}

			CTRef dataSource = GetGlobalVariable::New(
				TLS::GetCurrentInstance()->Memoize(key.result),
				init.result,
				key.result,
				std::make_pair(1,1),
				init.node,
				Stream,
				clock.result.Rest());

			if (key.result.IsNil()) return Specialization(dataSource, init.result);

			CTRef clockSource = ReactiveOperators::Tick::New(clock.result);

			clockSource = ReactiveOperators::Impose::New(clockSource, dataSource);

			return Specialization(clockSource, init.result);
		}     

		Specialization GenericAsset::Specialize(SpecializationTransform& spec) const {
			SPECIALIZE(spec, uri, GetUp(0));
			std::stringstream uriS;
			uriS << uri.result.Fix();
			auto& asset = TLS::GetCurrentInstance()->GetAsset(uriS.str());
#ifdef HAVE_LLVM           
            if (!asset.memory) {
                auto readFile = PAF::AudioFileReader(uriS.str().c_str());
                if (readFile) {
                    auto numCh = readFile->Get(PAF::NumChannels);
                    auto smpRate = readFile->Get(PAF::SampleRate);
                    
                    std::vector<float> memory;
                    readFile->Stream([&](const float* data, int numSamples) {
                        auto at = memory.size();
                        memory.resize(at + numSamples);
                        std::copy(data, data + numSamples, memory.data() + at);
                        return numSamples;
                    });
					readFile->Close();
                    
                    float *buffer = (float*)malloc(sizeof(float) * memory.size());
                    std::copy(memory.cbegin(), memory.cend(), buffer);
                    asset.memory = decltype(asset.memory){ (void*)buffer, free };
                    
                    Type frameTy;
                    switch (numCh) {
                        case 0: frameTy = Type::Nil; break;
                        case 1: frameTy = Type::Float32; break;
                        default: frameTy = Type::Chain(Type::Float32, (size_t)numCh - 1, Type::Float32); break;
                    }
                    
                    asset.type = Type::User(&AudioFileTag,
                        Type::Pair(Type::InvariantI64(smpRate), 
								   Type::Chain(frameTy, memory.size() / (size_t)numCh, Type::Nil)));
                    llvm::sys::DynamicLibrary::AddSymbol(uriS.str(), asset.memory.get());

				} else {
					spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::FileNotFound,
											 "Error while opening audio file '%s'", uriS.str().c_str());
				}
			}
#endif
			if (!asset.memory) {
				spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::FileNotFound, 
										 "Could not load asset '%s'", uriS.str().c_str());
				return spec.GetRep().TypeError("External-Asset", uri.result);
			}
			return Specialization(ExternalAsset::New(uriS.str(), asset.type), asset.type);

		}

		Specialization GenericRebindSymbol::Specialize(SpecializationTransform& spec) const {
			SPECIALIZE_ARGS(spec, 0, 1);

			if (A1.result.GetSize()) {
				spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, A1.result, 
										 "Rebinding does not yet support types with runtime values; that is, "
										 "signals, compounds with signals or closures over signals.");
				return TypeError(&FatalFailure);
			}

			std::stringstream symS;
			symS << A0.result;			
			auto oldSym = TLS::ResolveSymbol(symS.str().c_str());

			if (!oldSym) {
				spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::SymbolNotFound, A0.result,
										 "This qualified name must be bound to a symbol before rebinding.");
				return TypeError(&FatalFailure);
			}

			auto oldCache = TLS::GetCurrentInstance()->GetSpecializationCache();
			TLS::GetCurrentInstance()->SetSpecializationCache(new SpecializationCache);
			TLS::RebindSymbol(symS.str().c_str(), Invariant::Constant::New(A1.result));

			auto evalClosure = spec(Evaluate::New("rebind", GetUp(2), Invariant::Constant::New(Type::Nil)));

			TLS::RebindSymbol(symS.str().c_str(), oldSym);
			TLS::GetCurrentInstance()->SetSpecializationCache(oldCache);
			return evalClosure;
		}
	};
};
