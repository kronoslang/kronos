#pragma once
#include "Type.h"
#include <vector>

namespace K3 {
	using namespace std;
	class DriverSignature {
		Type priority;
		Type metadata;
		double mul;
		double div;
		vector<int> masks;
	public:
        // this enumeration determines the priority of the classes (lowest first)
		enum DriverClassEnum {
			InitOrNull,
			Recursive,
            EvalArgument,
			User
		};

		DriverSignature(const Type& metadata, const Type& priority, double mul = 1, double div = 1);
		DriverSignature(const Type& t);

		operator Type() const;

		vector<int>& Masks() { return masks; }

		const vector<int>& Masks() const { return masks; }

		void SetMultiplier(double num, double denom) { mul = num; div = denom; }
		void GetMultiplier(double& num, double& denom) const { num = mul; denom = div; } 
		double GetMul() const { return mul; }
		double GetDiv() const { return div; }
		double GetRatio() const { return mul / div; }

		Type GetPriority() const { return priority; }
		void SetPriority(Type p) { priority = move(p); }

		Type GetMetadata() const { return metadata; }
		void SetMetadata(Type md) { metadata = move(md); }

		static bool IsSignature(const Type& t);

		int OrdinalCompare(const DriverSignature&) const;
		bool operator<(const DriverSignature& r) const { return OrdinalCompare(r) < 0; }
		bool operator>(const DriverSignature& r) const { return OrdinalCompare(r) > 0; }
		bool operator==(const DriverSignature& r) const { return OrdinalCompare(r) == 0; }
		bool operator<=(const DriverSignature& r) const { return OrdinalCompare(r) <= 0; }
		bool operator>=(const DriverSignature& r) const { return OrdinalCompare(r) >= 0; }

		DriverClassEnum GetDriverClass() const {return drvClass;}
	private:
		DriverClassEnum drvClass;
	};
	
}
