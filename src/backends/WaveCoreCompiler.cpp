#include <iostream>
#include <list>
#include <sstream>
#include <regex>
#include <fstream>

#include "ModuleBuilder.h"
#include "UserErrors.h"
#include "WaveCoreModule.h"
#include "TargetLowering.h"
#include "Evaluate.h"
#include "Native.h"
#include "TypeAlgebra.h"
#include "CompilerNodes.h"
#include "DriverSignature.h"
#include "SysCaps.h"

static std::string CSanitize(std::string symbol) {
	for (auto &c : symbol) {
		if (!isalnum(c)) c = '_';
	}

	for (size_t i(1); i<symbol.size(); ++i) if (symbol[i - 1] == '_') symbol[i] = toupper(symbol[i]);

	std::regex removeUnderScores("([a-zA-Z]*)_+([a-zA-Z]*)");

	symbol = std::regex_replace(symbol, removeUnderScores, "$1$2");
	symbol = std::regex_replace(symbol, std::regex("_+"), "_");

	if (!isalpha(symbol[0])) return "_" + symbol;
	else return symbol;
}


namespace K3 {
	namespace Backends {
		class WaveCoreTransform;
		using namespace Kronos;
		//bool WaveCore::GetExternalInput(const IType& key, IType& dataType, IType& clockSignature, int& clockPriority, int& dataRateMultiplier, int& dataRateDivider) noexcept {
		//	if (key == Kronos::GetString("audio")) {
		//		dataType = Kronos::GetTuple(Kronos::GetFloat32Ty(), 2);
		//		clockSignature = key;
		//		return true;
		//	}
		//	else return false;
		//}
	}

	namespace Nodes {
		namespace WaveCore {
			class IWCNode : REFLECTING_CLASS {
				INHERIT(IWCNode, Reflecting)
			public:
				virtual Backends::WCInstr* AppendToNetList(Backends::WaveCoreTransform&) const = 0;
			};
		}
	}

	namespace Backends {

		std::string GetName(WCInstr* i) {
			if (i == nullptr) return "void";
			if (i->Type() == VOLATILE_CONSTANT) return (char *)i->Op1();
			return "v" + std::to_string(i->OutID());
		}

		std::ostream& operator<<(std::ostream& stream, const WCInstr& wci) {
			PAType t;
			int id, dly;
			float p;
			WCInstr *up1, *up2, *tau;
			std::tie(t, id, up1, up2, p, tau, dly) = wci.GetTuple( );
			static const char *mnemonic[] = {
				"A",
				">",
				"/",
				"K",
				"L",
				"*",
				"Q",
				"M",
				"R",
				"C",
				"C"
			};

			if (t != VOLATILE_CONSTANT) {
				stream << mnemonic[t] << " " << GetName(up1) << " " << GetName(up2) << " v" << id << " " << p << " " << dly << "\n";
			} 

			return stream;
		}


		class WaveCoreTransform : public CachedTransform < const Typed, WCInstr* > {
			WCNetList& nl;
			Type forDriver;
		public:
			WCNetList& GetNetList( ) { return nl; }
			WaveCoreTransform(CTRef root, Type Driver, WCNetList& nl) :CachedTransform(root), nl(nl),forDriver(Driver) { }

			WCInstr* operate(CTRef node) {
				RingBuffer* rb;
				Native::ITypedBinary *itb;
				Native::Constant *nc;				
				Nodes::WaveCore::IWCNode *wcn;
				Nodes::Boundary *boundary;
				Nodes::Monad *monad;
				Nodes::GetGlobalVariable *gv;

				if (node->Cast(itb)) {
					PAType pa = UNDEF;
					float param = 0;
					switch (itb->GetOpcode( )) {
					case Native::Add: pa = ADD; break;
					case Native::Mul: pa = MUL; break;
					case Native::Div: pa = DIV; break;
					case Native::Sub: pa = MAD; param = -1; break;
					case Native::Greater_Equal: pa = CMP; break;
					default: break;
					}
					return nl.append(pa,
						(*this)(itb->GetUp(0)),
						(*this)(itb->GetUp(1)),
						param);
				} else if (node->Cast(wcn)) {
					return wcn->AppendToNetList(*this);
				} else if (node->Cast(nc)) {
					float val = 0;
					if (nc->FixedResult( ).IsFloat32( )) {
						val = *(const float*)nc->GetPointer( );
					} else if (nc->FixedResult( ).IsFloat64( )) {
						val = (float)*(const double*)nc->GetPointer( );
					} else if (nc->FixedResult( ).IsInt32( )) {
						val = (float)*(const int32_t*)nc->GetPointer( );
					} else if (nc->FixedResult( ).IsInt64( )) {
						val = (float)*(const int64_t*)nc->GetPointer( );
					}
					return nl.append(CONSTANT, 0, 0, val, 0, 0);
					throw Error::RuntimeError(Error::BadInput, "Composite constant in WaveCore backend");
				} else if (node->Cast(boundary) || node->Cast(monad)) {
					return (*this)(node->GetUp(0));
				} else if (node->Cast(rb)) {
					auto instr = nl.append(AMP, nullptr, nullptr, 1.f, nullptr, (int)rb->GetLen());
					QueuePostProcessing([=](WCInstr* i) {
						*instr = WCInstr(AMP, instr->OutID(), (*this)(rb->GetUp(1)), nullptr, 1.f, nullptr, (int)rb->GetLen());
						return i;
					});
					return instr;
				} else if (node->Cast(gv)) {
					if (gv->FixedResult().IsFloat32()) {
						std::stringstream ss;
						ss << gv->GetKey();
						auto sym = "." + CSanitize(ss.str());
						nl.extVars.insert(sym);
						return nl.append(VOLATILE_CONSTANT, (WCInstr*)TLS::GetCurrentInstance()->Memoize(sym.c_str()), nullptr);
					} else throw Error::RuntimeError(Error::BadDefinition, "WaveCore backend only supports external parameters as float scalars");
				} else {
					throw Error::RuntimeError(Error::BadDefinition,"The WaveCore backend does not support the required node: " + std::string(node->GetLabel()));
				}
			}
		};

