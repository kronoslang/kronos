#include "NodeBases.h"
#include <ostream>
#include <iomanip>
#include <cstdarg>
#include <sstream>
#include "Errors.h"
#include "EnumerableGraph.h"
#include "Parser.h"

namespace K3 {
	namespace Nodes{
		GenericBase::GenericBase() {
			pos = K3::Parser::CurrentSourcePosition();
		}

		bool GenericBase::VerifyAllocation(MemoryRegion* region, CGRef node) {
/*			return Qxx::FromGraph(node).Where([&](CGRef node)
			{
				return region->HasDependency(node->GetHostRegion()) == false;
			}).Any() == false;*/
			return true;
		}

		Specialization Generic::TypeError(TypeDescriptor* desc) {
			return Specialization(nullptr,Type(desc));
		}

		Specialization Generic::TypeError(TypeDescriptor* desc, const Type& content) {
			return Specialization(nullptr,Type::User(desc,content));
		}

		Specialization Generic::SpecializationFailure() {
			return Specialization(nullptr,Type(&K3::SpecializationFailure));
		}

		CGRef Generic::IdentityTransform(GraphTransform<const Generic,CGRef>& copy) const {
			Generic *tmp(ConstructShallowCopy());
			assert(tmp->GetNumCons() == 0 || tmp->upstream != upstream);
			for(unsigned i(0);i<GetNumCons();++i) {
				tmp->Reconnect(i,copy(tmp->GetUp(i)));
			}
			return tmp;
		}


		SpecializationDiagnostic::SpecializationDiagnostic(std::ostream *r,Verbosity loglevel,int tabSize)
			:report(r),verbosity(loglevel),indent(0) {
//			if (report) {*report<<"<"<<element<<">";}
		}

		SpecializationDiagnostic::~SpecializationDiagnostic() {
//			if (report) {*report<<"</"<<element<<">";}
		}

		Specialization SpecializationDiagnostic::TypeError(const char *node, const Type& arg, const Type& nested) {
			if (IsActive()) {
				return { nullptr, Type::User(&FatalFailure,
											 Type::Tuple(Type(node), arg, nested)) };
			} else {
				return { nullptr, Type(&FatalFailure) };
			}
		}

		void SpecializationDiagnostic::DoIndent() { if (indent > 1) (*report) << setw(indent) << " ";}

		SpecializationDiagnostic::DiagnosticBlock::DiagnosticBlock(SpecializationDiagnostic &d,const char *block, const char *attr):diag(d),b(block) {
			if (diag.report && b) {
				diag.DoIndent();
				*diag.report << "<"<<b<<attr<<">\n";
				diag.indent++;
			}
		}

		SpecializationDiagnostic::DiagnosticBlock::~DiagnosticBlock() {
			if (diag.report && b) { 
				diag.indent--;
				diag.DoIndent();
				*diag.report << "</"<<b<<">\n";
			}
		}


		SpecializationDiagnostic::DiagnosticBlock SpecializationDiagnostic::Block(Verbosity loglevel, const char *b, const char *fmt, ...) {
			if (loglevel<verbosity) return DiagnosticBlock(*this,0,0);

			char buffer[1024];
			buffer[0]=0;
			if (report) {
				if (fmt) {
					va_list arglist;
					va_start(arglist,fmt);
					buffer[0]=' ';
					vsnprintf(buffer+1,1023,fmt,arglist);
				}
			}
			return DiagnosticBlock(*this,b,buffer);
		}

		void SpecializationDiagnostic::Diagnostic(Verbosity loglevel, CGRef src, int code, const char *msg, ...) {
			if (report && loglevel >= verbosity) {
				const char *element;
				static const char *error_str("err");
				switch(code) {
					case Error::Info:element = "i";break;
					case Error::Success:element = "s";break;
					default:element = error_str; break;
				}
				char buffer[1024];
				va_list arglist;
				va_start(arglist,msg);
				vsnprintf(buffer,1024,msg,arglist);
				va_end(arglist);
				if (element == error_str) {
					DoIndent();
					uintptr_t pos = (uintptr_t)src->GetRepositoryAddress();
					*report << "<" << element << " c='" << code << "' at='" << std::hex << pos << std::dec << "'>" << buffer << "</" << element << ">\n";
				} else {
					DoIndent();
					*report << "<"<<element<<">"<<buffer<<"</"<<element<<">\n";
				}
			}
		}

