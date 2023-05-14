#pragma once

struct mu{};

#define CXX_INHERIT_PUBLIC(SUPERCLASS) public SUPERCLASS,

#define END	};

#define LOCAL_COMPARE_BODY(x) if (x<rhs.x) return -1;if(x>rhs.x) return 1;
#define DEFAULT_LOCAL_COMPARE(BASE,...) int LocalCompare(const ImmutableNode& r) const override \
{auto& rhs(static_cast<decltype(*this)>(r)); META_MAP(LOCAL_COMPARE_BODY,__VA_ARGS__); return BASE::LocalCompare(r);}

#include "Generic.h"
#include "Typed.h"

#include <ostream>

template <class NODE>
std::ostream& operator<<(std::ostream& out, const K3::CachedTransformNode<NODE>& graph) {
	static thread_local unsigned int indent;
	struct printVisitor {
		const ImmutableNode *parentTuple = nullptr;
		std::ostream& strm;
		printVisitor(std::ostream& s):strm(s) {indent=0;}
		K3::FlowControl Visit(const ImmutableNode *node) {			
			this->strm<<" ";
			if (node->GetNumCons()>1) {
				this->strm<<"\n";
				for(unsigned i(0);i<indent;++i) this->strm<<"  ";
			}
			node->Output(this->strm);
			indent++;
			if (node->GetNumCons()>0) {
				this->strm<<"(";
			}

			return K3::Continue;
		}

		void Back(const ImmutableNode *node) {
			if (node->GetNumCons()>0) this->strm<<" )";
			indent--;
		}

		bool Recursive(const ImmutableNode* node) {
			this->strm<<" ~";node->Output(this->strm);
			if (node->GetNumCons()) {
				this->strm << "<" << (void*)node << ">";
			}
			return true;
		}
	};
	printVisitor pv(out);
	K3::ForGraph(&graph,pv);
	return out;
}

