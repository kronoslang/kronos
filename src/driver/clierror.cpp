#include <ostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include "tinyxml.h"
#include "kronos.h"
#include <cctype>


using namespace std;

std::string UnwrapException(const char* txt, const char*& pos) {
	std::stringstream out;

	const char exception[] = "Exception{";
	if (strncmp(txt, exception, sizeof(exception) - 1) == 0) {
		std::string em(txt + sizeof(exception) - 1);
		em.pop_back();
		if (em.back() == '}') {
			std::string title = em.substr(0, em.find_first_of('{'));
			em = em.substr(title.size() + 1);
			em.pop_back();
			out << em << " (" << title << ")";
		} else {
			out << em;
		}
	} else {
		out << txt;
	}
	return out.str();
}

void FormatErrorNode(const TiXmlElement& node, std::ostream& out, Kronos::Context& cx, std::string& inFile) {
	auto pos = node.Attribute("at");
	auto code = node.Attribute("c");
	auto msg = node.Attribute("msg");
	auto recv = node.Attribute("a");
	auto expect = node.Attribute("x");


	if (node.GetText()) {
		out << UnwrapException(node.GetText(), pos);
	}

	if (code) {
//		out << " (E" << code << ")";
	}

	if (msg) {
		out << " " << msg;
	}

	out << "\n";

	if (pos) {
		std::string p = cx.GetModuleAndLineNumberText((const char*)strtoull(pos, nullptr, 16));
		if (p.size() && p != inFile) {
			inFile = p;
			if (inFile.front() != '#') {
				out << " in " << p << "\n"
					<< cx.ShowModuleLine((const char*)strtoull(pos, nullptr, 16));
			}
		}
	}
 
	if (recv) {
		out << "Received " << recv << "\n";
	}

	if (expect) {
		out << "Expected " << expect << "\n";
	}
}


void Diagnose(const TiXmlElement& node, std::ostream& out, Kronos::Context& cx, int indent) {
	using namespace std::string_literals;
	if (node.FirstChildElement("td")) {
		return;
	}
	
	if (indent > 0 && node.Value() == "err"s) {
		char fatal[] = "Fatal{";
		if (strncmp(node.GetText(), fatal, sizeof(fatal) - 1) == 0) return;
		std::string dummy;
		FormatErrorNode(node, out, cx, dummy);
		out << "\n";
		return;
	}

	if (node.Value() == "eval"s) {
		std::string callLine;
		std::string sourcePos;
		auto label = node.Attribute("label");
		callLine = label ? label : "(anonymous)";
		auto info = node.FirstChildElement("i");
		if (info) {
			auto args = info->Attribute("a");
			if (args) {
				if (*args != '(') callLine += "(";
				if (strlen(args) > 20) {
					callLine += std::string(args, args + 20) + " ... ";
				} else {
					callLine += args;
				}
				if (callLine.back() != ')') callLine += ")";
			}
		}

		if (indent > 0) {
			out << left << setw(indent) << "| " << callLine << "\n";
		}
	}

	if (node.Value() == "constr"s) {
		auto failure = node.FirstChildElement("err");
		if (failure) {
			Diagnose(*failure, out, cx, indent);
		}
		return;
	}

	for (auto child = node.FirstChildElement(); child; child = child->NextSiblingElement()) {
		Diagnose(*child, out, cx, indent + 1);
	}
}

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx, int startIndent = 0) {
	string file("");
	TiXmlDocument doc;
	stringstream(xml) >> doc;
	if (doc.Error()) return;
	if (doc.FirstChildElement()) Diagnose(*doc.FirstChildElement(), out, cx, startIndent);
	out.flush();
}

bool ListErrorNodesEDN(const TiXmlElement& node, std::ostream& out, Kronos::Context& cx, int indent, std::string& inFile) {
	bool hasTrace = false;
	if (string(node.Value()) == "eval") {
		hasTrace = true;
		if (node.FirstChildElement("td")) return false;

		out << "{\n";
		auto info = node.FirstChildElement("i");
		string args, pos, func;
		func = node.FirstAttribute()->ValueStr();
		if (info) {
			for (auto at = info->FirstAttribute(); at; at = at->Next()) {
				if (at->NameTStr() == "a") args = at->ValueStr();
				else if (at->NameTStr() == "at") pos = at->ValueStr();
			}
		}

		out << " :in \"" << func << " " << args << "\" " << endl;

		string at = cx.GetModuleAndLineNumberText((const char*)strtoull(pos.c_str(), NULL, 16));

		if (at != inFile) {
			if (at.size()) { out << " :source \"" << at << "\" "; }
			inFile = at;
		}

		auto forms = node.FirstChildElement("form");

		if (forms && forms->FirstChildElement("err")) {
			out << " :errors [ ";
			for (auto et = forms->FirstChildElement("err"); et; et = et->NextSiblingElement("err")) {
				out << "\"" << et->GetText() << "\"\n";
			}
			out << "] ";
		} else {
			out << " :errors [\"No valid forms\"]";
		}

    } else {
        if (node.FirstChildElement("err")) {
            hasTrace = true;
            out << " :errors [";
            for (auto et = node.FirstChildElement("err"); et; et = et->NextSiblingElement("err")) {
                out << "\"" << et->GetText() << "\" ";
            }
            out << "]";
        }
    }
    
    bool hasCallees = false;

	for (auto n = node.FirstChildElement(); n; n = n->NextSiblingElement()) {
        if (!hasCallees) { out << " :callees ["; hasCallees = true; }
		hasTrace |= ListErrorNodesEDN(*n, out, cx, indent + 1, inFile);
	}
    
    if (hasCallees) out << "]";

	if (node.ValueStr() == "eval") {
		out << "}";
	}

	return hasTrace;
}

void FormatErrorsEDN(const std::string& xml, std::ostream& out, Kronos::Context& cx) {
	string file("");
	TiXmlDocument doc;
	stringstream(xml) >> doc;
    out << "{";
    if (doc.FirstChildElement()) ListErrorNodesEDN(*doc.FirstChildElement(), out, cx, 0, file);
    out << "}";
}

int GetNumberOfChannels(Kronos::Type outFrameTy) {
	using namespace Kronos;
	int numOutChannels = 0;
	if (outFrameTy.IsUserType()) outFrameTy = outFrameTy.GetUserTypeContent();
	while (true) {
		if (outFrameTy->IsPair( )) {
			if (outFrameTy.GetFirst( ) != GetFloat32Ty( )) {
				return 0;
			} else {
				numOutChannels++;
				outFrameTy = outFrameTy.GetRest( );
			}
		} else if (*outFrameTy == GetFloat32Ty( )) {
			return ++numOutChannels;
		} else if (*outFrameTy == GetNil( )) {
			return numOutChannels;
		} else {
			return 0;
		}
	}
}