		std::ostream& operator<<(std::ostream& s, const WCNetList& wcnl) {
			for (auto wci : wcnl) s << wci;
			return s;
		}
	}

	namespace Nodes {
		namespace WaveCore {
			TYPED_NODE(Amp, TypedUnary, IWCNode)
				float coef;
				int delay;
				Amp(CTRef up, float c, int d) :coef(c), delay(d), TypedUnary(up) { }
			public:
				static CTRef New(CTRef up, float c, int dly) { 
					Native::Constant *cn;
					if (dly == 0 && up->Cast(cn)) {
						return Native::Constant::New(*(float*)cn->GetPointer() * c);
					}
					return new Amp(up, c, dly); 
				}
				Backends::WCInstr* AppendToNetList(Backends::WaveCoreTransform& xfm) const {
					auto instr = xfm.GetNetList( ).append(Backends::AMP, nullptr,
						nullptr, coef, 0, delay);
					CTRef sig = GetUp(0);
					xfm.QueuePostProcessing([instr, sig, &xfm](Backends::WCInstr* i) {
						instr->Op1( ) = xfm(sig);
						return i;
					});
					return instr;
				}
				Type Result(ResultTypeTransform& t) const { return GetUp(0)->Result(t); }
				CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const {
					return New(transform(GetUp(0)), coef, delay);
				}
			END

			TYPED_NODE(Mad, TypedBinary, IWCNode)
				float coef;
				Mad(float coef, CTRef a, CTRef b) :coef(coef), TypedBinary(a, b) { }
			DEFAULT_LOCAL_COMPARE(TypedBinary, coef)
				PUBLIC
				static CTRef New(CTRef accumulator, CTRef addend, float coef) { 
					Native::Constant *a, *b;
					if (accumulator->Cast(a) && addend->Cast(b)) {
						return Native::Constant::New(*(float*)a->GetPointer() + *(float*)b->GetPointer() * coef);
					}
					return new Mad(coef, accumulator, addend); 
				}
				Backends::WCInstr* AppendToNetList(Backends::WaveCoreTransform& xfm) const {
					return xfm.GetNetList( ).append(Backends::MAD, xfm(GetUp(1)),
						xfm(GetUp(0)), coef, 0, 0);
				}
				Type Result(ResultTypeTransform& t) const { return GetUp(0)->Result(t); }
				CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const {
					return New(transform(GetUp(0)), transform(GetUp(1)), coef);
				}
				void Output(std::ostream& strm) const {
					strm << "( ";
					GetUp(0)->Output(strm);
					strm << " * " << coef << " + ";
					GetUp(1)->Output(strm);
					strm << " )";
				}
			END

			TYPED_NODE(ExtInput, DisposableTypedLeaf, IWCNode)
				std::string inName;
				DEFAULT_LOCAL_COMPARE(DisposableTypedLeaf, inName);
				ExtInput(const char *n) :inName(n) {}
			PUBLIC
				static ExtInput* New(const char *name) { return new ExtInput(name); }
				Backends::WCInstr* AppendToNetList(Backends::WaveCoreTransform& xfm) const {
					xfm.GetNetList().extVars.insert(inName);
					return xfm.GetNetList().append(Backends::VOLATILE_CONSTANT, (Backends::WCInstr*)inName.c_str(), nullptr);
				}
				Type Result(ResultTypeTransform& t) const { return Type::Float32; }
				void Output(std::ostream& strm) const {
					strm << inName;
				}
			END