		template <typename STRI> void EscapeStringXML(std::ostream& strm, const STRI& beg, const STRI& end) {
			for (auto i(beg); i != end; ++i) {
				if (iscntrl(*i)) {
					strm << "&#" << (int)* i << ";";
				} else {
					switch (*i) {
					case '\"':strm << "&quot;"; break;
					case '<':strm << "&lt;"; break;
					case '>':strm << "&gt;"; break;
					case '&':strm << "&amp;"; break;
					case '\'': strm << "&apos;"; break;
					default:strm << (char)* i; break;
					}
				}
			}
		}

		void XMLAttr(std::ostream& s, const Type& t) {
			std::stringstream ss;
			ss << t;
			auto str = ss.str();
			EscapeStringXML(s, str.cbegin(), str.cend());
		}

		void SpecializationDiagnostic::Diagnostic(Verbosity loglevel, CGRef src, int code, const Type& recv, const char *msg, ...) {
			if (report && loglevel >= verbosity) {
				const char *element;
				switch(code) {
					case Error::Info:element = "i";break;
					case Error::Success:
						DoIndent();
						*report << "<s r='"; XMLAttr(*report, recv); *report << "'/>\n";
						return;
					default:element = "err";break;
				}

				char buffer[1024];
				va_list arglist;
				va_start(arglist,msg);
				const char *end = buffer + vsnprintf(buffer,1024,msg,arglist);
				va_end(arglist);

				DoIndent();
				*report << "<" << element << " c='" << code << "' a='";
				XMLAttr(*report, recv);
				uintptr_t pos = (uintptr_t)src->GetRepositoryAddress();
				*report << "' at='" << std::hex << pos << std::dec << "'>";
				EscapeStringXML(*report, (const char*)buffer, end);
				*report << "</" << element << ">\n";
			}
		}

		void SpecializationDiagnostic::SuccessForm(Verbosity loglevel, const char* func, const Type& arg, const Type& res) {
			if (report && loglevel >= verbosity) {
				DoIndent();
				*report << "<td f='" << func << "' a='";
				XMLAttr(*report,arg); *report << "' r='"; XMLAttr(*report, res); *report << "'/>\n";
			}
		}

		void SpecializationDiagnostic::Diagnostic(Verbosity loglevel, CGRef src, int code, const Type& recv, const Type& expect, const char *msg, ...) {
			if (report && loglevel >= verbosity) {
				const char *element = "err";

				switch(code) {
					case Error::Info:element = "i";break;
					case Error::Success:
						DoIndent();
						*report << "<s a='"; XMLAttr(*report, expect); *report << "' r='"; XMLAttr(*report, recv); *report << "'/>\n";
						return;
					default:element = "err";break;
				}

				char buffer[1024];
				va_list arglist;
				va_start(arglist,msg);
				vsnprintf(buffer,1024,msg,arglist);
				va_end(arglist);
				DoIndent();
				*report << "<" << element << " code='" << code << "' a='";
				XMLAttr(*report, recv);
				*report << "' x='";
				XMLAttr(*report, expect);
				std::uintptr_t pos = (uintptr_t)src->GetRepositoryAddress();
				*report << "' at='" << std::hex << pos << std::dec << "'>" << buffer << "</" << element << ">\n";
			}
		}

		std::pair<Type, Graph<Typed>> SpecializationTransform::Process(CGRef root, const Type& argument, SpecializationState::Mode m) {
			RegionAllocator specRegion;
			SpecializationDiagnostic d(0);
			Specialization tmp(SpecializationTransform(root,argument,d,m).Go());
			return make_pair(tmp.result,tmp.node);
		}

		Type SpecializationTransform::Infer(CGRef root, const Type& argument) {
			RegionAllocator inferral;
			return Process(root,argument,SpecializationState::Normal).first;
		}
	};
};
