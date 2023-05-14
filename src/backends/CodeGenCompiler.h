#pragma once

#include "common/Ref.h"
#include "common/PlatformUtils.h"
#include "CallGraphAnalysis.h"
#include "NodeBases.h"
#include "CompilerNodes.h"


namespace K3 {
	enum DriverActivity {
		Never = -2,
		Always = -1,
		Counter
	};

	namespace Backends {
		using namespace K3::Nodes;

		enum BuilderPass {
			Sizing,
			Initialization,
			InitializationWithReturn,
			Evaluation,
			Reactive
		};

		class ICompilationPass {
		public:
			virtual DriverActivity IsDriverActive(const K3::Type& driver) = 0;
			//virtual ActivityStatus GetMaskStatus(unsigned idx) = 0;
			virtual const Backends::CallGraphNode* GetCallGraphAnalysis(const Nodes::Subroutine*) = 0;
			virtual const std::string& GetCompilationPassName() = 0;
			virtual BuilderPass GetPassType() = 0;
			virtual void SetPassType(BuilderPass p) = 0;
		};

		// major: or, minor: and
		class ActivityMaskVector : public RefCounting, public std::vector<std::vector<int>> {
			bool operator<(const ActivityMaskVector&) const;
			void DebugLog() const;
		};

		class CodeGenTransformBase {
		protected:
			std::string MakeUniqueName(const std::string& templateName);
			typedef std::tuple<CTRef, Ref<ActivityMaskVector>> SchedulingUnit;
			static std::vector<SchedulingUnit> TopologicalSort(const std::vector<SchedulingUnit>&);
			Ref<ActivityMaskVector> GetActivityMasks(CTRef node);
			static bool Schedule(const SchedulingUnit&, const SchedulingUnit&);
			ICompilationPass& compilationPass;
		public:
			CodeGenTransformBase(ICompilationPass& compilationPass) : compilationPass(compilationPass) {}
			bool IsInitPass() { return compilationPass.GetPassType() == Initialization || compilationPass.GetPassType() == InitializationWithReturn; }
			bool IsSizingPass() { return compilationPass.GetPassType() == Sizing; }
			ICompilationPass& GetCompilationPass() { return compilationPass; }
			static bool RefersLocalBuffers(CTRef node);
			Ref<ActivityMaskVector> CollectionToMask(const Qxx::IEnumerable<Type>& drivers, bool allDriversActive);
		};

		/* can remove a set of known active masks from driver signatures */
		DriverActivity RemoveCounter(ActivityMaskVector*, DriverActivity act);
		DriverActivity FilterDriverActivity(const K3::Type& dr, ActivityMaskVector* avm, ICompilationPass& master);

	}
}