			TYPED_NODE(Unsupported, TypedLeaf, IWCNode)
				const char *msg;
				DEFAULT_LOCAL_COMPARE(TypedLeaf, msg);
				Unsupported(const char *m) : msg(m) { }
			PUBLIC
				static Unsupported* New(const char *msg) { return new Unsupported(msg); }
				Type Result(ResultTypeTransform&) const { throw Error::Internal(msg); }
				Backends::WCInstr* AppendToNetList(Backends::WaveCoreTransform&) const {
					throw Error::Internal(msg);
				}
			END

			Interpreter::Var Amp::Interpret(InterpretTransform&) const { throw; }
			Interpreter::Var Mad::Interpret(InterpretTransform&) const { throw; }
			Interpreter::Var Unsupported::Interpret(InterpretTransform&) const { throw; }
			Interpreter::Var ExtInput::Interpret(InterpretTransform&) const { throw; }
		}
	}

	namespace Backends {
		static void InsertWaveCoreLoweringPatterns(Transform::LoweringPatterns& pat) {
			WildCard *a = WildCard::New( );
			WildCard *b = WildCard::New( );
			WildCard *c = WildCard::New( );

			using Transform::Lowering;

			/* subgraph constructors */
			auto ADD = [](CTRef a, CTRef b) { return Native::MakeFloat("add", Native::Add, a, b); };
			auto MAD = [](CTRef a, CTRef b, float coef) { return Nodes::WaveCore::Mad::New(a, b, coef); };
			auto SUB = [=](CTRef a, CTRef b) { return MAD(a, b, -1.f); };
			auto MUL = [](CTRef a, CTRef b) { return Native::MakeFloat("mul", Native::Mul, a, b); };
			auto DIV = [](CTRef a, CTRef b) { return Native::MakeFloat("div", Native::Div, a, b); };
			auto AMP = [](CTRef a, float coef, int dly) { return Nodes::WaveCore::Amp::New(a, coef, dly); };
			auto WCNOT = [=](CTRef a) { return SUB(Native::Constant::New(1.f), a); };
			auto WCAND = MUL;
			auto WCOR = [=](CTRef a, CTRef b) { return WCNOT(WCAND(WCNOT(a), WCNOT(b))); };
			auto GREATEREQ = [](CTRef a, CTRef b) { return Native::MakeFloat("gt", Native::Greater_Equal, a, b); };
			auto LESSEQ = [=](CTRef a, CTRef b) { return GREATEREQ(b, a); };
			auto LESS = [=](CTRef a, CTRef b) { return WCNOT(GREATEREQ(a, b)); };
			auto GREATER = [=](CTRef a, CTRef b) { return WCNOT(LESSEQ(a, b)); };
			auto EQUAL = [=](CTRef a, CTRef b) { return WCAND(GREATEREQ(a, b), LESSEQ(a, b)); };


			// remap audio input
//			Type data, clock; int mul, div, priority;
//			std::tie(data, clock, priority, mul, div) = TLS::GetCurrentInstance()->GetExternalStreamParameters(Type("audio"));
			//CTRef audioIn = GetGlobalVariable::New(
			//	TLS::GetCurrentInstance()->Memoize(Type("audio")),
			//	data,
			//	Type("audio"),
			//	std::make_pair(mul, div),
			//	nullptr,
			//	Stream,
			//	clock);

			//pat.AddRule(audioIn, [=](Matches& m, Lowering& l) -> CTRef {
			//	return Pair::New(
			//		Nodes::WaveCore::ExtInput::New(".AudioInL"),
			//		Nodes::WaveCore::ExtInput::New(".AudioInR"));
			//});

			// provide stereo audio input
			pat.AddRule(First::New(a),
				[=](Matches& m, Lowering& l) -> CTRef {
				if (auto gv = m[a]->Cast<GetGlobalVariable>()) {
					if (gv->GetKey() == Type::InvariantString(abstract_string::cons("audio"))) {
						return 
							Nodes::WaveCore::Mad::New(Nodes::Native::Constant::New(-1.5f),
												      Nodes::WaveCore::ExtInput::New(".AudioInL"), 1);
					}
				}
				return nullptr;
			});

			pat.AddRule(Rest::New(a),
						[=](Matches& m, Lowering& l) -> CTRef {
				if (auto gv = m[a]->Cast<GetGlobalVariable>()) {
					if (gv->GetKey() == Type::InvariantString(abstract_string::cons("audio"))) {
						return
							Nodes::WaveCore::Mad::New(Nodes::Native::Constant::New(-1.5f),
													  Nodes::WaveCore::ExtInput::New(".AudioInR"), 1);
					}
				}
				return nullptr;
			});

			// remap ring buffer
			pat.AddRule(First::New(Rest::New(a)),
				[=](Matches& m, Lowering& l) -> CTRef {
				RingBuffer* rb;
				if (m[a]->Cast(rb)) {
					auto dly = rb->PubConstructShallowCopy();
					l.QueuePostProcessing([dly, &l](CTRef root) {
						dly->Reconnect(1, l(dly->GetUp(1)));
						return root;
					});
					return dly;
				} else return nullptr;
			});

			/* mul-add with constant to MAD */
			pat.AddRule(Native::MakeFloat("add", Native::Add, a, Native::MakeFloat("mul", Native::Mul, b, c)),
				[=](Matches& m, Lowering& l) -> CTRef {
				Native::Constant *cn;
				auto a1 = l(m[a]), a2 = l(m[b]), a3 = l(m[c]);
				if (a2->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1,a3,*(float*)cn->GetPointer());
				if (a3->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1,a2,*(float*)cn->GetPointer());
				return ADD(a1, MUL(a2, a3));
			});

			/* mul-sub with constant to MAD */
			pat.AddRule(Native::MakeFloat("sub", Native::Sub, a, Native::MakeFloat("mul", Native::Mul, b, c)),
						[=](Matches& m, Lowering& l) -> CTRef {
				Native::Constant *cn;
				auto a1 = l(m[a]), a2 = l(m[b]), a3 = l(m[c]);
				if (a2->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1, a3, -*(float*)cn->GetPointer());
				if (a3->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1, a2, -*(float*)cn->GetPointer());
				return ADD(a1, MUL(a2, a3));
			});

			pat.AddRule(Native::MakeFloat("add", Native::Add, Native::MakeFloat("mul", Native::Mul, b, c), a),
				[=](Matches& m, Lowering& l) -> CTRef {
				Native::Constant *cn;
				auto a1 = l(m[a]), a2 = l(m[b]), a3 = l(m[c]);
				if (a2->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1, a3, *(float*)cn->GetPointer());
				if (a3->Cast(cn) && cn->FixedResult().IsFloat32()) return MAD(a1, a2, *(float*)cn->GetPointer());
				return ADD(a1, MUL(a2, a3));
			});

			/* mul with constant to AMP */
			pat.AddRule(Native::MakeFloat("mul", Native::Mul, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				Native::Constant *c;
				auto a1 = l(m[a]), a2 = l(m[b]);
				if (a1->Cast(c) && c->FixedResult().IsFloat32()) return AMP(a2, *(float*)c->GetPointer(), 0);
				if (a2->Cast(c) && c->FixedResult().IsFloat32()) return AMP(a1, *(float*)c->GetPointer(), 0);
				return MUL(a1, a2);
			});

			/* sub to MAD */
			pat.AddRule(Native::MakeFloat("sub", Native::Sub, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				return SUB(l(m[a]), l(m[b]));
			});

			/* Comparison lowering */
			pat.AddRule(Native::MakeFloat("eq", Native::Equal, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				return EQUAL(l(m[a]), l(m[b]));
			});

			pat.AddRule(Native::MakeFloat("ne", Native::Not_Equal, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				return WCNOT(EQUAL(l(m[a]), l(m[b])));
			});

			pat.AddRule(Native::MakeFloat("lt", Native::Less, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				return LESS(l(m[a]), l(m[b]));
			});

			pat.AddRule(Native::MakeFloat("le", Native::Less_Equal, a, b),
				[=](Matches& m, Lowering& l) -> CTRef {
				return LESSEQ(l(m[a]), l(m[b]));
			});

			pat.AddRule(Native::MakeFloat("gt", Native::Greater, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				return GREATER(l(m[a]), l(m[b]));
			});

			/* MAX and MIN via compare and blend */
			pat.AddRule(Native::MakeFloat("max", Native::Max, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				auto _b = l(m[b]);
				auto cmp = LESS(_a, b);
				return ADD(_a, WCAND(cmp, SUB(_b, _a)));
			});

			pat.AddRule(Native::MakeFloat("min", Native::Min, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				auto _b = l(m[b]);
				auto cmp = GREATEREQ(_a, b);
				return ADD(_a, WCAND(cmp, SUB(_b, _a)));
			});

			/* ABS via comparator */
			pat.AddRule(Native::MakeFloat("abs", Native::Abs, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				auto cmp = GREATEREQ(_a, Native::Constant::New(0.f));
				return MUL(_a, MAD(Native::Constant::New(-1.f), cmp, 2.f));
			});

			/* ROUND via forced loss of precision */
			auto precisionLimit = Native::Constant::New((float)(1 << 23));
			pat.AddRule(Native::MakeFloat("nearest", Native::Round, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				return SUB(ADD(_a, precisionLimit), precisionLimit);
			});

			pat.AddRule(Native::MakeFloat("floor", Native::Floor, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				return SUB(ADD(ADD(_a, Native::Constant::New(-0.5f)), precisionLimit), precisionLimit);
			});

			pat.AddRule(Native::MakeFloat("ceil", Native::Ceil, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				return SUB(ADD(ADD(_a, Native::Constant::New(0.5f)), precisionLimit), precisionLimit);
			});

			pat.AddRule(Native::MakeFloat("trunc", Native::Truncate, a),
				[=](Matches& m, Lowering& l) -> CTRef {
				auto _a = l(m[a]);
				auto _c = ADD(Native::Constant::New(-0.5f), GREATER(_a, Native::Constant::New(1.f)));
				return SUB(ADD(_c, precisionLimit), precisionLimit);
			});
		}

