#include <algorithm>
#include <cstdint>

#include "common/Graphviz.h"

#include "SideEffectCompiler.h"
#include "NodeBases.h"
#include "CompilerNodes.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
#include "Reactive.h"
#include "Native.h"
#include "Conversions.h"
#include "Invariant.h"
#include "Reactive.h"
#include "DynamicVariables.h"
#include "NativeVector.h"
#include "DriverSignature.h"
#include "FlowControl.h"
#include "TLS.h"
#include "UserErrors.h"

#ifndef NDEBUG
#include <sstream>
#endif

#define NI64(op,a,b) Nodes::Native::MakeInt64(#op, Native::op, a, b)
#define NI32(op,a,b) Nodes::Native::MakeInt32(#op, Native::op, a, b)
#define FI32(op,a,b) Nodes::Native::MakeFloat(#op, Native::op, a, b)

#define INSTRUMENT_FUNCTIONS 0

#if INSTRUMENT_FUNCTIONS
template <typename T> void Consume(std::ostream& to, const void*& data) {
	to << *(T*)data;
	data = (const char*)data + sizeof(T);
}

const char* ToStream(std::ostream& os, const char* typeInfo, const void*& dataBlob) {
	if (dataBlob) {
		for (;;) {
			switch (char c = *typeInfo++) {
			case '\0': return typeInfo;
			case '%':
				c = *typeInfo++;
				switch (c) {
				case 'f': Consume<float>(os, dataBlob); break;
				case 'd': Consume<double>(os, dataBlob);  break;
				case 'i': Consume<std::int32_t>(os, dataBlob);  break;
				case 'q': Consume<std::int64_t>(os, dataBlob);  break;
				case '[': {
					char *loopPoint;
					for (auto loopCount = strtoull(typeInfo, &loopPoint, 10); loopCount; --loopCount)
						typeInfo = ToStream(os, loopPoint + 1, dataBlob);
					break;
				}
				case ']': return typeInfo;
				case '%': os.put('%'); break;
				default:
					assert(0 && "Bad format string");
				}
				break;
			default:
				os.put(c);
				break;
			}
		}
	} else {
		for (;;) {
			switch (char c = *typeInfo++) {
			case '\0': return typeInfo;
			case '%':
				c = *typeInfo++;
				switch (c) {
				case 'f': os << "\"Float\""; break;
				case 'd': os << "\"Double\"";  break;
				case 'i': os << "\"Int32\"";  break;
				case 'q': os << "\"Int64\""; break;
				case '[': {
					char *loopPoint;
					for (auto loopCount = strtoull(typeInfo, &loopPoint, 10); loopCount; --loopCount)
						typeInfo = ToStream(os, loopPoint + 1, dataBlob);
				}
				case ']': break;
				case '%': os.put('%'); break;

				default:
					assert(0 && "Bad format string");
				}
				break;
			default:
				os.put(c);
				break;
			}
		}
	}
	return typeInfo;
}
KRONOS_ABI_EXPORT int32_t kvm_label(const char *l) {
	std::clog << "\n\n[" << l << "]\n";
	return 0;
}
KRONOS_ABI_EXPORT int32_t kvm_instrument(int32_t chain, const char *l, const void* data, const char* fmt) {
	std::clog << l << " : "; ToStream(std::clog, fmt, data);
	std::clog << "\n";
	return chain;
}
#endif

namespace K3
{
	static CTRef Canonicalize(CTRef data, CRRef r, const Type& e, bool referenced, bool mustCopy, Backends::SideEffectTransform& sfx);

	static bool CheckForArgInputs(CTRef dst) {
		return Qxx::FromGraph(dst).OfType<SubroutineArgument>().Where(
			[](auto sa) {
			if (sa->IsSelf()) return false;
			if (sa->IsLocalState()) return false;
			if (sa->IsState()) return false;
			if (sa->IsOutput()) return false;
			return true;
		}).Any();
	}

#pragma region side effect transform infrastructure
	namespace Backends
	{
		static bool IsPair(CTRef graph) {
			const Deps *m;
			if (graph->Cast(m)) return IsPair(m->GetUp(0));
			const DataSource *ds;
			if (graph->Cast(ds)) return ds->HasPairLayout();
			else return IsOfExactType<Pair>(graph);
		}

		static bool IsReference(CTRef graph) {
			const Deps *m;
			if (graph->Cast(m)) return IsReference(m->GetUp(0));
			const DataSource *ds;
			if (graph->Cast(ds)) return ds->IsReference();
			else return false;
		}

		static CTRef DereferenceOnce(CTRef graph, CRRef rx) {
			//const Deps *m;
			//if (graph->Cast(m)) return Deps::Transfer(DereferenceOnce(m->GetUp(0)),m);
			const DataSource *ds;
			if (graph->Cast(ds) && ds->IsReference()) return ds->Dereference(rx);
			else return graph;
		}

		static CTRef DereferenceOnce(CTRef graph) { 
			return DereferenceOnce(graph, graph->GetReactivity());
		}

		static CTRef DereferenceAll(CTRef graph, CRRef rx) {
			//const Deps *m;
			//if (graph->Cast(m)) return Deps::Transfer(DereferenceAll(m->GetUp(0)),m);
			const DataSource *ds;
			if (graph->Cast(ds) && ds->IsReference()) return DereferenceAll(ds->Dereference(rx), rx);
			else return graph;
		}

		static CTRef DereferenceAll(CTRef graph) { 
			return DereferenceAll(graph, graph->GetReactivity());
		}

		static CTRef SplitFirst(CTRef graph) {
			const Deps *m;
			if (graph->Cast(m)) return Deps::Transfer(SplitFirst(m->GetUp(0)), m);

			const DataSource *ds;
			if (graph->Cast(ds)) return ds->First();
			else return First::New(graph);
		}

		static CTRef SplitRest(CTRef graph) {
			const Deps *m;
			if (graph->Cast(m)) return Deps::Transfer(SplitRest(m->GetUp(0)), m);

			const DataSource *ds;
			if (graph->Cast(ds)) return ds->Rest();
			else return Rest::New(graph);
		}

		static CTRef SyntheticPair(CTRef first, CTRef rest) {
			auto p = Pair::New(first, rest);
			if (auto px = p->Cast<Pair>()) {
				px->SetReactivity(new Reactive::LazyPair(first->GetReactivity(), rest->GetReactivity()));
			}
			return p;
		}

		bool FoldConstantInt(std::int64_t& i, CTRef node) {
			Native::Constant *c;
			if (node->Cast(c)) {
				if (c->FixedResult().IsInt32()) {
					i = *(std::int32_t*)c->GetPointer();
					return true;
				}
				if (c->FixedResult().IsInt64()) {
					i = *(std::int64_t*)c->GetPointer();
					return true;
				}
				return false;
			}

			Native::ITypedBinary *itb;
			if (node->Cast(itb)) {
				std::int64_t a, b;

				if (FoldConstantInt(a, itb->GetUp(0)) == false) return false;
				if (FoldConstantInt(b, itb->GetUp(1)) == false) return false;

				switch (itb->GetOpcode()) {
				case Native::Add: i = a + b; return true;
				case Native::Mul: i = a * b; return true;
				case Native::Sub: i = a - b; return true;
				case Native::Div: i = a / b; return true;
				default:
					return false;
				}
			}
			return false;			
		}

		static CTRef GetAccessor(CTRef graph) {
			const Deps *m;
			if (graph->Cast(m)) return Deps::Transfer(GetAccessor(m->GetUp(0)), m);

			const DataSource *ds;
			if (graph->Cast(ds)) return ds->GetAccessor();
			else return graph;
		}

		static CTRef ComputeSize(CTRef node) {
			const Pair* p;
			if (node->Cast(p)) return NI64(Add, ComputeSize(p->GetUp(0)), ComputeSize(p->GetUp(1)));

			const IFixedResultType *fr;
			if (node->Cast(fr)) return Native::Constant::New(int64_t(fr->FixedResult().GetSize()));

			const Reference* ref;
			if (node->Cast(ref)) return SizeOfPointer::New();

			const DataSource *ds;
			if (node->Cast(ds)) return ds->SizeOf();

			const Deps *m;
			if (node->Cast(m)) return ComputeSize(m->GetUp(0));

			const VariantTuple *vt;
			if (node->Cast(vt)) {
				return NI64(Add, 
							NI64(Mul, vt->GetUp(1), ComputeSize(vt->GetUp(0))),
							ComputeSize(vt->GetUp(2)));
			}

			const Boundary *b;
			if (node->Cast(b)) return ComputeSize(b->GetUp(0));

			assert(0 && "Should not be reachable");
			ResultTypeWithNoArgument rtnoa(node);
			return Native::Constant::New(int64_t(node->Result(rtnoa).GetSize()));
		}

		static bool ValidateSfxOutput(CTRef out) {
			if (auto constant = out->Cast<Native::Constant>()) {
				if (constant->FixedResult().GetSize() && constant->GetPointer() == nullptr) return false;
				return true;
			}
			return true;
		}

		static bool ValidateSfxUpstream(CTRef out) {
			if (auto ds = out->Cast<DataSource>()) {
				return ValidateSfxOutput(ds->GetAccessor());
			}
/*			if (auto proc = out->Cast<Deps>()) {
				return ValidateSfxOutput(proc->GetUp(0));
			}*/
			for (auto &u : out->Upstream()) {
				if (!ValidateSfxOutput(u)) return false;
			}
			return true;
		}

		/* return by value -> return by side effect */
		CTRef SideEffectTransform::operate(CTRef src) {
			auto result = src->SideEffects(*this);
			assert(ValidateSfxOutput(result) && ValidateSfxUpstream(result));
			return result;
		}

	#if INSTRUMENT_FUNCTIONS
		static CTRef InstrumentLabel(CTRef chain, const char *l) {
			auto diags = Native::ForeignFunction::New("int32", "kvm_label");
			diags->AddParameter("int32_t", chain, Type::Int32);
			diags->AddParameter("const char*", CStringLiteral::New(Type(l)), Type(l));
			return diags;
		}

		static CTRef InstrumentData(SideEffectTransform& sfx, const char* label, const Type& ty, CTRef data, CTRef chain) {
			if (IsPair(data)) {
				return InstrumentData(sfx, label, ty.Rest(), SplitRest(data),
									  InstrumentData(sfx, label, ty.First(), SplitFirst(data), chain));
			} else {
				auto diags = Native::ForeignFunction::New("int32", "kvm_instrument");
				diags->AddParameter("int32", chain, Type::Int32);
				diags->AddParameter("const char*", CStringLiteral::New(Type(label)), Type(label));

				if (ty.GetSize()) {
					data = Canonicalize(data, data->GetReactivity(), ty, true, false, sfx);
				} else {
					data = CStringLiteral::New(Type("()"));
				}

				diags->AddParameter("const void*", GetAccessor(data), ty);

				std::stringstream tyS;
				ty.OutputFormatString(tyS);

				tyS << " (";

				std::stringstream dataBundle;
				dataBundle << "(" << *data << ")";

				for (auto c : dataBundle.str()) {
					switch (c) {
					case '\n': break;
					case '%': tyS << "%%"; break;
					default: tyS.put(c); break;
					}
				}

				tyS << ")";


				Type tyStr(tyS.str().c_str());
				diags->AddParameter("const char*", CStringLiteral::New(tyStr), tyStr);
				return diags;
			}
		}
	#endif

		void SideEffectTransform::CompileSubroutineAsync(const char* label, const Type& arg, const Type& res, Subroutine* subr, CTRef sourceBody, CTRef args, CTRef results, bool wait) {
			assert(!CheckForArgInputs(results));

			TLS::WithNewStack([&]() mutable {
#ifndef NDEBUG
				if (TLS::GetCurrentInstance()->ShouldTrace("Compile", subr->GetLabel())) {
					std::clog << "Compile: [" << subr->GetLabel() << "]\n" << *sourceBody << "\n\nArgs: " << *args << "\nResults: " << *results ;
				}
#endif
				auto body = Compile(symbols, sourceBody, args, results, label, arg, res);
				subr->SetBody(body);
				return 0;
			});
            Deps *m;
            SubroutineMeta *sm;
            if (subr->GetBody()->Cast(m) && m->GetUp(m->GetNumCons()-1)->Cast(sm)) {
                if (sm->HasLocalState) AllocatesState();
                if (sm->HasSideEffects) MutatesGVars();
            } else assert(0 && "Missing metadata");
		}

#define TYPE_CHECK(type) IsOfExactType<type>(node) || 
#define IS_A(...) META_MAP(TYPE_CHECK,__VA_ARGS__) false
		
		void SideEffectTransform::AddSideEffect(CTRef WritePointer, CTRef WriteSource, CTRef ReadPointer, CRRef Reactivity, std::int64_t sizeIfKnownOrZero) {
			SideEffect fx{
				WritePointer,
				WriteSource,
				ReadPointer,
				Reactivity,
				sizeIfKnownOrZero
			};
			sfx.emplace_back(std::move(fx));
		}

