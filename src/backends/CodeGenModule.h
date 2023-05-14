#pragma once

#include "ModuleBuilder.h"
#include "CallGraphAnalysis.h"
#include "DriverSignature.h"
#include "CodeGenCompiler.h"

namespace K3 {
	struct ActivationCell : public std::tuple<Type, int64_t> {
		static Type RemoveDriverPriority(const Type& d) {
			DriverSignature dsgn{ d };
			dsgn.SetPriority(Type::Nil);
			return dsgn;
		}
		ActivationCell(Type driver, int64_t divider) : std::tuple<Type, int64_t>(RemoveDriverPriority(driver), divider) {}
		const Type& GetDriver() const {
			assert(DriverSignature{ std::get<0>(*this) }.GetPriority().IsNil());
			return std::get<0>(*this);
		}
		int64_t GetDivider() const { return std::get<1>(*this); }
	};

	struct ActivationMatrix : public std::vector<std::vector<ActivationCell>> {
		int OversamplingFactor;
		ActivationMatrix(int of) :OversamplingFactor(of) { }
	};

	struct ActivationCounter : public std::tuple<unsigned, int64_t, int64_t> {
		ActivationCounter(unsigned index, int64_t divider) : std::tuple<unsigned, int64_t, int64_t>(index, divider, -1) {}
		unsigned GetIndex() const { return std::get<0>(*this); }
		int64_t GetDivider() const { return std::get<1>(*this); }
		int64_t& BitMaskIndex() { return std::get<2>(*this); }
		int64_t BitMaskIndex() const { return std::get<2>(*this); }
	};

	struct CounterIndiceSet : public std::unordered_map<Type, ActivationCounter> {};

	class CodeGenPass : public Reactive::DriverSet, public Backends::ICompilationPass {
	protected:
		const std::string label;
		CTRef ast;
		const CounterIndiceSet& counterIndices;
	public:
		CodeGenPass(const std::string& label, CTRef ast, const CounterIndiceSet& counters);

		int insert(const K3::Type& driver);

		DriverActivity IsDriverActive(const K3::Type& driverID) override;
	};

	class CodeGenModule : public K3::Module {
	protected:
		CounterIndiceSet GetCounterSet(ActivationMatrix& amtx, int vector);
		ActivationMatrix GetActivationMatrix(const Type& signature, int vector, int& outJitter);
		Backends::CallGraphMap cgmap;
	public:
		CodeGenModule(const Type& argType, const Type& resType);
		const Backends::CallGraphNode* GetCallGraphData(const Nodes::Subroutine* node) { return cgmap[node]; }
		static int ComputeAuspiciousVectorLength(ActivationMatrix &scalarActivations, int maxVectorLength);
		ActivationMatrix CombineRows(ActivationMatrix& src, int jitter);

		struct dshash {
			size_t operator()(const Reactive::DriverSet& ds) const {
				// drivers may be stored in different order for small sets.
				// thus use a crappy xor hash combinator to ensure order
				// is transparent to hash.
				size_t hash(0);
				ds.for_each([&](const Type& t) { hash ^= t.GetHash(); });
				return hash;
			}
		};
	};
}