		static void DoRemaps(WCNetList& nl, std::unordered_map<WCInstr*, WCInstr*> replace) {
			auto remap = [&](WCInstr* src) {
				auto f = replace.find(src);
				if (f == replace.end()) return src;
				return f->second;
			};

			for (auto& i : nl) {
				i.Op1() = remap(i.Op1());
				i.Op2() = remap(i.Op2());
				i.Tau() = remap(i.Tau());
			}
		}

		static bool ShareSubgraphs(WCNetList& nl) {
			using WCIdentity = std::tuple<PAType, WCInstr*, WCInstr*, float, WCInstr*, int>;
			std::map<WCIdentity, WCInstr*> distinct;
			bool didMakeChanges = false;
			std::unordered_map<WCInstr*, WCInstr*> replace;
			auto remap = [&](WCInstr* src) {
				auto f = replace.find(src); 
				if (f == replace.end()) return src;
				didMakeChanges = true;
				return f->second;
			};

			for (auto& i : nl) {
				i.Op1() = remap(i.Op1());
				i.Op2() = remap(i.Op2());
				i.Tau()  = remap(i.Tau());
				auto id = std::make_tuple(
					i.Type(), i.Op1(), i.Op2(), i.Param(), i.Tau(), i.Delay());
				auto duplicate = distinct.find(id);
				if (duplicate != distinct.end()) {
					replace[&i] = duplicate->second;
				} else distinct.insert(std::make_pair(id, &i));
			}
			if (replace.size()) DoRemaps(nl, replace);
//			if (replace.size()) std::clog << "[SGS]\n" << nl;
			return !replace.empty();
		}