		struct ReadWriteHazard {
			size_t readOffset;
			ReadWriteHazard(size_t readOffset = std::numeric_limits<size_t>::max()) :readOffset(readOffset) {}
			
			ReadWriteHazard& operator|=(const ReadWriteHazard& rhs) {
				readOffset = std::min(rhs.readOffset, readOffset);
				return *this;
			}

			ReadWriteHazard operator|(ReadWriteHazard rhs) {
				rhs |= *this;
				return rhs;
			}

			bool IsConnected() const {
				return readOffset < std::numeric_limits<size_t>::max();
			}

			bool Overlaps(const size_t writeLimit) const {
				if (writeLimit == 0) {
					// unknown size
					return IsConnected();
				}
				return readOffset < writeLimit;
			}

			ReadWriteHazard operator+(int b) const {
				return IsConnected() ? readOffset + b : readOffset;
			}
		};

		class DataHazardTransform : public CachedTransform<Typed, ReadWriteHazard> {
			std::int64_t hazardLimit;
		public:
			std::set<CTRef>& hazards;
			CTRef readPointer;
			DataHazardTransform(CTRef root, CTRef readPointer, std::set<CTRef>& hazards, std::int64_t size) :hazards(hazards), readPointer(readPointer), CachedTransform(root),hazardLimit(size) { }

			ReadWriteHazard operate(CTRef node) {
				Offset *ofs;
				if (*node == *readPointer) {
					return 0;
				} else if (node->Cast(ofs)) {
					std::int64_t o(0);
					FoldConstantInt(o, ofs->GetUp(1));
					assert(o >= 0 && "All offsets should be non-negative");
					auto h1 = (*this)(node->GetUp(0)) + (int)o;
					auto h2 = (*this)(node->GetUp(1));
					return h1 | h2;
				} 
				
				if (IS_A(Deps, AtIndex, Reference, BoundaryBuffer, BitCast)) {
					// these nodes may pass undereferenced pointers from up to downstream
					if (node->GetNumCons() == 0) return false;
					for (unsigned i(1); i < node->GetNumCons(); ++i) (*this)(node->GetUp(i));
					auto val = (*this)(node->GetUp(0));
					if (val.IsConnected()) return 0;
					else return val;
				} else if (IS_A(Native::Select)) {
					// this node could pass undereferenced pointers from up to downstream
					(*this)(node->GetUp(0));
					auto val = (*this)(node->GetUp(1));
					val |= (*this)(node->GetUp(2));
					return val;
				} else if (IS_A(Subroutine)) {
					ReadWriteHazard val;
					// first argument to subroutine is the state pointer which never carries hazards as it will not alias
					// all other arguments may be dereferenced
					(*this)(node->GetUp(0));
					for (unsigned i(1); i < node->GetNumCons(); ++i) val |= (*this)(node->GetUp(i));
					if (val.Overlaps(hazardLimit)) hazards.insert(node);
				} else if (IS_A(Dereference, Native::ForeignFunction)) {
					// these nodes may dereference pointers
					for (unsigned int i(0);i < node->GetNumCons();++i) 
						if ((*this)(node->GetUp(i)).Overlaps(hazardLimit)) hazards.insert(node);
				} else if (IS_A(Copy)) {
					(*this)(node->GetUp(0));
					(*this)(node->GetUp(2));
					if ((*this)(node->GetUp(1)).Overlaps(hazardLimit)) hazards.insert(node);
				} else if (IS_A(MultiDispatch)) {
					MultiDispatch *md;
					node->Cast(md);
					for (auto &d : md->GetDispatchees()) {
						(*this)(d.first);
					}
					for(unsigned i(0);i<node->GetNumCons();++i) {
						(*this)(node->GetUp(i));
					}
				} else {
					ReadWriteHazard val;
					for(unsigned i(0);i<node->GetNumCons();++i) {
						val |= (*this)(node->GetUp(i));
					}
#ifndef NDEBUG
					if (val.Overlaps(hazardLimit)) {
						std::clog << node->GetLabel() << " swallows a data hazard\n";
					}
#endif
				}
				return {};
			}
		};

		CTRef SideEffectTransform::Process(CTRef body, const Reactive::Node *rootReactivity) {
			CTRef processed = Deps::New(CopyData(results, (*this)(body), rootReactivity, true, false, false));

			struct HazardousEffect {
				CTRef ReadPointer;
				CTRef Pure;
				CTRef Effect;
				Deps *Guard;
				std::int64_t size;
			};
			std::vector<HazardousEffect> protectForHazards;

			while (sfx.size( )) {
				SideEffect fx = sfx.back( ); sfx.pop_back( );


				HazardousEffect hfx;
				hfx.ReadPointer = fx.ReadPointer;
				hfx.Guard = nullptr;
				hfx.size = fx.size;
				hfx.Pure = nullptr;

				if (fx.WriteValue == nullptr) {
					// this side effect releases the allocation
					hfx.Guard = Deps::New();
					hfx.Guard->Connect(fx.WritePointer);
					hfx.Effect = ReleaseBuffer::New(hfx.Guard, fx.ReadPointer->GetUp(0));
					protectForHazards.emplace_back(hfx);
				} else {
					// this side effect contains a memory write
					hfx.Pure = (*this)(fx.WriteValue);
					auto effects = CopyData(fx.WritePointer, hfx.Pure, fx.Reactivity, true, true, false);
					auto handleEffect = [&](CTRef n) {
						Copy* cpy;
						if (n->Cast(cpy)) {
							HazardousEffect write = hfx;
							write.Effect = cpy;
							write.Pure = cpy->GetUp(1);
							write.Guard = Deps::New();
							write.Guard->Connect(cpy->GetUp(0));
							cpy->Reconnect(0, write.Guard);
							protectForHazards.emplace_back(write);
						}
					};

					handleEffect(effects);
					for (auto fx : effects->Upstream()) handleEffect(fx);
				}
			}

			if (recursiveBranch) {
				recursiveBranch->Reconnect(0, GetStatePointer());
				SetStatePointer(recursiveBranch);
			}

			Deps* complete = Deps::New();
			complete->Connect(GetStatePointer());
			complete->Connect(processed);

			// add data protectors
			for (size_t i(0);i < protectForHazards.size();++i) {
				auto &h(protectForHazards[i]);
				if (h.ReadPointer != nullptr) {
					std::set<CTRef> hazards;
					std::int64_t hazardSize(h.size);

#ifndef NDEBUG
					bool found = false;
					for (auto n : Qxx::FromGraph(processed)) {
						if (*n == *h.ReadPointer) {
							if (found) {
								std::clog << "* Aliased read pointers:\n" << *processed;
								INTERNAL_ERROR("aliased read pointers detected in sfx compiler");
							}
							found = true;
						}
					}
#endif
					DataHazardTransform hazardAnalysis(processed, h.ReadPointer, hazards, hazardSize);
					hazardAnalysis.Go();
					for (size_t j(0);j < protectForHazards.size();++j) {
						if (protectForHazards[j].Pure) {
							DataHazardTransform hazardAnalysis(protectForHazards[j].Pure, h.ReadPointer, hazards, hazardSize);

							if (hazardAnalysis.Go().Overlaps(hazardSize) && i!=j) {
								hazards.insert(protectForHazards[j].Effect);
							}

							if (i != j) {
								hazardAnalysis(protectForHazards[j].Effect);
							}
						}
					}

					for (auto n : hazards) {
						h.Guard->Connect(n);
					}
				}
				complete->Connect(h.Effect);
			}

//#ifndef NDEBUG
#if 0
			// verify reactivity
			for (auto node : Qxx::FromGraph(complete)) {
				auto rx = node->GetReactivity();

				if (IS_A(Deps)) continue;

				std::unordered_set<Type> drivers;
				if (rx) {
					for (auto d : Qxx::FromGraph(rx)
						 .OfType<Reactive::DriverNode>()
						 .Select([](Reactive::DriverNode* dn) { return dn->GetID(); })) 
					{
						drivers.emplace(d);
					}
				}

				for (auto up : node->Upstream()) {
					if (IsOfExactType<BoundaryBuffer>(up)) continue;

					if (up->GetReactivity()) {
						std::unordered_set<Type> updrv;
						for (DriverSignature d : Qxx::FromGraph(up->GetReactivity())
							.OfType<Reactive::DriverNode>()
							.Select([](Reactive::DriverNode* dn) { return dn->GetID(); })) 
						{
							DriverSignature dsig{ d };
							if (d.GetDriverClass() == DriverSignature::User) {
								updrv.emplace(d);
							} else if (d.GetMetadata().GetDescriptor() == &Reactive::NullDriver) {
								goto skipCheck;
							}

						}

						if (rx == nullptr && updrv.size() || 
							(!updrv.empty() && 
							 !std::includes(updrv.begin(), updrv.end(), drivers.begin(), drivers.end())))
						{
							std::cerr << "\n\nUsing undefined value:" << *node << "\n" << up->GetLabel() << " @ " << *up->GetReactivity() << " -> ";
							if (rx) std::cerr << *rx;
							else std::cerr << "<static>";
							std::cerr << "\n";
						}
					} 
				skipCheck:;
				}
			}
#endif

			complete->Connect(SubroutineMeta::New(allocatesState, mutatesGVars));

			return cache.PostProcess(complete);
		}

		CTRef SideEffectTransform::GetDataLayout(CTRef graph) {
			Deps *m;
			for (;;) {
				if (graph->Cast(m)) graph = m->GetUp(0);
				else break;
			}

			const DataSource *ds;
			if (graph->Cast(ds)) return ds->GetDataLayout();
			else {
				ResultTypeWithNoArgument rt(graph);
				return Native::Constant::New(graph->Result(rt), 0);
			}
		}

		CTRef SideEffectTransform::GetDataAccessor(CTRef graph) {
			Deps *m;
			if (graph->Cast(m)) return Deps::Transfer(GetDataAccessor(m->GetUp(0)), m);

			const DataSource *ds;
			if (graph->Cast(ds)) return ds->GetAccessor();
			else return graph;
		}

		CTRef SideEffectTransform::GetDereferencedAccessor(CTRef graph, CRRef rx) {
			Deps *m;
			if (graph->Cast(m)) return Deps::Transfer(GetDereferencedAccessor(m->GetUp(0), rx), m);

			const DataSource *ds;
			if (graph->Cast(ds)) {
				while (ds->IsReference()) ds = ds->Dereference(rx);
				return ds->GetAccessor();
			} else return graph;
		}

		CTRef SideEffectTransform::GetDereferencedAccessor(CTRef graph) { 
			return GetDereferencedAccessor(graph, graph->GetReactivity());
		}

		static CTRef TransferDependencies(CTRef src, bool deref, CRRef rx, Deps*& deps) {
			if (deref) src = DereferenceAll(src, rx);

			Deps *m;
			if (src->Cast(m)) {
				if (deps == nullptr) deps = Deps::New( );
				for (unsigned i(0); i < m->GetNumCons(); ++i)
					deps->Connect(m->GetUp(i));

				src = TransferDependencies(m->GetUp(0), deref, rx, deps);
				if (deref) src = DereferenceAll(src, rx);
			}

			return src;
		}

		static CTRef RepackVector(CTRef root) {
			ExtractVectorElement *head;
			if (IsPair(root)) {
				if (GetAccessor(SplitFirst(root))->Cast(head)) {
					if (head->GetIndex() != 0) return root;
					int n = head->GetMaxIndex();
					CTRef i = SplitRest(root);
					for (int k = 1;k < n;++k) {
						ExtractVectorElement *el(nullptr);
						if (IsPair(i)) {
							SplitFirst(i)->Cast(el);
							i = SplitRest(i);
						}
						else i->Cast(el);
						if (el == nullptr) return root;
						if (el->GetUp(0) != head->GetUp(0)) return root;
						if (el->GetIndex() != k) return root;
					}
					// we can remove the unpack stage
					return head->GetUp(0);
				}
			}
			return root;
		}

		static bool IsFused(CRRef r) {
			return r == nullptr || r->IsFused();
		}
		
		static CRRef AnalyzeReactivity(CTRef node) {
			if (Deps* d = node->Cast<Deps>()) {
				return AnalyzeReactivity(d->GetUp(0));
			}

			if (Offset* o = node->Cast<Offset>()) {
				return AnalyzeReactivity(o->GetUp(0));
			}

			if (DataSource* ds = node->Cast<DataSource>()) {
				return AnalyzeReactivity(ds->GetAccessor());
			}

			if (IsOfExactType<BoundaryBuffer>(node)) return nullptr;

			return node->GetReactivity();
		}

