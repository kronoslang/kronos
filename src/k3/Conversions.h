#pragma once
#include "NodeBases.h"

namespace K3 {
	namespace Nodes{

			static const char *NativeTypeName[4]=
			{
				"Float",
				"Double",
				"Int32",
				"Int64"
			};

		GENERIC_NODE(Convert,GenericUnary)
		PUBLIC
		enum NativeType{
			Float32,
			Float64,
			Int32,
			Int64
		};
			NativeType targetType;
		PRIVATE
			int LocalCompare(const ImmutableNode& rhs) const override;
			unsigned ComputeLocalHash() const override;
			Convert(NativeType kind, CGRef up) :GenericUnary(up), targetType(kind) { }
		PUBLIC
			static Convert* New(NativeType type, CGRef up) {return new Convert(type,up);}
			int GetWeight() const { return 1; }
		END

		template<typename TO, typename FROM> Backends::LLVMValue EmitCvt(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<float,float>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<double,float>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int32_t,float>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int64_t,float>(Backends::LLVMTransform& lt, Typed *src, int vec);

		template<> Backends::LLVMValue EmitCvt<float,double>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<double,double>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int32_t,double>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int64_t,double>(Backends::LLVMTransform& lt, Typed *src, int vec);

		template<> Backends::LLVMValue EmitCvt<float,int32_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<double,int32_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int32_t,int32_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int64_t,int32_t>(Backends::LLVMTransform& lt, Typed *src, int vec);

		template<> Backends::LLVMValue EmitCvt<float,int64_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<double,int64_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int32_t,int64_t>(Backends::LLVMTransform& lt, Typed *src, int vec);
		template<> Backends::LLVMValue EmitCvt<int64_t,int64_t>(Backends::LLVMTransform& lt, Typed *src, int vec);

		template <typename DST, int OPCODE> CTRef MakeConversionNode(CTRef up, const Type& src, CRRef rx);
	};
};