		static void CountUses(WCNetList& nl, std::unordered_map<WCInstr*,int>& uses) {
			for (auto& i : nl) {
				uses[i.Op1()]++;
				if (i.Op2() !=& i) uses[i.Op2()]++;
				uses[i.Tau()]++;
			}
		}

		static bool EliminateDeadPAs(WCNetList& nl, std::unordered_map<WCInstr*, int>& uses) {
			bool didMakeChanges = false;

			for (auto i = nl.begin();i != nl.end();) {
				if (!uses[&*i] && i->Type() != VOLATILE_CONSTANT) {
					didMakeChanges = true;
					nl.erase(i++);
				}
				else ++i;
			}
//			if (didMakeChanges) std::clog << "[DCE]\n" << nl;
			return didMakeChanges;
		}

		static bool StrengthReduction(WCNetList& nl, std::unordered_map<WCInstr*, int>& uses) {
			bool changes = false;
			std::unordered_map<WCInstr*, WCInstr*> replace;
			auto remap = [&](WCInstr* src) {
				auto f = replace.find(src);
				if (f == replace.end()) return src;
				changes = true;
				return f->second;
			};

			for (auto i = nl.begin();i != nl.end(); ++i) {
				i->Op1() = remap(i->Op1());
				i->Op2() = remap(i->Op2());
				i->Tau() = remap(i->Tau());
				// kill multiplies by known constants
				if (i->Type() == MUL && i->Delay() == 0) {
					if (i->Op1()->Type() == CONSTANT) {
						if (i->Op1()->Param() == 1.f) {
							replace[&*i] = i->Op2();
						} else if (i->Op1()->Param() == 0.f) {
							replace[&*i] = i->Op1();
						} else {
							i->Type() = AMP;
							i->Param() = i->Op1()->Param();
							i->Op1() = i->Op2();
							i->Op2() = nullptr;
							changes = true;
						}
					}
					if (changes == false && i->Op2()->Type() == CONSTANT) {
						if (i->Op2()->Param() == 1.f) {
							replace[&*i] = i->Op1();
						} else if (i->Op2()->Param() == 0.f) {
							replace[&*i] = i->Op2();
						}
					}
				}

				// kill redundant additions
				if (i->Type() == ADD && i->Delay() == 0) {
					if (i->Op1()->Type() == CONSTANT && i->Op1()->Param() == 0.f) {
						replace[&*i] = i->Op2();
					}
					else if (i->Op2()->Type() == CONSTANT && i->Op2()->Param() == 0.f) {
						replace[&*i] = i->Op1();
					}
				}

				// kill redundant AMPs
				if (i->Type() == AMP && i->Delay() == 0 && i->Param() == 1.f) {
					replace[&*i] = i->Op1();
				}

				// simplify MADs
				if (i->Type() == MAD && i->Delay() == 0) {
					if (i->Param() == 0.f) {
						replace[&*i] = i->Op2();
					} else if (i->Param() == 1.f) {
						i->Param() = 0;
						i->Type() = ADD;
						changes = true;
					}
				}

				// hack to improve PEQ

				// merge delay lines into PAs
				//if (i->Type() == AMP && i->Param() == 1.f && i->Delay()) {
				//	if (uses[i->Op1()] < 2) {
				//		int dly = i->Delay() + i->Op1()->Delay();
				//		*i = *i->Op1();
				//		i->Delay() = dly;
				//		changes = true;
				//	}
				//}
			}

			if (replace.size()) {
				DoRemaps(nl, replace);
				changes = true;
			}

//			if (changes) std::clog << "[SR]\n" << nl;
			return changes;
		}