		CTRef SideEffectTransform::CopyData(CTRef dst, CTRef src, CRRef reactivity, bool byVal, bool mutatesState, bool doesInit) {
			Deps *deps(nullptr);

			dst = TransferDependencies(dst, byVal, reactivity, deps);
			src = TransferDependencies(src, byVal, reactivity, deps);

			src = RepackVector(src);

			bool dstIsPair = IsPair(dst), srcIsPair = IsPair(src);

			if (dstIsPair || srcIsPair || !IsFused(reactivity)) {
				return Deps::New(CopyData(SplitFirst(dst), SplitFirst(src), reactivity ? reactivity->First() : 0, byVal, mutatesState,doesInit),
					CopyData(SplitRest(dst), SplitRest(src), reactivity ? reactivity->Rest() : 0, byVal, mutatesState,doesInit));
			}


			DataSource *dst_ds, *src_ds;

			CTRef finalDst, finalSrc, finalSz;
			Copy::Mode finalMode;

			if (dst->Cast(dst_ds)) {
				if (src->Cast(src_ds) && src_ds->CanTakeReference()) {
					finalDst = Reference::New(dst_ds->GetAccessor());
					finalSrc = Reference::New(src_ds->GetAccessor());
					finalSz = dst_ds->SizeOf();
					finalMode = Copy::MemCpy;
				} else if (Typed::IsNil(src)) {
					return deps ? Deps::Transfer(Typed::Nil(), deps) : src;
				} else {
					ResultTypeWithNoArgument tmp(src);

					finalDst = Reference::New(dst_ds->GetAccessor());
					finalSrc = GetDereferencedAccessor(src, reactivity);
					finalSz = dst_ds->SizeOf();
					finalMode = Copy::Store;
				}

				/* flatten out any Depss inside data sources */
				finalSrc = TransferDependencies(finalSrc, false, reactivity, deps);
				finalDst = TransferDependencies(finalDst, false, reactivity, deps);

				/* elided copy or null destination */
				if (*finalDst == *finalSrc || Typed::IsNil(dst_ds->GetAccessor())) {
					if (deps) return Deps::Transfer(Typed::Nil(), deps);
					else return Typed::Nil();
				}


				CTRef result(0);
				if (deps) finalDst = Deps::Transfer(finalDst, deps);
				assert(reactivity == nullptr || reactivity->IsFused());

#ifndef NDEBUG
				auto srcRx = AnalyzeReactivity(finalSrc);
				std::set<Type> opDrivers, dataDrivers;
				if (reactivity) {
					for (auto dn : Qxx::FromGraph(reactivity).OfType<Reactive::DriverNode>()) {
						opDrivers.emplace(dn->GetID());
					}
				}

				if (srcRx) {

					for (auto dn : Qxx::FromGraph(srcRx).OfType<Reactive::DriverNode>()) {
						dataDrivers.emplace(dn->GetID());
					}
				}

				if (opDrivers.empty()) {
					if (!dataDrivers.empty()) {
						std::cerr << "! Storing undefined value ! <partial -> static>\n" << *finalSrc << " -> " << *finalDst << "\n";
						for (auto& d : dataDrivers) {
							std::cerr << " - " << d << "\n";
						}
					}
				} else if (!dataDrivers.empty()) {
					if (!std::includes(dataDrivers.begin(), dataDrivers.end(),
										opDrivers.begin(), opDrivers.end())) {
						std::cerr << "! Storing undefined value !\n" << *finalSrc;
					}
				}
#endif

				result = Copy::New(finalDst, finalSrc, finalSz, finalMode, reactivity, 1, mutatesState, doesInit);
				return result;
			} else if (Typed::IsNil(dst)) {
				if (deps) return Deps::Transfer(dst, deps);
				else return dst;
			} else {
				INTERNAL_ERROR("Bad copy operation");
			}
		}

		bool GraphvizReduceProcEdgeWeight(std::ostream& dot, CTRef d, CTRef u) {
			// reduce layout weight of procedural data-less flows
			if (IsOfExactType<Deps>(d) || IsOfExactType<Deps>(u)) {
				if (d->GetNumCons() && d->GetUp(0) == u) return false;
				dot << "n" << u << " -> n" << d << " [weight=0.2, style=dashed, color=gray];\n";
				return true;
			}

			// simplify offset nodes
			std::int64_t tmp;
			if (IsOfExactType<Offset>(d) && d->GetUp(1) == u && FoldConstantInt(tmp, d->GetUp(1))) {
				return true;
			}
			if (IsOfExactType<Copy>(d) && d->GetUp(2) == u) {
				return true;
			}
			return false;
		}

		bool GraphvizReduceOffsets(std::ostream& dot, CTRef n, int color) {
			Offset *ofs;
			if (n->Cast(ofs)) {
				std::int64_t constant(0);
				if (FoldConstantInt(constant, ofs->GetUp(1))) {
					dot << "n" << n << " [label=\"[+" << constant << "]\"];\n";
					return true;
				}
			}

			SubroutineArgument *arg;
			if (n->Cast(arg)) {
				dot << "n" << n << " [label=\"arg" << arg->GetID() << "\"];\n";
				return true;
			}

			if (IsOfExactType<Deps>(n)) {
				dot << "n" << n << " [label=\"Depends\"];\n";
				return true;
			}

			return false;
		}

		Graph<Typed> SideEffectTransform::Compile(IInstanceSymbolTable& symbols, const CTRef pureBody, const CTRef arguments, const CTRef results, const char *l, const Type& argTy, const Type& resTy) {
			auto result = Graph<Typed>{};
			if (symbols.GetMemoized(std::make_tuple(pureBody, arguments, results), result)) {
				return result;
			}

			RegionAllocator compilationAllocator;
			CopyElisionTransform::ElisionMap emap;
			CopyElisionTransform elision(results, emap);

//			std::cout << "arg   : " << *arguments << std::endl;

			CTRef body = InliningTransform(pureBody, arguments).Go();

			//std::clog << "[src  ] : " << *pureBody << std::endl;
			//std::clog << "[w/arg] : " << *body << std::endl;
			//std::cout << "[out  ]: " << *results << std::endl;

			elision(body);

			SideEffectTransform::map_t cache;
			SideEffectTransform sfx(symbols, body, arguments, results, cache, elision);


			CTRef compiledBody(sfx.Process(body, pureBody->GetReactivity( ))); {
				/* clean copy for generational garbage collection and repatching the state pointers */
				RegionAllocator final;

				struct RepatchState : public Transform::Identity<const Typed> {
					std::unordered_set<Graph<Typed>> shareSubgraphs;
					CTRef localState;
#ifndef NDEBUG
					std::unordered_map<int64_t, Nodes::Buffer*> buffers;
#endif
				public:
					RepatchState(CTRef root, CTRef localState) :Identity(root),localState(localState) {}
					virtual CTRef operate(CTRef src) {
						auto f = shareSubgraphs.find(src);
						if (f == shareSubgraphs.end()) {
							shareSubgraphs.emplace(src);
						} else {
#ifndef NDEBUG
							Nodes::Buffer *buf;
							if (src->Cast(buf)) {
								auto bf = buffers.find(buf->GetUID());
								assert(bf == buffers.end() || bf->second == buf);
								buffers[buf->GetUID()] = buf;
							}

							std::stringstream a, b;
							src->Output(a); (*f)->Output(b);
							if (a.str() != b.str()) {
								std::clog << "False sharing?\n" << a.rdbuf() << "\n" << b.rdbuf() << "\n---------------\n";
							}
#endif
							src = *f;
						}

						SubroutineArgument* a;
						if (src->Cast(a) && a->IsLocalState()) {
							return localState;
						}
						return src->IdentityTransform(*this);
					}
				};

				Transform::Identity<const Typed> cpy(sfx.GetLocalStatePointer());
				CTRef finalBody = RepatchState(compiledBody, cpy.Go()).Go();
				symbols.SetMemoized(std::make_tuple(pureBody, arguments, results), finalBody);

				//ExportGraphviz(std::clog, "sfx", finalBody, GraphvizReduceOffsets, GraphvizReduceProcEdgeWeight);
			#if INSTRUMENT_FUNCTIONS
				if (l) {
					Deps* d = nullptr;
					finalBody->Cast(d);
					auto meta = d->GetUp(d->GetNumCons() - 1);
					d->Reconnect(d->GetNumCons() - 1, 
								 InstrumentData(sfx, "->", resTy, results,
									InstrumentData(sfx, "<-", argTy, arguments,
									   InstrumentLabel(
										   Deps::New(Native::Constant::New((int32_t)0), d->GetUp(0)), l))));
					d->Connect(meta);
				}
			#endif

				return finalBody;
			}
		}

		Graph<Typed> SideEffectTransform::Compile(IInstanceSymbolTable& symbols, CTRef body, const Type& arg, const Type& res) {
			auto cmp = Compile(symbols, body,
				DataSource::New(SubroutineArgument::In(1,nullptr,4), Reference::New(Native::Constant::New(arg, 0))),
				DataSource::New(SubroutineArgument::Out(2,4), Reference::New(Native::Constant::New(res, 0))), "root", arg, res);

			return cmp;
		}

		K3::TypeDescriptor pointerTag("Ptr");

		Type GetConcreteType(CTRef node) {
			IFixedResultType *fr;
			if (node->Cast(fr)) {
				auto c = fr->FixedResult();
				while (c.IsUserType()) c = c.UnwrapUserType();
				return c;
			}
			if (IsPair(node)) {
				return Type::Pair(GetConcreteType(First::New(node)), GetConcreteType(Rest::New(node)));
			}
			return Type(&pointerTag);
			INTERNAL_ERROR("bad type");
		}

		bool NeverHasData(CTRef node) {
			const IFixedResultType *fr;
			if (node->Cast(fr)) return fr->FixedResult().GetSize() == 0;
			const Reference *ref;
			if (node->Cast(ref)) return NeverHasData(ref->GetUp(0));
			const Dereference *deref;
			if (node->Cast(deref)) return NeverHasData(deref->GetUp(0));
			const Deps* m;
			if (node->Cast(m)) return NeverHasData(m->GetUp(0));
			const Offset* ofs;
			if (node->Cast(ofs)) return NeverHasData(m->GetUp(0));
			const DataSource* ds;
			if (node->Cast(ds)) return NeverHasData(ds->GetDataLayout( ));
			if (IsPair(node)) {
				return NeverHasData(First::New(node)) && NeverHasData(Rest::New(node));
			} else return false;
		}

		bool MayHaveData(CTRef node) { return !NeverHasData(node); }

		static CTRef ReduceType(CTRef node) {
			if (IsPair(node)) return Pair::New(
				ReduceType(First::New(node)),
				ReduceType(Rest::New(node)));

			if (IsOfExactType<First>(node)) return ReduceType(node->GetUp(0))->GraphFirst();
			if (IsOfExactType<Rest>(node)) return ReduceType(node->GetUp(0))->GraphRest();

			const IFixedResultType *fr;
			if (node->Cast(fr)) return Native::Constant::New(fr->FixedResult(), 0);

			const Reference *ref;
			if (node->Cast(ref)) return ReduceType(ref->GetUp(0));

			const Dereference* deref;
			if (node->Cast(deref)) return ReduceType(deref->GetUp(0));

			const DataSource* ds;
			if (node->Cast(ds)) return ReduceType(ds->GetDataLayout());

			const Deps *m;
			if (node->Cast(m)) return ReduceType(m->GetUp(0));

			const BoundaryBuffer* b;
			if (node->Cast(b)) return ReduceType(b->GetUp(0));

			const Native::Select* s;
			if (node->Cast(s)) return ReduceType(s->GetUp(1));

			assert(IsOfExactType<VariantTuple>(node) || IsOfExactType<Native::Constant>(node));
			return node;
		}
	};

#pragma endregion 

#pragma region implementation of supporting nodes

	namespace Nodes {
		using namespace Backends;

		void Copy::Output(std::ostream& strm) const {
			switch (mode) {
			case MemCpy: strm << "MemCpy"; break;
			case Store: strm << "Store"; break;
			default:INTERNAL_ERROR("Bad copy mode");
			}
		}

		SubroutineStateAllocation::SubroutineStateAllocation(const Subroutine* subr, CTRef state):subr(subr),TypedUnary(state) {
			subr->HasInvisibleConnections();
		}

		CTRef SubroutineStateAllocation::IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const {
			auto ssa = ConstructShallowCopy();
			ssa->subr = transform(ssa->subr)->Cast<Subroutine>();
			ssa->Reconnect(0, transform(ssa->GetUp(0)));
			ssa->subr->HasInvisibleConnections();
			assert(ssa->subr);
			return ssa;
		}

		void SubroutineArgument::Output(std::ostream& strm) const {
			switch (ID) {
			case 0: strm << "%self"; break;
			case 1: strm << "%state"; break;
			default:
				strm << (isOutput ? "%out" : "%in") << ID - 1;
			}
			strm << "<" << type << ">";
		}

