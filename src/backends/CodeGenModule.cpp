#include "CodeGenModule.h"

namespace {
	template<typename INT> static INT GCD(INT a, INT b) {
		while (true) {
			a = a % b;
			if (a) {
				b = b % a;
				if (b == 0) return a;
			} else return b;
		}
	}
}

namespace K3 {
	ActivationMatrix CodeGenModule::GetActivationMatrix(const Type& signature, int vector, int& outJitter) {
		auto multipliers = Qxx::From(drivers)
			.Where([&signature](DriverSignature t) { return t.GetMetadata() == signature; })
			.ToVector();

		int superclock = Qxx::From(multipliers)
			.Select([vector](DriverSignature t) -> int {
			int vmul = vector * (int)t.GetMul();
			return vmul / GCD(vmul, (int)t.GetDiv());
		})
			.Aggregate(1, [](int a, int b) {
			return (a * b) / GCD(a, b);
		});

		ActivationMatrix mtx{ vector };
		mtx.resize(superclock);

		outJitter = superclock;


		for (DriverSignature m : multipliers) {
			int mul = (int)m.GetMul() * vector;
			int div = (int)m.GetDiv();
			int gcd = GCD(mul, div);
			if (gcd != 1) {
				mul /= gcd;
				div /= gcd;
			}
			int stride = superclock / mul;

			if (stride < outJitter) outJitter = stride;
			for (int i = 0; i < superclock; i += stride) mtx[i].push_back(ActivationCell(m, div));
		}

		return mtx;
	}

	int CodeGenModule::ComputeAuspiciousVectorLength(ActivationMatrix &scalarActivations, int maxVectorLength) {
		static const int testPrimes[] = { 2, 3, 5, 7, 9, 11, 13, 15 };
		int vectorLen = 1;
		for (int commonPrimeFactor = 999; commonPrimeFactor > 1;) {
			commonPrimeFactor = 1;
			map<int, int> primeFactorOccurrences;

			for (auto& frame : scalarActivations) {
				for (auto& act : frame) {
					for (auto p : testPrimes) {
						if (act.GetDivider() % (p * vectorLen) == 0) {
							primeFactorOccurrences[p]++;
							if (primeFactorOccurrences[p] > primeFactorOccurrences[commonPrimeFactor])
								commonPrimeFactor = p;
						}
					}
				}
			}

			if (vectorLen * commonPrimeFactor > maxVectorLength) return vectorLen;
			else vectorLen *= commonPrimeFactor;
		}
		return vectorLen;
	}

	static std::pair<int, int> GetLoopStructure(ActivationMatrix::iterator start, ActivationMatrix::iterator end) {
		int longestLoop = 1;
		int longestLoopPeriod = 1;
		for (auto period : { 1, 2, 3, 4, 5, 6, 7, 8 }) {
			ActivationMatrix::iterator i;
			if (end - start > period) {
				for (i = start + period; i < end; i += period) {
					int j;
					for (j = 0; j<period && (end - i) > j && *(i + j) == *(i + j - period); ++j);
					if (j != period) break;
				}

				int thisLoop = int(i - start) / period;
				if (thisLoop > 1 && thisLoop * period > longestLoop * longestLoopPeriod) {
					longestLoop = thisLoop;
					longestLoopPeriod = period;
				}
			} else break;
		}
		return std::make_pair(longestLoopPeriod, longestLoop);
	}

	ActivationMatrix CodeGenModule::CombineRows(ActivationMatrix& src, int jitter) {
		if (jitter < 2) return src;
		ActivationMatrix mtx((int)src.size() / jitter);
		mtx.OversamplingFactor = src.OversamplingFactor / jitter;
		for (size_t i(0); i < mtx.size(); i++) {
			for (int j(0); j < jitter; ++j) {
				mtx[i].insert(mtx[i].begin(), src[i*jitter + j].begin(), src[i*jitter + j].end());
			}
		}
		return mtx;
	}

	CounterIndiceSet CodeGenModule::GetCounterSet(ActivationMatrix& amtx, int vectorLen = 1) {
		CounterIndiceSet set;
		for (auto& mx : amtx) {
			for (auto& my : mx) {
				if (my.GetDivider() > 1) {
					DriverSignature dsgn = my.GetDriver();
					dsgn.SetPriority(Type::Nil);
					set.insert(make_pair(dsgn, ActivationCounter(GetIndex(&my), my.GetDivider())));
					assert(set.find(my.GetDriver()) != set.end());
				}
			}
		}
		return set;
	}

	CodeGenModule::CodeGenModule(const Type& argType, const Type& resType) :Module(argType, resType) {

	}

	CodeGenPass::CodeGenPass(const std::string& label, CTRef ast, const CounterIndiceSet& counters):label(label), ast(ast), counterIndices(counters) {

	}


	int CodeGenPass::insert(const K3::Type& driver) {
		DriverSignature dsgn{ driver };
		dsgn.SetPriority(Type::Nil);
		Reactive::DriverSet::insert(dsgn);
		return 0;
	}

	DriverActivity CodeGenPass::IsDriverActive(const K3::Type& driverID) {
		DriverSignature d(driverID);
		if (d.GetDriverClass() == DriverSignature::User) {
			/* ignore mask sequence, priority and reconstruct primary driver */
			d.Masks().clear();
			d.SetPriority(Type::Nil);

			if (find(d) != nullptr) {
				auto f(counterIndices.find(d));
				if (f != counterIndices.end()) {
					assert(f->second.BitMaskIndex() >= 0 && "Counter has not been assigned a bit mask index");
					return (DriverActivity)f->second.BitMaskIndex();
				} else {
					for (auto ci : counterIndices) {
						assert((Type)d != ci.first && "bad hash");
					}
					return Always;
				}
			} else return Never;

		} else if (driverID == K3::Type(&Reactive::NullDriver)) {
			return Always; // null driver is always active
		}

		if (find(driverID) != 0) {
			auto f(counterIndices.find(d));
			if (f != counterIndices.end()) return (DriverActivity)get<0>(f->second);
			else return Always;
		} else return Never;
	}

}