		static void Optimize(WCNetList& nl, std::unordered_map<WCInstr*, int> roots) {
			bool changes = true;
//			std::clog << "[Optimize]\n" << nl;
			while (changes) {
				changes = ShareSubgraphs(nl);
				std::unordered_map<WCInstr*, int> uses(roots);
				CountUses(nl, uses);
				changes |= StrengthReduction(nl, uses);
				changes |= EliminateDeadPAs(nl, uses);
			}
		}

		TYPED_NODE(WaveCoreTransmit, TypedUnary)
			int tokenIndex;
			WaveCoreTransmit(CTRef sig, int tokenIndex) :TypedUnary(sig), tokenIndex(tokenIndex) {}
		PUBLIC
			static WaveCoreTransmit* New(CTRef sig, int tokenIndex) { return new WaveCoreTransmit(sig, tokenIndex); }
			Type Result(ResultTypeTransform& rt) const { return GetUp(0)->Result(rt); }
		END

		Interpreter::Var WaveCoreTransmit::Interpret(InterpretTransform&) const { throw; }

		class FlattenCG : public CachedTransform<const Typed, CTRef> {
			CTRef bindArg;
			Type argTy;
			int tokenCounter = 0;
		public:
			int GetNumNativeEdges() const { return tokenCounter; }
			Monad* OuterRegionEdges;
			FlattenCG(CTRef root, CTRef bindArg, Type argTy, Monad* OuterRegion) : CachedTransform(root), bindArg(bindArg), OuterRegionEdges(OuterRegion),argTy(argTy) {
				OuterRegionEdges->Connect(Typed::Nil());
			}

			CTRef MakeEdge(const Type& t, CTRef node, const Reactive::Node *rx) {
				if (t.IsPair()) {
					return Pair::New(
						MakeEdge(t.First(), node->GraphFirst(), rx->First()),
						MakeEdge(t.Rest(), node->GraphRest(), rx->Rest()));
				}
				//auto wct = WaveCoreTransmit::New(node, ti);
				if (t.IsFloat32()) {
					int ti = tokenCounter++;
					auto wct = Native::ForeignFunction::New("float", "WaveCoreTransmit");
					wct->AddParameter("int32", Native::Constant::New(int32_t(ti)), Type::Int32);
					wct->AddParameter("float", node, Type::Float32);
					wct->SetReactivity(rx);
					OuterRegionEdges->Connect(wct);
					std::stringstream n;
					n << ".ControlToken" << ti;
					return K3::Nodes::WaveCore::ExtInput::New(n.str().c_str());
				}
				return Typed::Nil();
			}

			CTRef operate(CTRef src) {
				Argument *a; FunctionCall *fc; FunctionSequence *fseq; Boundary* b;
				if (src->Cast(a)) return bindArg;
				else if (src->Cast(b)) {
					ResultTypeWithConstantArgument rta(b->GetUp(0), argTy);
					return MakeEdge(b->GetUp(0)->Result(rta), b->GetUp(0), b->GetUpstreamReactivity());
				} else if (src->Cast(fc)) {
					FlattenCG sub(fc->GetBody(), (*this)(fc->GetUp(0)), fc->ArgumentType(), OuterRegionEdges);
					return TLS::WithNewStack([&]() {return sub.Go();});
				} else if (src->Cast(fseq)) {
					std::vector<CTRef> args;
					std::vector<Type> argTys;
					args.reserve(fseq->GetRepeatCount());
					args.push_back((*this)(fseq->GetUp(0)));
					ResultTypeWithConstantArgument rtcaFront(args.back(), argTy);
					argTys.push_back(rtcaFront.Go());

					for (size_t i = 0;i < fseq->GetRepeatCount();++i) {
						FlattenCG iter(fseq->GetIterator(), args.back(), argTys.back(), OuterRegionEdges);
						args.push_back(iter.Go());
						ResultTypeWithConstantArgument rtca(fseq->GetIterator(), argTys.back());
						argTys.push_back(rtca.Go());
					}
					FlattenCG tail(fseq->GetTailCall(), args.back(), argTys.back(), OuterRegionEdges);
					CTRef result = tail.Go();

					ResultTypeWithConstantArgument rtcaTail(fseq->GetTailCall(), argTys.back());
					Type resultTy = rtcaTail.Go();

					for (size_t i = fseq->GetRepeatCount() - 1; i < fseq->GetRepeatCount();--i) {
						auto geneArg = Pair::New(args[i], result);
						auto geneArgTy = Type::Pair(argTys[i], resultTy);
						FlattenCG gener(fseq->GetGenerator(), geneArg, geneArgTy, OuterRegionEdges);
						result = gener.Go();
						ResultTypeWithConstantArgument rtca(fseq->GetGenerator(), geneArgTy);
						resultTy = rtca.Go();
					}
					return result;
				} else return src->IdentityTransform(*this);
			}
		};