		unsigned SubroutineArgument::ComputeLocalHash() const {
			size_t h(DisposableTypedLeaf::ComputeLocalHash());
			HASHER(h, ID);
			HASHER(h, type.GetHash());
			HASHER(h, isOutput ? 1 : (0 + (isReference ? 2 : 0)));
			//size_t ID;
			//bool isOutput;
			//Type type;
			//bool isReference;
			return unsigned(h);
		}

		CTRef Dereference::New(CTRef upstream, CRRef rx) { 
			Type c;
			Reference* r;
			if (upstream->Cast(r)) return r->GetUp(0);

			size_t align = 0;
			IAlignmentTrackerNode* at;
			if (upstream->Cast(at)) align = at->GetAlignment();

			c = GetConcreteType(upstream);
			if (c == Type(&pointerTag)) {
				auto dr = new Dereference((int)align, upstream, Type(false), true);
				dr->SetReactivity(rx);
				return dr;
			} else if (c.GetSize() > 0) {
				if (align > c.GetSize()) align = c.GetSize();
				while (c.IsUserType()) c = c.UnwrapUserType();
				auto dr = new Dereference((int)align, upstream, c, false);
				dr->SetReactivity(rx);
				return dr;
			} else {
				return Typed::Nil();
			}
		}

		CTRef Dereference::New(CTRef upstream) {
			return New(upstream, upstream->GetReactivity());
		}

		CTRef Dereference::New(CTRef upstream, CRRef rx, const Type& loadType) {
			if (IsOfExactType<Reference>(upstream)) return upstream->GetUp(0);
			else {
				if (loadType != Type(&pointerTag) && loadType.IsNil()) {
					return Typed::Nil();
				}
				size_t align = 0;
				IAlignmentTrackerNode *at;
				if (upstream->Cast(at)) align = at->GetAlignment();
				if (loadType.GetSize() && align > loadType.GetSize()) align = (int)loadType.GetSize();
				auto dr = new Dereference((int)align, upstream, loadType, loadType == Type(&pointerTag));
				dr->SetReactivity(rx);
				return dr;
			}
		}

		CTRef Dereference::New(CTRef upstream, const Type& loadType) { 
			return New(upstream, upstream->GetReactivity(), loadType);
		}

		void Dereference::Output(std::ostream& strm) const {
			strm << "Deref<";
			if (loadPtr) strm << "Pointer";
			else strm << loadType;
			strm << ">";
		}

		CTRef Reference::New(CTRef upstream) {
			/* maintain dependencies */
			Deps *m;
			if (upstream->Cast(m)) return Deps::Transfer(New(upstream->GetUp(0)), m);

			/* simplify */
			Dereference *dr;
			if (upstream->Cast(dr)) return upstream->GetUp(0);
			else return new Reference(0, upstream);
		}

		CTRef Offset::New(CTRef buffer, CTRef offset) {
			/* maintain dependencies */
			Native::Constant *constOffset;
			if (offset->Cast(constOffset)) {
				if (constOffset->FixedResult() == Type::Int64 && *(int64_t*)constOffset->GetPointer() == 0) {
					return buffer;
				}
			}
			Deps *m;
			if (buffer->Cast(m)) return Deps::Transfer(New(m->GetUp(0), offset), m);
			
			auto o = new Offset(buffer, offset);
			o->SetReactivity(buffer->GetReactivity());
			return o;
		}

		int Offset::GetAlignment() const {
			IAlignmentTrackerNode *at;
			int offset = 0;

			CTRef n = this;
			Offset *on;

			while (n->Cast(on)) {
				Native::Constant *c;
				if (on->GetUp(1)->Cast(c) && c->FixedResult().IsInt64()) {
					offset += (int)*(std::int64_t*)c->GetPointer();
				} else return 0;
				n = on->GetUp(0);
			}


			if (n->Cast(at)) {
				int align = at->GetAlignment();
				if (align) {
					align = -((-offset) | (-align));
					align = (align & ~(align - 1));
				}
				return align;
			}
			return 0;
		}

		const DataSource* DataSource::Dereference(CRRef rx) const {
			auto dataLayout(Dereference::New(GetDataLayout(), nullptr));
			return New(Dereference::New(GetAccessor(), rx, GetConcreteType(dataLayout)), dataLayout);
		}

		const DataSource* DataSource::Dereference() const { 
			return Dereference(GetAccessor()->GetReactivity());
		}

		const DataSource* DataSource::Reference() const {
			return New(Reference::New(GetAccessor()), Reference::New(GetDataLayout()));
		}

		CTRef DataSource::SizeOf() const {
			return ComputeSize(GetDataLayout());
		}

		const DataSource* DataSource::First() const {
			if (IsReference()) return Dereference()->First()->Reference();
			auto ds(New(GetAccessor(), First::New(GetDataLayout())));
			return ds;
		}

		const DataSource* DataSource::Rest() const {
			if (IsReference()) return Dereference()->Rest()->Reference();

			auto dataLayout(Rest::New(GetDataLayout()));

			auto ds{ 
				New(
					Dereference::New(Offset::New(Reference::New(GetAccessor()), First()->SizeOf()),
									 nullptr,
									 GetConcreteType(dataLayout)),
					dataLayout)
			};
			return ds;
		}

		bool DataSource::CanTakeReference() const {
			auto a(GetAccessor());
			do {
				if (IsOfExactType<Nodes::Dereference>(a)) return true;
			} while (IsOfExactType<Deps>(a) && (a = a->GetUp(0)));
			return false;
		}

		bool DataSource::IsReference() const {
			return IsOfExactType<Nodes::Reference>(GetDataLayout()) || IsOfExactType<Offset>(GetDataLayout());
		}

		bool DataSource::HasPairLayout() const {
			return IsOfExactType<Pair>(GetDataLayout());
		}

		const DataSource* DataSource::New(CTRef source) {
			if (IsPair(source)) {
				auto fst(New(source->GraphFirst()));
				auto rst(New(source->GraphRest()));

				return New(
					Pair::New(
						fst->GetAccessor(),
						rst->GetAccessor()),
					Pair::New(
						fst->GetDataLayout(),
						rst->GetDataLayout()));
			}

			const DataSource *ds;
			if (source->Cast(ds)) return ds;
			auto nds = New(source, ReduceType(source));
			nds->SetReactivity(source->GetReactivity());
			return nds;
		}

		DataSource* DataSource::New(CTRef access, CTRef dataLayout) {
			auto ds = new DataSource(access, dataLayout);
			ds->SetReactivity(access->GetReactivity());
			return ds;
		}

		const DataSource* DataSource::Conform(SideEffectTransform& sfx, const DataSource* match, CRRef r) const {
			if (match->IsReference()) {
				{
					auto tmp = Conform(sfx, match->Dereference(r), r);
					if (tmp->IsReference()) return tmp->Dereference(r);

					if (*GetDataLayout() == *match->GetDataLayout()) return this;

					if (match->HasPairLayout()) {
						auto fst(First()->Conform(sfx, match, r));
						auto rst(Rest()->Conform(sfx, match, r));
						return DataSource::New(Pair::New(fst->GetAccessor(), rst->GetAccessor()),
											   Pair::New(fst->GetDataLayout(), rst->GetDataLayout()));
					}
				}

				// can I reference to match?
				auto tmp(this);
				while (tmp->IsReference()) {
					tmp = tmp->Dereference(r);
					if (*tmp->GetDataLayout() == *match->GetDataLayout()) return tmp;
				}

				// can I dereference to match?
				tmp = this;
				while (IsOfExactType<Nodes::Dereference>(tmp->GetAccessor())) {
					tmp = tmp->Reference();
					if (*tmp->GetDataLayout() == *match->GetDataLayout()) return tmp;
				}
			}
			// force conformance
			auto buf(Buffer::New(sfx, match->SizeOf(), Buffer::Stack, 16));
			auto buf_ds(DataSource::New(buf, match->Reference()->GetDataLayout()));
			return DataSource::New(
				Deps::New(buf_ds->Dereference(r)->GetAccessor(), sfx.CopyData(buf_ds, this, r, true, false, false)),
				match->GetDataLayout());
		}

		CTRef DataSource::GraphFirst() const { return First(); }
		CTRef DataSource::GraphRest() const { return Rest(); }

		CTRef VariantTuple::GraphFirst() const {
			return GetUp(0);
		}

		CTRef VariantTuple::GraphRest() const {
			return VariantTuple::New(GetUp(0), NI64(Sub, GetUp(1), Native::Constant::New(int64_t(1))), GetUp(2), GetRecurrenceDelta());
		}

		Buffer* Buffer::ConstructShallowCopy() const {
			auto buf = new Buffer(0, GetUp(0), alloc, alignment);
			buf->GUID = std::hash<void*>()(buf);
			return buf;
		}

		CTRef Buffer::New(SideEffectTransform& sfx, const Type& t, Allocation a) {
			return New(sfx, t.GetSize(), a);
		}

		CTRef Buffer::New(SideEffectTransform& sfx, size_t size, Allocation a) {
			
			if (size == 0) return NewEmpty();

			size_t align = 32;
			while (align > size) align >>= 1;
			auto buf = new Buffer(0, Native::Constant::New(static_cast<int64_t>(size)), a, (int)align);
			buf->GUID = std::hash<void*>()(buf);
			if (a == Stack && size != 0) {
				sfx.AddSideEffect(buf, nullptr, buf, 0);
			}
			return buf;
		}

		CTRef Buffer::NewEmpty() {
			return new Buffer(0, Typed::Nil(), Empty, -1);
		}

		CTRef Buffer::New(SideEffectTransform& sfx, CTRef sz, Allocation a, size_t align) {
			std::int64_t i{ 0 };
			if (FoldConstantInt(i, sz)) {
				return New(sfx, (size_t)i, a);
			}

			auto buf = new Buffer(0, sz, a, (int)align);
			buf->GUID = std::hash<void*>()(buf);
			if (a == Stack) {
				sfx.AddSideEffect(buf, nullptr, buf, 0);
			}
			return buf;
		}

		void Buffer::Output(std::ostream& strm) const {
			strm << "Buffer" << GUID;
		}

		CTRef Deps::GraphFirst() const {
			return Transfer(First::New(GetUp(0)), this);
			//New(First::New(GetUp(0)),this);
		}

		CTRef Deps::GraphRest() const {
			return Transfer(Rest::New(GetUp(0)), this);
			//			return New(Rest::New(GetUp(0)),this);
		}

		CTRef Deps::IdentityTransform(GraphTransform<const Typed, CTRef>& state) const {
			if (GetNumCons() == 1) return state(GetUp(0));
			else return TypedPolyadic::IdentityTransform(state);
		}

		CTRef Deps::Transfer(CTRef upstream, const Deps* deps) {
			DataSource *ds;
			if (upstream->Cast(ds)) {
				return DataSource::New(Transfer(ds->GetAccessor(), deps), ds->GetDataLayout());
			}
			Deps *m(New());
			m->Connect(upstream);
			if (deps->isDataProtector) {
				/* must maintain this Deps intact because its identity (via pointer) matters */
				m->Connect(deps);
			} else {
				for (unsigned i(1); i < deps->GetNumCons(); ++i) m->Connect(deps->GetUp(i));
			}
			return m;
		}

		void Deps::Connect(CTRef node) {
			DataSource *ds;

			Deps *m;
			if (node->Cast(m) && m->isDataProtector == false) {
				/* flatten chained Deps that are not data protectors */
				for (int i = 0; i < m->GetNumCons(); ++i) {
					if (i == m->GetNumCons() - 1) {
						node = m->GetUp(i);
						break;
					} else {
						Connect(m->GetUp(i));
					}
				}
			}

			if (GetNumCons()) {
				for (;;) {
					Invariant::Constant *ic;

					if (node->Cast(ds)) {
						/* ignore layout for dependencies */
						node = ds->GetAccessor();
						continue;
					} else if (IsPair(node)) {
						Connect(SplitFirst(node));
						node = SplitRest(node);
						continue;
					} else if (node->Cast(ic)) {
						return;
					} else if (IS_A(Reference)) {
						node = node->GetUp(0);
						continue;
					}
					break;
				}
			} else {
				SetReactivity(node->GetReactivity());
				assert(!node->Cast(ds));
			}
			TypedPolyadic::Connect(node);
		}

		CTRef SubroutineArgument::New(bool isInput, size_t ID, CTRef graph, CRRef rx, const char *l) {
			if (l == nullptr) {
				l = isInput ? "in" : "out";
			}
			Deps *m;
			if (graph->Cast(m)) return New(isInput, ID, m->GetUp(0), rx, l);

			const DataSource *ds;
			if (graph->Cast(ds)) {
				while (ds->CanTakeReference()) ds = ds->Reference();
				if (ds->IsReference()) {
					int align = 0;
					IAlignmentTrackerNode *at;
					if (ds->GetAccessor()->Cast(at)) align = at->GetAlignment();
					if (isInput) {
						return In(ID, rx, align, l);
					} else {
						return Out(ID, align, l);
					}
				}
			}
			ResultTypeWithNoArgument rtnoa(graph);
			auto t = graph->Result(rtnoa);
			if (isInput) {
				return In(ID, rx, t, l);
			} else {
				return Out(ID, t, l);
			}
		}
#pragma endregion 

#pragma region per-node side effects

