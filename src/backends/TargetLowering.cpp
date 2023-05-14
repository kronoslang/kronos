#include "TargetLowering.h"
#include "Evaluate.h"
#include "Errors.h"

namespace K3{
	namespace Nodes{
		bool Match(CTRef lhs, CTRef rhs, Matches& m) {
			if (lhs->TypeID() == rhs->TypeID() &&
				lhs->LocalCompare(*rhs) == 0 && lhs->GetNumCons() == rhs->GetNumCons()) {
				for (unsigned i(0); i < lhs->GetNumCons(); ++i) {
					if (Match(lhs->GetUp(i), rhs->GetUp(i), m) == false) return false;
				}
				return true;
			}

			WildCard *wc;
			if (lhs->Cast(wc)) {
				m[wc] = rhs;
				return true;
			} else if (rhs->Cast(wc)) {
				m[wc] = lhs;
				return true;
			}
			return false;
		}
	}

	namespace Transform {

		CTRef Replace::operate(CTRef node) {
			WildCard *wc;
			if (node->Cast(wc)) {
				return loweringTransform(matches[wc]);
			} else return node->IdentityTransform(*this);
		}

		CTRef ReplaceReactivity(Typed* node, const Reactive::Node *rx) {
			if (node->GetReactivity() == AnyRX || node->GetReactivity() == nullptr) {
				node->SetReactivity(rx);
				for (unsigned i(0);i < node->GetNumCons();++i) {
					ReplaceReactivity((Typed*)node->GetUp(i), rx);
				}
			}
			return node;
		}

		CTRef Lowering::operate(CTRef node) {
			auto match = pat.equal_range(node);
			if (match.first != match.second) {
				for (auto m = match.first; m != match.second; ++m) {
					Matches matchTable;
					if (Match(node, m->first, matchTable)) {
						CTRef result = m->second(matchTable, *this);
						if (result) {
							return ReplaceReactivity(const_cast<Typed*>(result), node->GetReactivity( ));
						}
					}
				}
			} 
			node = node->IdentityTransform(*this);
			FunctionCall *fc;
			if (node->Cast(fc)) {
				Lowering sub(fc->GetBody(), pat);
				fc->SetBody(sub.Go());
			}
			FunctionSequence *fseq;
			if (node->Cast(fseq)) {
				Lowering sub(fseq->GetIterator(), pat);
				fseq->SetIterator(sub.Go());
				fseq->SetGenerator(sub.Rebase(fseq->GetGenerator()));
			}
			return node;
		}
	}
}