		Class* WaveCore::Build(Kronos::BuildFlags flags) {
			RegionAllocator ra(buildMemory);
			WaveCoreClass* wc = new WaveCoreClass(GetResultType( ), GetOutputReactivity( ));
			try {
				PartialTransform<Transform::Identity<const Typed>> BuildIPCEdges(intermediateAST);

				Monad *OuterRegion = Monad::New();
				OuterRegion->Connect(Typed::Nil());

//				std::clog << *intermediateAST;
				FlattenCG flat(intermediateAST, Typed::Nil(), Type::Nil, OuterRegion);
				intermediateAST = flat.Go();

				wc->SetNumNativeEdges(flat.GetNumNativeEdges());

				Transform::LoweringPatterns wcLowerPats;
				InsertWaveCoreLoweringPatterns(wcLowerPats);
				Transform::Lowering wcLowering(intermediateAST, wcLowerPats);
				intermediateAST = wcLowering.Go();

				DriverSignature dsig(Type::InvariantString(abstract_string::cons("audio")), Type::InvariantI64(10));
//				std::clog << *intermediateAST;
				WaveCoreTransform xfm(intermediateAST, dsig, wc->GetNetList(dsig));

				Pair *p; WCInstr *left, *right;
				if (intermediateAST->Cast(p)) {
					// stereo output
					xfm.Rebase(p->GetUp(0));
					left = xfm.Go();
					xfm.Rebase(p->GetUp(1));
					right = xfm.Go();
				} else {
					// mono output - duplicate
					right = left = xfm.Go();
				}

				wc->SetOutputs(left, right);

				std::unordered_map<WCInstr*, int> uses;
				uses[left]++; uses[right]++;

				Backends::Optimize(wc->GetNetList(dsig), uses);

				// build native driver stub
				intermediateAST = OuterRegion;
				wc->SetNativeProcess(LLVM::Build(BuildFlags::Default, "host", "host", "host"));
				return wc;
			} catch (...) {
				delete wc;
				throw;
			}
		}

		static std::string GetProcessName(Type driver) {
			DriverSignature dsig(driver);
			std::stringstream pn;
			pn << dsig.GetMetadata();
			if (dsig.GetDiv() != 1.0) pn << "_" << dsig.GetDiv() << "th";
			return pn.str();
		}

		void WaveCoreClass::WriteProcessGraph(const char *nsPrefix, std::ostream& write) {
			static const int clock = 86016000;
			write <<
				"------------------------------------------------------\n"
				" WaveCore Process Graph for " << nsPrefix << "\n"
				" Generated by Kronos " << KRONOS_PACKAGE_VERSION << "\n"
				" (c) 2015 Vesa Norilo, University of Arts Helsinki\n"
				"------------------------------------------------------\n\n";

			write <<
				"\tSequencer Scheduler {\n"
				"\t\tFrequency " << clock << "\n"
				"\t\tFireLink AudioRate " << clock / AudioRate << "\n"
				"\t}\n\n";

			if (NumNativeEdges) {
				write <<
					"\tProcess NativeControl {\n"
					"\t\tSource  " << nsPrefix << "Control.proc\n"
					"\t\tOutEdge Parameters ControlPort\n"
					"\t}\n\n";
			}

			write <<
				"\tProcess " << nsPrefix << " {\n"
				"\t\tSource   " << nsPrefix << ".proc\n"
				"\t\tFireLink Scheduler  AudioRate\n"
				"\t\tInEdge   ADC        AudioInPort\n";

			if (NumNativeEdges) write << 
				"\t\tInEdge   Parameters ControlInPort\n";

			write << 
				"\t\tOutEdge  DAC        AudioOutPort\n"
				"\t}\n\n"
				"\tProcess ADC {\n"
				"\t\tSource   ADC.proc\n"
				"\t\tFireLink Scheduler  AudioRate\n"
				"\t\tOutEdge  ADC        ADCport\n"
				"\t}\n\n"

				"\tProcess DAC {\n"
				"\t\tSource   DAC.proc\n"
				"\t\tFireLink Scheduler  AudioRate\n"
				"\t\tInEdge   DAC        DACport\n"
				"\t}\n\n"
				;


		}