		/* first and rest do not trigger dereferencing */
		CTRef First::SideEffects(SideEffectTransform& sfx) const {
			return IdentityTransform(sfx);
		}

		CTRef Rest::SideEffects(SideEffectTransform& sfx) const {
			return IdentityTransform(sfx);
		}

		CTRef Pair::SideEffects(SideEffectTransform& sfx) const {
			return IdentityTransform(sfx);
		}

		CTRef Deps::SideEffects(SideEffectTransform& sfx) const {
			Deps *side(Deps::New());
			for (unsigned i(1); i < GetNumCons(); ++i) side->Connect(GetAccessor(sfx(GetUp(i))));
			if (GetNumCons()) return Deps::New(sfx(GetUp(0)), side);
			else return side;
		}

		CTRef DataSource::SideEffects(SideEffectTransform& sfx) const {
			/* leaf node of side effect transform */
			return this;
		}

		CTRef Argument::SideEffects(SideEffectTransform& sfx) const {
			return sfx(sfx.GetArgument());
		}

		CTRef SetGlobalVariable::SideEffects(SideEffectTransform& sfx) const {
			KRONOS_UNREACHABLE;
			sfx.MutatesGVars();

			auto ptr = GetSlot::New(sfx.GetSymbolTable().GetIndex(uid));
				
				/*Offset::New(SubroutineArgument::Self(),
								   NI64(Mul,
													 Native::Constant::New(int64_t(sfx.GetSymbolTable().GetIndex(uid))),
													 SizeOfPointer::New()));*/

			CTRef new_val = sfx(GetUp(0));

			if (byRef == false) {
				const DataSource* ptr_ds = DataSource::New(ptr, Reference::New(ReduceType(GetUp(0))));
				return sfx.CopyData(ptr_ds, new_val, 0, true, true, false);
			}

			auto rx = GetReactivity();

			const DataSource* ptr_ds = DataSource::New(ptr, Reference::New(ReduceType(GetUp(0))));

			const DataSource* new_ds;
			const Deps* new_m;
			if (new_val->Cast(new_m) && new_m->GetUp(0)->Cast(new_ds)) {
				auto tmp1(ptr_ds), tmp2(new_ds);
				while (tmp1->IsReference()) tmp1 = tmp1->Dereference(rx);
				while (tmp2->IsReference()) tmp2 = tmp2->Dereference(rx);
				if (*tmp1->GetDataLayout() == *tmp2->GetDataLayout()) {
					//return sfx.CopyData(ptr_ds->Dereference(),tmp2->Reference(),0,false);
					return Copy::New(ptr_ds->GetAccessor(),
									 tmp2->Reference()->Reference()->GetAccessor(),
									 Deps::Transfer(SizeOfPointer::New(), new_m), Copy::Store, new_val->GetReactivity(), 1, true, false);
				}
			}


			auto buf(Buffer::New(sfx, ptr_ds->Dereference(rx)->Dereference(rx)->SizeOf(), Buffer::Stack, 16));
			const DataSource* buf_ds = DataSource::New(buf, ptr_ds->Dereference(rx)->GetDataLayout());
			return Deps::New(Copy::New(ptr_ds->GetAccessor(), Reference::New(buf), SizeOfPointer::New(), Copy::Store, 0, 1, true, false),
							  sfx.CopyData(buf_ds, new_val, 0, true, true, false));
		}

		CTRef GetGlobalVariable::SideEffects(SideEffectTransform& sfx) const {
			CTRef initializer = nullptr;;
			auto layout(Reference::New(Native::Constant::New(t, 0)));

			auto slotIndex = sfx.GetSymbolTable().GetIndex(uid);

			if (GetNumCons()) {
				initializer = sfx(GetUp(0));
			} else if (IsConfigurationSlot()) {
				initializer = DataSource::New(Configuration::New(t, slotIndex), layout);
			}

			if (initializer) {
				/* this variable has an initializer */
				auto buf = Buffer::New(sfx, Native::Constant::New(int64_t(t.GetSize())), Buffer::Module, 16);
				initializer = Deps::New(buf, sfx.CopyData(DataSource::New(buf, layout), initializer, 
									    sfx.GetSymbolTable().GetInitializerReactivity(), true, true, true));
			} 

			if (key.IsNil() == false) {
				sfx.GetSymbolTable().RegisterExternalVariable(key, t, uid, k, vectorRate, clock);
			}

			auto slot = GetSlot::New(slotIndex, initializer);
			slot->SetReactivity(GetReactivity());
			return DataSource::New(slot, layout);
		}

		CTRef ExternalAsset::SideEffects(SideEffectTransform& sfx) const {
			return DataSource::New(ConstructShallowCopy(), Reference::New(Native::Constant::New(dataType, nullptr)));
		}

		/* allocate temporary space for non-elided returns */
		class BufferBuilder : public GraphTransform<const Typed, CTRef> {
			CTRef t;
			SideEffectTransform& sfx;
			CRRef r;
		public:
			BufferBuilder(CTRef t, CRRef reactivity, SideEffectTransform& sfx) :t(t), sfx(sfx), r(reactivity) {}
			CTRef operate(CTRef node) {
				if (IsPair(node)) {
					return (CTRef)SyntheticPair(
						BufferBuilder(t->GraphFirst(), r ? r->First() : r, sfx)(node->GraphFirst()),
						BufferBuilder(t->GraphRest(), r ? r->Rest() : r, sfx)(node->GraphRest()));
				}

				if (Typed::IsNil(node)) {
					return (CTRef)DataSource::New(
						Buffer::New(sfx, ComputeSize(t), Buffer::Stack, 16),
						Reference::New(t));
				}
				return node;
			}
		};

		/* default plumbing action: dereference and unwrap all data sources */
		CTRef Typed::SideEffects(SideEffectTransform& sfx) const {
			auto newNode(ConstructShallowCopy());
			for (unsigned i(0); i < newNode->GetNumCons(); ++i) {
				newNode->Reconnect(i, sfx.GetDereferencedAccessor(sfx(GetUp(i)), newNode->GetReactivity()));
			}
			return newNode;
		}

		/* ensure that the output of the boundary is in same LLVM format regardless of whether it comes from cache or live data */
		static CTRef FactorBoundaries(Backends::SideEffectTransform& sfx, CTRef upstream, CRRef up, const Reactive::Node *down) {
			if (IsPair(upstream) || !IsFused(up)) {
				return SyntheticPair(
					FactorBoundaries(sfx, upstream->GraphFirst(), up->First(), down->First()),
					FactorBoundaries(sfx, upstream->GraphRest(), up->Rest(), down->Rest()));
			}

			if (Typed::IsNil(upstream)) return upstream;

			auto upstreamType(ReduceType(upstream));

			auto bufferPtr(sfx.GetLocalStatePointer());
			auto buffer(DataSource::New(bufferPtr, Reference::New(upstreamType)));
			sfx.SetLocalStatePointer(Offset::New(bufferPtr, buffer->Dereference(nullptr)->SizeOf()));

			const DataSource *up_ds;
			if (upstream->Cast(up_ds) && up_ds->IsReference()) {
				while (up_ds->IsReference()) up_ds = up_ds->Dereference();
				return DataSource::New(
					BoundaryBuffer::New(up_ds->Reference()->GetAccessor(),
										Deps::New(
											bufferPtr,
											sfx.CopyData(buffer, upstream, up, true, true, true)),
										up),
					buffer->GetDataLayout()); // ->Dereference(nullptr);
			} else {
				return
					BoundaryBuffer::New(
						sfx.GetDereferencedAccessor(upstream, up), 
						Deps::New(sfx.GetDereferencedAccessor(buffer, nullptr),
								  sfx.CopyData(buffer, upstream, up, true, true, true)), 
						up);
			}
		}

