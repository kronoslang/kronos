#include <NodeBases.h>

namespace K3 {
	extern const Reactive::Node* AnyRX;
	namespace Nodes {
		using Matches = std::unordered_map<CTRef, CTRef>;

		TYPED_NODE(WildCard, TypedLeaf)
		PUBLIC
		Type Result(ResultTypeTransform&) const override { assert(false); KRONOS_UNREACHABLE; }
			static WildCard* New() { return new WildCard(); }
		END

		bool Match(CTRef lhs, CTRef rhs, Matches&);
	}

	namespace Transform {
		using namespace K3::Nodes;

		class Lowering;

		using ReplaceFuncTy = std::function<CTRef(Nodes::Matches&, Lowering&)>;
		class LoweringPatterns :
			public std::unordered_multimap<CTRef, ReplaceFuncTy,
										size_t(*)(CTRef), 
										bool(*)(CTRef, CTRef)> {

			static size_t HashRoot(CTRef root) {
				return root->ComputeLocalHash();
			}

			static bool EqGraph(CTRef lhs, CTRef rhs) {
				return lhs->TypeID() == rhs->TypeID() && lhs->LocalCompare(*rhs) == 0;
			}

			static void SetAnyReactivity(Typed *pattern) {
				if (pattern->GetReactivity() != AnyRX) {
					pattern->SetReactivity(AnyRX);
					for (unsigned i(0);i < pattern->GetNumCons();++i) {
						SetAnyReactivity((Typed*)pattern->GetUp(i));
					}
				}
			}

		public:
			LoweringPatterns():unordered_multimap(8, HashRoot, EqGraph) { }
			void AddRule(CTRef pattern, ReplaceFuncTy rpf) {
				SetAnyReactivity(const_cast<Typed*>(pattern));
				insert(std::make_pair(pattern, rpf));
			}
		};

		class Lowering : public CachedTransform<const Typed, CTRef> {
			LoweringPatterns& pat;
		public:
			Lowering(CTRef root, LoweringPatterns& pat):pat(pat), CachedTransform(root) { }
			CTRef operate(CTRef node);
		};

		class Replace : public CachedTransform < const Typed, CTRef > {
			Lowering& loweringTransform;
			Matches& matches;
		public:
			Replace(CTRef root, Lowering& t, Matches& m):CachedTransform(root), loweringTransform(t),matches(m) { }
			CTRef operate(CTRef node);
		};

	}
}