		void WaveCoreClass::WriteProcess(const char *nsPrefix, std::ostream& write) {
			write <<
				"------------------------------------------------------\n"
				" WaveCore Process for " << nsPrefix << "\n"
				" Generated by Kronos " << KRONOS_PACKAGE_VERSION << "\n"
				" (c) 2015 Vesa Norilo, University of Arts Helsinki\n"
				"------------------------------------------------------\n"
				"\n"

				"\tProcessEntity " << nsPrefix << "{ \n"
				"\t\tProcType WaveCore\n";
			for (auto &d : nl) {
				auto pn = GetProcessName(d.first);
				write << 
					"\t\tWpp\n"
					"\t\t\tWppID " << nsPrefix << "\n"
					"\t\t\tWppSource " << nsPrefix << "_" << pn << ".sfg\n"
					"\t\tEndWpp\n";
			}

			write <<
				"\t\tPort AudioInPort\n"
				"\t\t\tDirection InBound\n"
				"\t\t\tSignal    " << nsPrefix << " .AudioInL\n"
				"\t\t\tSignal    " << nsPrefix << " .AudioInR\n"
				"\t\tEndPort\n";

			if (NumNativeEdges) {
				write <<
					"\t\tPort ControlInPort\n"
					"\t\t\tDirection InBound\n";
				for (int i(0);i < NumNativeEdges;++i) {
					write << "\t\t\tSignal    " << nsPrefix << " .ControlToken" << i << "\n";
				}
				write <<
					"\t\tEndPort\n";
			}
			
			write <<
				"\t\tPort AudioOutPort\n"
				"\t\t\tDirection OutBound\n"
				"\t\t\tSignal    " << nsPrefix << " .AudioOutL\n"
				"\t\t\tSignal    " << nsPrefix << " .AudioOutR\n"
				"\t\tEndPort\n"
				"\t}\n";
		}

		void WaveCoreClass::WriteProcessPartition(Type driver, WCNetList& nl, const char *nsPrefix, std::ostream& write) {
			write <<
				".------------------------------------------------------\n"
				". WaveCore Process Partition for " << nsPrefix << "/" << driver << "\n"
				". Generated by Kronos " << KRONOS_PACKAGE_VERSION << "\n"
				". (c) 2015 Vesa Norilo, University of Arts Helsinki\n"
				".------------------------------------------------------\n"
				".\n"
				"SFG\n.\n"
				". Externals\n";
			for (auto&& e : nl.extVars) {
				float def = 0;
				if (e == ".AudioInL" || e == ".AudioInR") def = 1.5;
				write << "C void void " << e << " " << def << " 1\n";
				
			}
			write <<
				".\n"
				". Signal Processor\n"
				<< nl <<
				".\n. Outputs\n"
				"C void void DACOffset 1.5 0\n"
				"A " << GetName(leftOut)  << " DACOffset .AudioOutL 0 1\n"
				"A " << GetName(rightOut) << " DACOffset .AudioOutR 0 1\n"
				".\nGFS\n";
		}

		void WaveCoreClass::MakeStatic(const char *nsPrefix, const char *fileType, std::ostream& write, Kronos::BuildFlags flags, const char*, const char*, const char*) {
			if (nsPrefix == nullptr) nsPrefix = "KronosDSP";

			ofstream wpg(nsPrefix + std::string(".wpg"));
			WriteProcessGraph(nsPrefix, wpg);

			ofstream proc(nsPrefix + std::string(".proc"));
			WriteProcess(nsPrefix, proc);

			if (NumNativeEdges) {
				ofstream proc(nsPrefix + std::string("Control.proc"));
				proc <<
					"------------------------------------------------------\n"
					" WaveCore Control Process for " << nsPrefix << "\n"
					" Generated by Kronos " << KRONOS_PACKAGE_VERSION << "\n"
					" (c) 2015 Vesa Norilo, University of Arts Helsinki\n"
					"------------------------------------------------------\n"
					"\n"
					"\tProcessEntity NativeControl {\n"
					"\t\tProcType Control\n"
					"\t\tPort ControlPort\n"
					"\t\t\tDirection OutBound\n";
				for (int i(0);i < NumNativeEdges;++i) {
					proc << "\t\t\tSignal void .ControlToken" << i << " 0\n";
				}
				proc <<
					"\t\tEndPort\n"
					"}\n\n";
				proc.close();
			}

			for (auto&& wppnl : nl) {
				ofstream wpp(std::string(nsPrefix) + "_" + GetProcessName(wppnl.first) + ".sfg");
				WriteProcessPartition(wppnl.first, wppnl.second, nsPrefix, wpp);
			}

			ofstream script(nsPrefix + std::string(".script"));
			script <<
				"------------------------------------------------------\n"
				" WaveCore Build Script for " << nsPrefix << "\n"
				" Generated by Kronos " << KRONOS_PACKAGE_VERSION << "\n"
				" (c) 2015 Vesa Norilo, University of Arts Helsinki\n"
				"------------------------------------------------------\n"
				"\n"
				"VERBOSITY 5\n"
				"WPG " << nsPrefix << ".wpg\n"
				"SIMTIME 0.1\n"
				"PSIM\n";
			script.close();
		}
	}
}