		Type ReduceSignature(const Type& inc) {
			if (inc.IsUserType()) {
				// remove mask sequence from analysis
				DriverSignature tmp(inc);
				tmp.Masks().clear();
				return tmp;
			} else return inc;
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

		CTRef Boundary::SideEffects(Backends::SideEffectTransform& sfx) const {
			auto upd(sfx(GetUp(0)));

			std::unordered_set<Type> upDrivers, dnDrivers;

			// ignore all driver priorities in boundary generation
			for (auto d : Qxx::FromGraph(upstreamReactivity).OfType<Reactive::DriverNode>()
				 .Select([](const Reactive::DriverNode* dn) {return dn->GetID(); })) {
				DriverSignature dsgn{ d };
				dsgn.SetPriority(Type::Nil);
				upDrivers.insert(dsgn);
			}

			if (upDrivers.empty()) return upd;

			for (auto d : Qxx::FromGraph(GetReactivity()).OfType<Reactive::DriverNode>()
				 .Select([](const Reactive::DriverNode* dn) {return dn->GetID(); })) {
				DriverSignature dsgn{ d };
				dsgn.SetPriority(Type::Nil);
				dnDrivers.insert(dsgn);
			}

			// is there a downstream driver that needs a cache?
			for (auto d : dnDrivers) {
				if (upDrivers.find(d) == upDrivers.end()) {
					// no direct match, is there a supersampled match?
					DriverSignature dsig(d);
					int64_t dmul = static_cast<int64_t>(dsig.GetMul()), ddiv = static_cast<int64_t>(dsig.GetDiv());
					for (auto u : upDrivers) {
						DriverSignature usig(u);
						int64_t umul = static_cast<int64_t>(usig.GetMul()), udiv = static_cast<int64_t>(usig.GetDiv());
						if (dsig.GetMetadata() != usig.GetMetadata() ||
							umul % dmul != 0 ||
							ddiv % udiv != 0) {
							// cache is needed as downstream driver is not an exact fraction of the upstream
							return FactorBoundaries(sfx, upd, upstreamReactivity, GetReactivity());
						}
					}
				}
			}

			// no downstream driver needs a cache
			return upd;
		}

		namespace ReactiveOperators { 
		CTRef Merge::SideEffects(Backends::SideEffectTransform& sfx) const {

			auto bufferPtr(sfx.GetStatePointer());
			auto buffer(DataSource::New(bufferPtr, Reference::New(Native::Constant::New(FixedResult(),nullptr))));
			CTRef output = Deps::New(buffer);
			auto rx = GetReactivity();
			sfx.SetStatePointer(Offset::New(bufferPtr, buffer->Dereference(rx)->SizeOf()));

			for (unsigned i(0);i < GetNumCons();++i) {
				auto upstream(sfx(GetUp(i)));
				auto up = upRx[i];
				output = Deps::New(output, sfx.CopyData(buffer, upstream, up, true, true, true));
			}
			return output;
		}
	}


		static bool IsCanonical(CTRef data) {
			const DataSource* ds;
			if (data->Cast(ds) && ds->HasPairLayout() == false) {
                return true;
			} else return false;
		}

		namespace Native {
			CTRef ForeignFunction::SideEffects(SideEffectTransform& sfx) const {

				ForeignFunction *ff = ConstructShallowCopy();
				ff->compilerNode = true;
				if (Symbol.back() == '!') {
					sfx.MutatesGVars();
				} 
				CTRef returnValue = DataSource::New(Typed::Nil(), Native::Constant::New(Type(false), nullptr));
				for(int i(ff->GetNumCons() - 1); i >= 0; --i) {
					if (CTypes[i] == "sizeof") {
						ff->Reconnect(i, Native::Constant::New((std::int64_t)KTypes[i].GetSize()));
					} else if (CTypes[i] == "typeof") {
						// provide type information for JIT interaction via a memoized type
						static_assert(sizeof(std::int64_t) >= sizeof(const void*), "Current scheme can handle up to 64-bit pointers");
						std::int64_t persistent = (std::int64_t)TLS::GetCurrentInstance()->Memoize(KTypes[i]);
						ff->Reconnect(i, Native::Constant::New(persistent));
					} else if (CTypes[i] == "const char*") {
						// pass invariant string as a c-str
						ff->Reconnect(i, CStringLiteral::New(KTypes[i]));
					} else {
						bool isPtr, isOutput;
						CTypeToKronosType(CTypes[i], isOutput, isPtr);
						CTRef up(nullptr);
						if (isOutput) {
							assert(isPtr);
							up = Canonicalize(sfx(ff->GetUp(i)), GetReactivity(), KTypes[i], isPtr, isOutput, sfx);							
							returnValue = SyntheticPair(
								up, 
								returnValue);
						} else {
							up = Canonicalize(sfx(ff->GetUp(i)), GetReactivity(), KTypes[i], isPtr, isOutput, sfx);
						}
						ff->Reconnect(i, GetAccessor(up));
					}
				}
				return Pair::New(
					DataSource::New(ff, Native::Constant::New(ff->ReturnValue, nullptr)),
					Deps::New(returnValue, ff));
			}

			CTRef SelfID::SideEffects(SideEffectTransform& sfx) const {
				return BitCast::New(Type::Int64, 1, SubroutineArgument::Self());
			}

			CTRef Constant::SideEffects(SideEffectTransform& sfx) const {
				if (FixedResult().IsNativeType() || FixedResult().GetSize() == 0) return ConstructShallowCopy();
				else return DataSource::New(ConstructShallowCopy(), Reference::New(Native::Constant::New(FixedResult(), nullptr)));
			}
		}

		static CTRef BuildSelectTree(bool hasNil, CTRef vector, CTRef index, CRRef r, const Type& elem, int32_t start, size_t end, SideEffectTransform& sfx) {
			if (end > 1) {
				CTRef ai = AtIndex::New(
					Type::Chain(elem,end,Type()), 
					GetAccessor(Canonicalize(vector, r, Type::Chain(elem, end, Type::Nil), true, false, sfx)),
					Native::MakeInt32("usub", Native::Sub, index, Native::Constant::New(start)));

				if (auto i = ai->Cast<AtIndex>()) i->SetReactivity(r);
				return ai;
			} else {
				if (hasNil) return SplitFirst(vector);
				else return vector;
			}
		}

		CTRef AtIndex::SideEffects(Backends::SideEffectTransform& sfx) const {
			auto vec(sfx(GetUp(0)));
			auto idx(sfx(GetUp(1)));

			auto rx = GetReactivity();
			auto tree = BuildSelectTree(
				vectorTy.IsNilTerminated(), vec, GetAccessor(DereferenceAll(idx, rx)), 
				rx, elem, 0, size, sfx);

			return DataSource::New(tree, Reference::New(Native::Constant::New(elem, nullptr)));
		}

		static void GetSliceBounds(Backends::SideEffectTransform& sfx, int staticLen, const Type& elementType, 
								   CRRef rx, CTRef vec, CTRef& ptr, CTRef &offset, CTRef &limit) {
			if (staticLen < 0) {
				ptr = BitCast::New(Type::ArrayView(elementType), 1,
								   GetAccessor(DereferenceAll(SplitFirst(vec), rx)));
				auto ofsl = SplitRest(vec);
				offset = GetAccessor(DereferenceAll(SplitFirst(ofsl), rx));
				limit = GetAccessor(DereferenceAll(SplitRest(ofsl), rx));
			} else {
				auto bufType = Type::List(elementType, staticLen);
				ptr = GetAccessor(Canonicalize(vec, rx, bufType, true, false, sfx));
				limit = Native::Constant::New((int32_t)staticLen);
				offset = Native::Constant::New((int32_t)0);
			}
		}

		CTRef SubArray::SideEffects(Backends::SideEffectTransform& sfx) const { 
			CTRef ptr = nullptr, offset = nullptr, limit = nullptr;
			
			auto vec(sfx(GetUp(0)));
			auto skip = sfx(GetUp(1));
			GetSliceBounds(sfx, src.sourceLen, src.GetElementType(), GetReactivity(), vec, ptr, offset, limit);

			skip = NI32(Add, skip, offset);
			skip = MakeConversionNode<int64_t, Native::ToInt64>(skip, Type::Int32, GetReactivity());

			auto zero = Native::Constant::New((int64_t)0);
			auto skipBelowZero = NI64(Min, zero, skip);
			auto skipAboveZero = NI64(Max, zero, skip);

			limit = MakeConversionNode<int64_t, Native::ToInt64>(limit, Type::Int32, GetReactivity());
			offset = MakeConversionNode<int64_t, Native::ToInt64>(offset, Type::Int32, GetReactivity());

			auto buf = Buffer::New(sfx, src.GetElementType().GetSize() * sliceLen, Buffer::Allocation::StackZeroed);
			auto cSz = Native::Constant::New((int64_t)src.GetElementType().GetSize());
			
			auto size =
				NI64(Max, zero,
					NI64(Mul, cSz,
						NI64(Min,
							NI64(Add, Native::Constant::New((int64_t)sliceLen), skipBelowZero),
							NI64(Sub, limit, skipAboveZero))));

			auto cpy =
				Copy::New(
					Offset::New(buf,
								NI64(Mul,
									 skipBelowZero,
									 Native::MakeInt64("neg", Native::Neg, cSz))),
					Offset::New(GetAccessor(ptr),
								NI64(Mul, skipAboveZero, cSz)),
					size, Copy::MemCpy, GetReactivity(), 1, false, true);

			return DataSource::New( 
				Deps::New(buf, cpy),
				Reference::New(Native::Constant::New(FixedResult(), nullptr)));
		} 

		static CTRef MakeSlice(Backends::SideEffectTransform& sfx, CTRef ptr, CTRef offset, CTRef length, CRRef rx) {
			auto rawSliceTy = Type::Tuple(Type::Int64, Type::Int32, Type::Int32);
			auto sliceBuf = Buffer::New(sfx, rawSliceTy, Buffer::Stack);

			auto int64sz = Native::Constant::New((int64_t)Type::Int64.GetSize());
			auto int32sz = Native::Constant::New((int32_t)Type::Int32.GetSize());

			Deps* output = Deps::New();
			auto bufPtr = sliceBuf;

			output->Connect(sliceBuf);

			output->Connect(
				Copy::New(bufPtr, ptr, int64sz, Copy::Store, rx, 1, false, true));
			bufPtr = Offset::New(bufPtr, int64sz);


			output->Connect(
				Copy::New(bufPtr, offset, int32sz, Copy::Store, rx, 1, false, true));
			bufPtr = Offset::New(bufPtr, int32sz);

			output->Connect(
				Copy::New(bufPtr, length, int32sz, Copy::Store, rx, 1, false, true));

			return DataSource::New(
				output,
				Reference::New(Native::Constant::New(rawSliceTy, nullptr)));
		}

		CTRef Slice::SideEffects(Backends::SideEffectTransform& sfx) const {
			auto vec = sfx(GetUp(0));
			auto skip = sfx(GetUp(1)); 
			auto take = sfx(GetUp(2));


			CTRef limit = nullptr;
			CTRef offset = nullptr;
			CTRef ptr = nullptr;

			GetSliceBounds(sfx, src.sourceLen, src.GetElementType(), GetReactivity(), vec, ptr, offset, limit);

			auto skipOffs = NI32(Add, offset, skip);
			auto length = NI32(Min, take, NI32(Sub, limit, skipOffs));

			return MakeSlice(sfx, BitCast::New(Type::Int64, 1, ptr),
							 skipOffs, length, GetReactivity());
		}

		CTRef SliceArity::SideEffects(Backends::SideEffectTransform& sfx) const {
			CTRef ptr, offset, limit;
			GetSliceBounds(sfx, -1, /* ignored */ Type::Float32, GetReactivity(), sfx(GetUp(0)), ptr, offset, limit);
			return NI32(Sub, limit, offset);
		}

		CTRef RingBuffer::SideEffects(SideEffectTransform& sfx) const {
			CTRef evictPtr, bufferPtr, rover, statePtr(sfx.GetLocalStatePointer());
			CTRef bufferLen = nullptr;
			if (!lenConfigurator && len < 2) {
				bufferPtr = evictPtr = statePtr;
				rover = Native::Constant::New(int32_t(0));
				sfx.SetLocalStatePointer(Offset::New(statePtr, Native::Constant::New(int64_t(elementType.GetSize()))));

				/* emit initializer code */
				auto initializer(sfx.CopyData(
					DataSource::New(bufferPtr, Reference::New(Native::Constant::New(elementType, 0))),
					sfx(GetUp(0)), sfx.GetSymbolTable().GetInitializerReactivity(),
					true,true,true));

				evictPtr = bufferPtr = Deps::New(bufferPtr, initializer);
			} else {
				/* generate ring buffer loop */
				bufferPtr = statePtr;
				
				switch (len) {
				default:
					bufferLen = Native::Constant::New((int32_t)len);
					break;
				case 0: {
					Type clockSig;
					
					if (GetReactivity()) {
						clockSig = Qxx::FromGraph(GetReactivity())
							.OfType<Reactive::DriverNode>()
							.Select([](auto dn) { return dn->GetID(); })
							.FirstOrDefault(Type::Nil);
					}

					if (clockSig.IsNil()) {
						bufferLen = Native::Constant::New((int32_t)2);
					} else {
						DriverSignature dsig(clockSig);
						auto rateSig = Type::User(&ReactiveRateTag, dsig.GetMetadata());
						auto rateKey = TLS::GetCurrentInstance()->Memoize(rateSig);
						auto rateSlot = sfx.GetSymbolTable().GetIndex(rateKey);

						sfx.GetSymbolTable()
							.RegisterExternalVariable(rateSig, Type::Float32, rateKey, GlobalVarType::Configuration,
													  { 1,1 }, Type::Nil);

						auto rate = Dereference::New(Configuration::New(Type::Float32, rateSlot), nullptr);
						InliningTransform inlineRate(lenConfigurator, rate);

						auto cfgProc = Native::MakeInt32(
							"max", Native::Max, Native::Constant::New((int32_t)2), 
							GetAccessor(
								DereferenceAll(
									sfx(inlineRate.Go()),
									nullptr)));
						bufferLen = DerivedConfiguration::New(cfgProc);
					}
					break; }
				}

				auto elSz = Native::Constant::New((int32_t)elementType.GetSize());
				auto bufferSz = NI32(Mul, bufferLen, elSz);
				auto indexPtr = Offset::New(bufferPtr, bufferSz);

				sfx.SetLocalStatePointer(Offset::New(indexPtr, Native::Constant::New((int64_t)Type::Int32.GetSize())));

				auto initIndex = Copy::New(indexPtr, Native::Constant::New(int32_t(0 - elementType.GetSize( ))),
					Native::Constant::New(int32_t(4)), Copy::Store,
					sfx.GetSymbolTable( ).GetInitializerReactivity( ), 1, true, true);

				indexPtr = Deps::New(indexPtr, initIndex);

				auto initRbufFirst = sfx.CopyData(
					DataSource::New(bufferPtr,Reference::New(Native::Constant::New(elementType, nullptr))), 
					sfx(GetUp(0)), 
					sfx.GetSymbolTable().GetInitializerReactivity(), true, true, true);

				auto initRbufRest =
					Copy::New(
						Offset::New(bufferPtr, elSz),
						Deps::New(bufferPtr, initRbufFirst),
						MakeConversionNode<int64_t,Native::ToInt64>(
							elSz, Type::Int32, 
							sfx.GetSymbolTable().GetInitializerReactivity()),
						Copy::MemCpy,
						sfx.GetSymbolTable().GetInitializerReactivity(),
						NI32(Sub, bufferLen, Native::Constant::New((int32_t)1)), true, true);

				auto index = Dereference::New(indexPtr, GetReactivity(), Type::Int32);
				auto newIdx = NI32(Add, index, elSz);
				newIdx->SetReactivity(GetReactivity());
				newIdx = Native::Select::New(newIdx, newIdx, 
											 NI32(Mul, 
												  bufferLen,
												  Native::Constant::New(int32_t(0 - elementType.GetSize()))));
				newIdx->SetReactivity(GetReactivity());

				auto updatedIndex(Deps::New(newIdx, 
							   Copy::New(indexPtr, newIdx, 
										 Native::Constant::New(int64_t(Type::Int32.GetSize())), 
										 Copy::Mode::Store, GetReactivity(), 1, true, false)));

				bufferPtr = Deps::New(bufferPtr, initRbufRest);
				auto bufferOffset = NI32(Add, updatedIndex, bufferSz);
				evictPtr = Offset::New(bufferPtr, bufferOffset);

				rover = NI32(Div, bufferOffset, elSz);					
				const_cast<Typed*>(rover)->SetReactivity(GetReactivity());
			}

			auto output(DataSource::New(evictPtr, Reference::New(Native::Constant::New(elementType, 0))));

			sfx.AddSideEffect(output, GetUp(1), statePtr, GetReactivity(), elementType.GetSize());

			CTRef bufferView;
			if (lenConfigurator) {
				bufferView =
					MakeSlice(sfx,
							  BitCast::New(Type::ArrayView(elementType), 1, bufferPtr),
							  Native::Constant::New((int32_t)0),
							  bufferLen, GetReactivity());
			} else {
				bufferView = DataSource::New(
					bufferPtr,
					Reference::New(
						Native::Constant::New(
							Type::List(elementType, len), 0)));
			}

			auto outputTuple =
				Pair::New(bufferView,
						  Pair::New(output, rover));

			return outputTuple;
		}

		/* identity transform that removes Deps */
		class CalleeArgumentMap : public PartialTransform<Transform::Identity<const Typed>> {
		public:
			CalleeArgumentMap(CTRef root):PartialTransform(root) { }
			CTRef operate(CTRef src) {
				if (IsOfExactType<Deps>(src)) return operate(src->GetUp(0));
				else return PartialTransform::operate(src);
			}
		};

		CTRef GetDereferencedParams(SideEffectTransform& sfx, Type t, CTRef datum, CRRef rx, std::vector<CTRef>& CallerParams, size_t& ParamID) {
			while (t.IsUserType()) t = t.UnwrapUserType();
			if (t.IsPair()) {
				return Pair::New(
					GetDereferencedParams(sfx, t.First(), datum->GraphFirst(), rx?rx->First():nullptr, CallerParams, ParamID),
					GetDereferencedParams(sfx, t.Rest(), datum->GraphRest(), rx?rx->Rest():nullptr, CallerParams, ParamID));
			} else {
				datum = DereferenceAll(datum, rx);
				auto paramData(DataSource::New(SubroutineArgument::In(ParamID++, rx, datum), sfx.GetDataLayout(datum)));
				CallerParams.push_back(sfx.GetDataAccessor(datum));
				return paramData;
			}
		}

		static CTRef SetupParameterLeaf(SideEffectTransform& sfx, CTRef datum, CRRef rx, size_t& ParamID, std::vector<CTRef>& callerParams, bool input, bool potentialTailCall) {
			if (IsPair(datum)) {
				return Pair::New(
					SetupParameterLeaf(sfx, datum->GraphFirst(), rx?rx->First():nullptr, ParamID, callerParams, input, potentialTailCall),
					SetupParameterLeaf(sfx, datum->GraphRest(), rx?rx->Rest():nullptr, ParamID, callerParams, input, potentialTailCall));
			} else if (MayHaveData(datum)) {
				DataSource *buf_ds;
				IFixedResultType *layout;
				if (potentialTailCall && Qxx::FromGraph(GetAccessor(datum)).OfType<Buffer>().Any()
					&& datum->Cast(buf_ds) && buf_ds->Dereference(rx)->GetDataLayout()->Cast(layout)
					&& layout->FixedResult().GetSize() <= 256) {
						return GetDereferencedParams(sfx, layout->FixedResult(), datum, rx, callerParams, ParamID);
				} else {
					callerParams.push_back(sfx.GetDataAccessor(datum));
					auto arg = SubroutineArgument::New(input, ParamID++, datum, rx);
					return DataSource::New(arg, sfx.GetDataLayout(datum));
				}
			} else {
				return Typed::Nil();
			}
		}

		static CTRef GetCallerAndCalleeParams(SideEffectTransform& sfx, CTRef pgraph, CRRef rx, size_t& ParamID, std::vector<CTRef>& callerParams, bool input, bool potentialTailCall) {
			if (IsPair(pgraph)) {
				auto f = GetCallerAndCalleeParams(sfx, pgraph->GraphFirst(), rx?rx->First():rx, ParamID, callerParams, input, potentialTailCall);
				auto r = GetCallerAndCalleeParams(sfx, pgraph->GraphRest(), rx?rx->Rest():rx, ParamID, callerParams, input, potentialTailCall);
				return Pair::New(f, r);
			} else return SetupParameterLeaf(sfx, sfx(pgraph), rx, ParamID, callerParams, input, potentialTailCall);
		}

		static bool IsLayoutSimilar(CTRef a, CTRef b) {
			Native::Constant *ac, *bc;
			//VariantTuple *avt, *bvt;
			Reference *ar, *br;
			Pair *ap, *bp;

			if (a->Cast(ap) || b->Cast(bp)) {
				if (ap == 0 && a->Cast(ac) && ac->FixedResult().IsPair() == false) return false;
				if (bp == 0 && b->Cast(bc) && bc->FixedResult().IsPair() == false) return false;
				return IsLayoutSimilar(a->GraphFirst(), b->GraphFirst()) && IsLayoutSimilar(a->GraphRest(), b->GraphRest());
			}

			if (a->Cast(ar)) {
				if (b->Cast(br)) {
					return IsLayoutSimilar(a->GetUp(0), b->GetUp(0));
				} else return false;
			} else if (b->Cast(br)) return false;

			return true;
		}

		static int GetAlignment(CTRef n) {
			IAlignmentTrackerNode *iat;
			if (n->Cast(iat)) {
				int a = iat->GetAlignment();
				return a;
			}
			return 4;
		}


		static bool IsArgumentCompatible(CTRef a, CTRef b) {
			const VariantTuple* avt, *bvt;
			const DataSource *ads, *bds;

			if (a->Cast(avt) && b->Cast(bvt)) {
				return IsArgumentCompatible(a->GetUp(0), b->GetUp(0)) &&
					IsArgumentCompatible(a->GetUp(2), b->GetUp(2));
			} else if (a->Cast(ads) && b->Cast(bds)) {
				return  GetAlignment(ads->GetAccessor()) == GetAlignment(bds->GetAccessor()) &&
						IsLayoutSimilar(ads->GetDataLayout(), bds->GetDataLayout());
			} else if (IsPair(a)) {
				if (IsPair(b)) {
					return IsArgumentCompatible(a->GraphFirst(), b->GraphFirst()) &&
						   IsArgumentCompatible(a->GraphRest(), b->GraphRest());
				} else return false;
			} else if (IsPair(b)) {
				return false;
			} else if (a->Cast(ads)) {
				return IsLayoutSimilar(ads->GetDataLayout(), b);
			} else if (b->Cast(bds)) {
				return IsLayoutSimilar(a, bds->GetDataLayout());
			}			
			return IsLayoutSimilar(a, b);
		}

		static CTRef _SubstituteTypeToArgumentGraph(CTRef Argument, CTRef Type, bool splitPairs = true);
		static CTRef SubstituteTypeToArgumentGraph(CTRef Argument, CTRef Type, bool splitPairs = true) {
			auto tmp = _SubstituteTypeToArgumentGraph(Argument, Type, splitPairs);
			return tmp;
		}

		static CTRef _SubstituteTypeToArgumentGraph(CTRef Argument, CTRef Type, bool splitPairs) {
			const DataSource *ds;
			if (Argument->Cast(ds)) {
				return DataSource::New(
					ds->GetAccessor(),
					_SubstituteTypeToArgumentGraph(ds->GetDataLayout(), Type, false));
			}

			if (IsPair(Argument)) {
				if (splitPairs) {
					return Pair::New(
						_SubstituteTypeToArgumentGraph(Argument->GraphFirst(), Type->GraphFirst()),
						_SubstituteTypeToArgumentGraph(Argument->GraphRest(), Type->GraphRest()));
				} else {
					return Type;
				}
			}


			if (Argument->Cast<Native::Constant>() || Argument->Cast<VariantTuple>()) {
				Native::Constant *c;
				if (Argument->Cast(c)) {
					if (c->FixedResult().IsPair() == false) return c;
				}
				return Type;
			}

			const Reference* ref;
			if (Argument->Cast(ref)) {
				return Reference::New(_SubstituteTypeToArgumentGraph(ref->GetUp(0), Type, splitPairs));
			}

			return Argument;

			INTERNAL_ERROR("Unexpected node in argument graph");
		}

		static CTRef ReplaceBufferLayoutType(CTRef layout, const Type& t) {
			Pair *p;
			Reference *ref;
			Dereference *deref;
			Native::Constant *c;
			if (layout->Cast(ref)) {
				return Reference::New(ReplaceBufferLayoutType(ref->GetUp(0), t));
			}
			if (layout->Cast(deref)) {
				return Dereference::New(ReplaceBufferLayoutType(ref->GetUp(0), t), nullptr);
			}
			if (layout->Cast(p)) {
				return Pair::New(
					ReplaceBufferLayoutType(p->GetUp(0), t.First()),
					ReplaceBufferLayoutType(p->GetUp(1), t.Rest()));
			}
			if (layout->Cast(c)) {
				return Native::Constant::New(t, nullptr);
			}
			INTERNAL_ERROR("Unknown datalayout");
		}

		CTRef UnsafePointerCast::SideEffects(SideEffectTransform& sfx) const {
			auto up = sfx(GetUp(0));
			const DataSource* ds;
			if (up->Cast(ds)) {
				assert(ds->IsReference());
				return DataSource::New(ds->GetAccessor(), Reference::New(Native::Constant::New(t, nullptr)));
			}
			return up;
		}

		CTRef Switch::SideEffects(SideEffectTransform& sfx) const {
			// use the same buffer for all the switch branches
			BufferBuilder Allocator(Native::Constant::New(FixedResult(), 0), GetReactivity(), sfx);
			auto result(Allocator(sfx.GetElision()[this]));
			CTRef branchResult(result);

			bool resultIsTaggedUnion(FixedResult().IsUserType(UnionTag));

			// if result type is union, omit the tag (subtype index) from branch elision
			if (resultIsTaggedUnion) branchResult = branchResult->GraphFirst();

			auto md = MultiDispatch::New(FixedResult(), GetAccessor(DereferenceAll(sfx(GetUp(0)), GetReactivity())),
				resultIsTaggedUnion ? GetAccessor(result->GraphRest()) : Typed::Nil(), sfx.GetStatePointer());
			
			for (unsigned i(1);i < GetNumCons();++i) {
				const DataSource *ds = branchResult->Cast<DataSource>();
				IFixedResultType *fix = GetUp(i)->Cast<IFixedResultType>();
				if (fix->FixedResult().GetSize() == 0) {
					md->dispatchees.emplace_back(nullptr, branchResultSubtypeIndex[i]);
				} else {
					sfx.GetElision()[GetUp(i)] = DataSource::New(ds->GetAccessor(), ReplaceBufferLayoutType(ds->GetDataLayout(), fix->FixedResult()));
					auto subr = sfx(GetUp(i))->GetUp(0)->GetUp(1)->Cast<Subroutine>();
					assert(subr);
					assert(sfx.GetStatePointer() == subr);
					md->dispatchees.emplace_back(subr, branchResultSubtypeIndex[i]);
				}
			}
			sfx.SetStatePointer(md);
			return Deps::New(result, md);
		}


		CTRef FunctionCall::SideEffects(SideEffectTransform& sfx) const {
			BufferBuilder Allocator(Native::Constant::New(resultType, 0), GetReactivity(), sfx);
			auto result(Allocator(sfx.GetElision()[this]));

			size_t ParamID(1);
			//CalleeArgumentMap CalleeParams(GetUp(0)), CalleeResults(result);

			std::vector<CTRef> CallerParams, CallerResults;
			CTRef ArgumentGraph = GetCallerAndCalleeParams(sfx, GetUp(0), argumentReactivity, ParamID, CallerParams, true, false);

			if (ParamID > 8) {
				CallerParams.clear(); ParamID = 1;
				ArgumentGraph = GetCallerAndCalleeParams(sfx, 
					Canonicalize(sfx(GetUp(0)), argumentReactivity, ArgumentType(), true, false, sfx), 
					argumentReactivity,
					ParamID, CallerParams, true, false);
			}

			CTRef ResultGraph = GetCallerAndCalleeParams(sfx, result, nullptr, ParamID, CallerResults, false, false);

			auto subroutine(Subroutine::New(GetLabel(), Typed::Nil()));

			sfx.CompileSubroutineAsync(label.c_str(), ArgumentType(), FixedResult(), subroutine, body,
				SubstituteTypeToArgumentGraph(ArgumentGraph, Native::Constant::New(argumentType, 0)),
				SubstituteTypeToArgumentGraph(ResultGraph, Native::Constant::New(FixedResult(), 0)));

			/* connect the function to the state allocation chain */
			subroutine->Connect(sfx.GetStatePointer());
			auto subrAlloc = SubroutineStateAllocation::New(subroutine, sfx.GetStatePointer());
			sfx.SetStatePointer(subrAlloc);

			for (auto arg : CallerParams) subroutine->Connect(arg);
			subroutine->ArgumentsConnected();
			for (auto res : CallerResults) subroutine->Connect(res);

			/* return function result buffers with dependency on the subroutine call */
			return Deps::New(result, subroutine);
		}

		RecursionBranch::RecursionBranch(CTRef counter, CTRef up, int32_t* seq, Graph<Typed> t, Graph<Generic> a, Graph<Generic> r, CTRef calleeArgs, CTRef calleeRes, const FunctionSequence* parent)
			:sequenceLength(seq), tailContinuation(t), closedArgument(a), closedResult(r), DisposableTypedBinary(counter, up),
			parentSeq(parent), SequenceArgument(calleeArgs), SequenceResult(calleeRes), argumentReactivity(parent->argumentReactivity) { }


		static CTRef  _UsePrepadAllocation(SideEffectTransform& sfx, CTRef passedArgs, unordered_set<const DataSource*>& stopAlias, int& outPrepadAvailable, Type& outSlotType) {
			const DataSource *ds;
			if (passedArgs->Cast(ds) && ds->IsReference() && stopAlias.find(ds) == stopAlias.end()) {
				/* use each prepad at most once */
				stopAlias.insert(ds);

				VariantTuple *vt;
				if (ds->Dereference(nullptr)->GetDataLayout()->Cast(vt) && vt->GetRecurrenceDelta() > 0) {
					ResultTypeWithNoArgument rtnoa(nullptr);
					outSlotType = vt->GetUp(0)->Result(rtnoa);
					outPrepadAvailable = vt->GetRecurrenceDelta();
					return ds;
				}
				return nullptr;
			}

			const Pair *p;
			if (passedArgs->Cast(p)) {
				const DataSource *prepad;
				CTRef ast = _UsePrepadAllocation(sfx, p->GetUp(1), stopAlias, outPrepadAvailable, outSlotType);
				if (ast && ast->Cast(prepad) && outPrepadAvailable > 0 && prepad->IsReference()) {
					DataSource *slot = DataSource::New(
						Offset::New(prepad->GetAccessor(), Native::Constant::New(-int64_t(outSlotType.GetSize()))),
						Reference::New(Native::Constant::New(outSlotType, 0)));

					outPrepadAvailable--;

					return Deps::New(
						DataSource::New(slot->GetAccessor(), prepad->GetDataLayout()),
						sfx.CopyData(slot, sfx(p->GetUp(0)), 0, true,false,false));
				} else {
					outPrepadAvailable = 0;
					CTRef fst = _UsePrepadAllocation(sfx, p->GetUp(0), stopAlias, outPrepadAvailable, outSlotType);
					if (fst || ast) {
						return Pair::New(fst ? fst : p->GetUp(0), ast ? ast : p->GetUp(1));
					} else return nullptr;
				}
			}
			return nullptr;
		}

		static CTRef UsePrepadAllocation(SideEffectTransform& sfx, CTRef passedArgs) {
			unordered_set<const DataSource*> preventAlias;
			int tmp; Type tmp2;
			CTRef result = _UsePrepadAllocation(sfx, passedArgs, preventAlias, tmp, tmp2);
			return result ? result : passedArgs;
		}

		CTRef RecursionBranch::SideEffects(SideEffectTransform& sfx) const {
			auto Counter(sfx(GetUp(0)));
			auto Elision(sfx.GetElision()[this]);
			size_t ParamID(1);

			CTRef args = UsePrepadAllocation(sfx, GetUp(1));

			std::vector<CTRef> CallerParams, CallerResults;
			CTRef ArgumentGraph = GetCallerAndCalleeParams(sfx, args, argumentReactivity, ParamID, CallerParams, true, false);

			CTRef ResultGraph = GetCallerAndCalleeParams(sfx, Elision, nullptr, ParamID, CallerResults, false, false);

			//std::cout << "[" << GetLabel() << "]\n";
			//std::cout << "Sequence        Argument: " << *SequenceArgument << std::endl;
			//std::cout << "RecursionBranch Argument: " << *ArgumentGraph << std::endl;
			//std::cout << "Sequence        Result  : " << *SequenceResult << std::endl;
			//std::cout << "RecursionBranch Result  : " << *ResultGraph << std::endl;
			//std::cout << "arg " << *argumentReactivity << "\n" << *GetReactivity() << "\n";

			/* add counter argument */
            SubroutineArgument::In(ParamID++, nullptr, Type::Int32, "count");

			bool compatibleArgument(IsArgumentCompatible(ArgumentGraph, SequenceArgument));
			bool compatibleResult(IsArgumentCompatible(ResultGraph, SequenceResult));

			if (compatibleArgument && compatibleResult) {
				/* compile and emit tail branch */
				Type tailArgType(SpecializationTransform::Infer(parentSeq->GetArgumentFormula( ), Type::InvariantI64(*sequenceLength)));
				Type tailResType(SpecializationTransform::Infer(parentSeq->GetResultFormula(), Type::InvariantI64(0)));

				Subroutine *recur = Subroutine::NewRecursive("recur", Typed::Nil(), *sequenceLength);

				sfx.CompileSubroutineAsync(
					nullptr, Type::Nil, Type::Nil,
					recur, parentSeq->GetTailCall(),
					SubstituteTypeToArgumentGraph(ArgumentGraph, Native::Constant::New(tailArgType, 0)),
					SubstituteTypeToArgumentGraph(ResultGraph, Native::Constant::New(tailResType, 0)));

//				recur->Connect(sfx.GetStatePointer());
//				sfx.SetStatePointer(recur);

				recur->Connect(Typed::Nil()); // placeholder for state pointer
				sfx.SetRecursiveBranch(recur); // remember recursion point for state patching

				for (auto arg : CallerParams) recur->Connect(arg);
				recur->ArgumentsConnected();
				for (auto res : CallerResults) recur->Connect(res);
				recur->Connect(sfx.GetDataAccessor(Counter));

				return Deps::New(Elision, recur);
			} else {
				/* argument or result data map has changed; peel iteration out of loop */
				if (parentSeq->GetRepeatCount() > 1) {
					FunctionBase *shorten; 
					shorten = parentSeq->RemoveIterationsFront(1);

					shorten->Reconnect(0, GetUp(1));

					//cout << "Shortened\n"<<*shorten<<"\n";
					*sequenceLength = 1;
					shorten->SetArgumentReactivity(argumentReactivity);
					sfx.GetElision()[shorten] = Elision;
					return shorten->SideEffects(sfx);
				} else {
					/* compile and emit tail branch */
					Type tailArgType, tailResType; {
						tailArgType = (SpecializationTransform::Infer(parentSeq->GetArgumentFormula(), Type::InvariantI64(*sequenceLength)));
						tailResType = (SpecializationTransform::Infer(parentSeq->GetResultFormula(), Type::InvariantI64(0)));
					}

					Subroutine *recur = Subroutine::New("recur-end", Typed::Nil());

					BufferBuilder Allocator(Native::Constant::New(tailResType, 0), GetReactivity(), sfx);
					auto result = Allocator(Elision);

					CallerParams.clear(); CallerResults.clear();
					ParamID = 1;

					ArgumentGraph = GetCallerAndCalleeParams(sfx, args, argumentReactivity, ParamID, CallerParams, true, false);
					ResultGraph = GetCallerAndCalleeParams(sfx, result, nullptr, ParamID, CallerResults, false, false);

					sfx.CompileSubroutineAsync(
						nullptr, Type::Nil, Type::Nil,
						recur, parentSeq->GetTailCall(),
						SubstituteTypeToArgumentGraph(ArgumentGraph, Native::Constant::New(tailArgType, 0)),
						SubstituteTypeToArgumentGraph(ResultGraph, Native::Constant::New(tailResType, 0)));

					recur->Connect(sfx.GetStatePointer());
					auto recurAlloc = SubroutineStateAllocation::New(recur, sfx.GetStatePointer());
					sfx.SetStatePointer(recurAlloc);

					for (auto arg : CallerParams) recur->Connect(arg);
					recur->ArgumentsConnected();
					for (auto res : CallerResults) recur->Connect(res);
					return Deps::New(result, recur);
				}
			}
			KRONOS_UNREACHABLE;
		}

		static CTRef _PrepadGrowingVectorArguments(SideEffectTransform& sfx, CTRef passedArgs, CTRef argumentLayout, CTRef seqLen) {
			Pair* lp;
			if (argumentLayout->Cast(lp)) {
				CTRef pa = passedArgs->GraphFirst();
				CTRef pb = passedArgs->GraphRest();
				CTRef a = _PrepadGrowingVectorArguments(sfx, pa, lp->GetUp(0), seqLen);
				CTRef b = _PrepadGrowingVectorArguments(sfx, pb, lp->GetUp(1), seqLen);
				if (a || b) {
					return Pair::New(a ? a : pa, b ? b : pb);
				} else return 0;
			}

			VariantTuple *vt;
			if (argumentLayout->Cast(vt) && vt->GetRecurrenceDelta() > 0) {

				CTRef passedArgsType = ReduceType(passedArgs);
				CTRef elementType = ReduceType(vt->GetUp(0));

				ResultTypeWithNoArgument rtnoa(nullptr);
				CTRef padding = NI64(Mul,
					Native::Constant::New((int64_t)(elementType->Result(rtnoa).GetSize() * vt->GetRecurrenceDelta())),
					seqLen);

				CTRef bufSize = NI64(Add, 
					padding, ComputeSize(passedArgsType));
				CTRef paddedBuffer = Buffer::New(sfx, bufSize, Buffer::Stack, 16);
				CTRef iteratorPtr = Offset::New(paddedBuffer, padding);

				DataSource *iteratorDs = DataSource::New(iteratorPtr, Reference::New(passedArgsType));

				return Deps::New(iteratorDs, sfx.CopyData(iteratorDs, sfx(passedArgs), 0, true, false, false));
			} else return 0;
		}

		static CTRef PrepadGrowingVectorArguments(SideEffectTransform& sfx, CTRef passedArgs, CTRef argumentLayout, std::int32_t seqLen) {
			CTRef result = _PrepadGrowingVectorArguments(sfx, passedArgs, argumentLayout, Native::Constant::New((std::int64_t)seqLen));
			return result ? result : passedArgs;
		}

		CTRef FunctionSequence::SideEffects(SideEffectTransform& sfx) const {
			Type resultType{ FixedResult() };
			BufferBuilder Allocator(Native::Constant::New(resultType, 0), GetReactivity(), sfx);
			auto result(Allocator(sfx.GetElision()[this]));

			assert(num < std::numeric_limits<int32_t>::max());
			std::int32_t seqLenVal((std::int32_t)num);
			std::int32_t *seqLenPtr(&seqLenVal);

			CTRef args = PrepadGrowingVectorArguments(sfx, GetUp(0), typedArgument, seqLenVal);

			size_t ParamID(1);

			std::vector<CTRef> CallerParams, CallerResults;
			CTRef ArgumentGraph = GetCallerAndCalleeParams(sfx, args, argumentReactivity, ParamID, CallerParams, true, false);
			if (ParamID > 8) {
					CallerParams.clear(); ParamID = 1;
					ArgumentGraph = GetCallerAndCalleeParams(sfx,
						Canonicalize(sfx(GetUp(0)), GetReactivity(), ArgumentType(), true, false, sfx), 
															 argumentReactivity, ParamID, CallerParams, true, false);
			}
			CTRef ResultGraph = GetCallerAndCalleeParams(sfx, result, nullptr, ParamID, CallerResults, false, false);

			auto CounterArgument(SubroutineArgument::In(ParamID++, nullptr, Type::Int32, "count"));

			RecursionBranch *branch = RecursionBranch::New(
				DataSource::New(CounterArgument, Native::Constant::New(0)),
				this->iterator,
				seqLenPtr, tailContinuation, closedArgument, closedResult,
				ArgumentGraph, ResultGraph,
				this);

			branch->SetReactivity(GetReactivity( ));
            
			LambdaTransform<const Typed, CTRef> builder(generator);
			builder.SetTransform([&](CTRef node) {
				if (IsOfExactType<Argument>(node)) {
					return SyntheticPair(node, branch);
				} else return node->IdentityTransform(builder);
			});

			builder.QueuePostProcessing([this](CTRef root) {
				auto newRoot = root->ConstructShallowCopy();
				newRoot->SetReactivity(GetReactivity());
				return newRoot;
			});

			/* should run subgraph sharing */
			CTRef body(builder.Go());

			auto subroutine(Subroutine::New(GetLabel(), Typed::Nil()));
			sfx.CompileSubroutineAsync(
				label, ArgumentType(), FixedResult(),
				subroutine, body,
				SubstituteTypeToArgumentGraph(ArgumentGraph, typedArgument),
				SubstituteTypeToArgumentGraph(ResultGraph, typedResult), true);

			subroutine->Connect(sfx.GetStatePointer());
			auto subrAlloc = SubroutineStateAllocation::New(subroutine, sfx.GetStatePointer());
			sfx.SetStatePointer(subrAlloc);

			for (auto arg : CallerParams) subroutine->Connect(arg);
			subroutine->ArgumentsConnected();
			for (auto res : CallerResults) subroutine->Connect(res);
			subroutine->Connect(Native::Constant::New(std::int32_t(0))); // recursion counter

			/* return function result buffers with dependency on the subroutine call */
			return Deps::New(result, subroutine);
		}
	};
#pragma endregion 
	CTRef Canonicalize(CTRef data, CRRef r, const Type& e, bool referenced, bool mustCopy, SideEffectTransform& sfx) {
		if (mustCopy == false) {
			const DataSource* ds;
			data = DereferenceAll(data, r);
			if (data->Cast(ds) && ds->HasPairLayout() == false && IsFused(r)) {
				if (referenced) {
					if (ds->CanTakeReference()) return ds->Reference();
				} else return ds;
			}
		}
		auto buf(Buffer::New(sfx, e.GetSize(), Buffer::Stack));
		auto buf_ds(DataSource::New(buf, Reference::New(Native::Constant::New(e, 0))));
		auto val = Deps::New(buf_ds, sfx.CopyData(buf_ds, data, r, true, false, false));
		if (!referenced) return DereferenceOnce(val, r);
		else return val;
	}
};
