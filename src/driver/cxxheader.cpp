#include "kronos.h"
#include <ostream>
#include <regex>
#include <set>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <tuple>

std::string CSanitize(std::string symbol) {
	for (auto &c : symbol) {
		if (!isalnum(c)) c = '_';
	}

	for (int i(1); i<symbol.size( ); ++i) if (symbol[i - 1] == '_') symbol[i] = toupper(symbol[i]);

	std::regex removeUnderScores("([a-zA-Z]*)_+([a-zA-Z]*)");

	symbol = std::regex_replace(symbol, removeUnderScores, "$1$2");
	symbol = std::regex_replace(symbol, std::regex("_+"), "_");

	if (!isalpha(symbol[0])) return "_" + symbol;
	else return symbol;
}

class CppSymbolDict : public std::set<std::string> {
	std::string prefix;

	void MakeSym(std::ostream& str) { }

	template <typename ARG, typename... ARGS> void MakeSym(std::ostream& str, ARG a, ARGS... as) {
		str << a;
		MakeSym(str, as...);
	}

public:
	CppSymbolDict(const std::string& pf) :prefix(pf) { }

	template <typename... ARGS> std::string operator()(ARGS... as) {
		std::stringstream tmp;
		MakeSym(tmp, as...);
		std::string s = CSanitize(prefix + "_" + tmp.str( ));
		if (find(s) == end( )) {
			insert(s);
			return s;
		} else {
			int idx(2);
			while (true) {
				std::stringstream tmp;
				tmp << s << idx++;
				if (find(tmp.str( )) == end( )) {
					insert(tmp.str( ));
					return tmp.str( );
				}
			}
		}
	}
};

struct CppTypeDefs;

bool OutputCType(std::ostream& stream, Kronos::Type t, CppTypeDefs *typeDefs=nullptr, int memberIndex = 1);
	
struct CppTypeDefs : public std::unordered_map<Kronos::Type, std::string> {
	std::ostream& writeDefs;
	CppSymbolDict& dict;
	int numCompositeTypes;
	CppTypeDefs(std::ostream& writeDefsToStream, CppSymbolDict& dict) :writeDefs(writeDefsToStream), dict(dict), numCompositeTypes(0) { }
	std::string operator()(const Kronos::Type& t) {
		auto f(find(t));
		if (f == end( )) {
			std::string typeName;
			std::stringstream def;

			if (OutputCType(def, t, this)) {
				typeName = dict("Ty", ++numCompositeTypes);
				writeDefs << "\ntypedef " << def.str( ) << " " << typeName << ";\n\n";
			} else {
				typeName = def.str( );
			}

			f = insert(make_pair(t, typeName)).first;
		}
		return f->second;
	}
};

bool OutputCType(std::ostream& stream, Kronos::Type t, CppTypeDefs *typeDefs, int memberIndex) {
	using namespace Kronos;
	if (t.SizeOf( ) < 1) stream << "void";
	else if (t == GetFloat32Ty( )) stream << "float";
	else if (t == GetFloat64Ty( )) stream << "double";
	else if (t == GetInt32Ty( )) stream << "int32_t";
	else if (t == GetInt64Ty( )) stream << "int64_t";
	else if (t.IsPair( )) {
		stream << "struct { \n";

		while (t.IsPair( )) {
			auto fst = t.GetFirst( );
			int count = 1;

			Kronos::Type rst = t.GetRest( );
			while (rst.IsPair( ) && rst.GetFirst( ) == fst) {
				count++;
				rst = rst.GetRest( );
			}

			if (rst == t.GetFirst( )) count++;

			if (fst.SizeOf( )) {
				OutputCType(stream, fst, typeDefs);
				stream << " V" << memberIndex++;
				if (count > 1) stream << "[" << count << "]";
				stream << ";\n";
			}

			if (rst == t.GetFirst( )) {
				stream << "}";
				return true;
			} else t = rst;
		}

		if (t.SizeOf( )) {
			OutputCType(stream, t, typeDefs, 1);
			stream << " V" << memberIndex << ";\n";
		}
		stream << "}";
		return true;

	} else stream << "/*unknown*/";
	return false;
};


void MakeCppHeader(const char *prefix, Kronos::Class cls, Kronos::Type arg, std::ostream& stream) {
	std::string prefixUpper(prefix);
	for (auto &c : prefixUpper) c = toupper(c);

	CppSymbolDict syms(prefix);
	CppSymbolDict members("");
	CppTypeDefs typeDefs(stream, syms);

	stream << "#ifndef __" << prefixUpper << "_H\n#define __" << prefixUpper << "_H\n\n";
	stream << "#include <stdint.h>\n#include <stdlib.h>\n#ifdef __cplusplus\n#include <cstring>\n"
		"extern \"C\" { \n#endif\n\n";

	stream << "#pragma pack(push)\n#pragma pack(1)\n";

	auto instTy = syms("Instance");
	auto argTy = syms("ArgumentTy");
	auto resTy = syms("ResultTy");

	stream << "typedef void* " << instTy << ";\n";
	stream << "typedef "; OutputCType(stream, arg, &typeDefs); stream << " " << argTy << ";\n";
	stream << "typedef "; OutputCType(stream, cls.TypeOfResult( ), &typeDefs); stream << " " << resTy << ";\n\n";

	std::string GetSize = syms("GetSize");
	std::string GetSymbolOffset = syms("GetSymbolOffset");
	std::string Init = syms("Initialize");
	std::string Eval = syms("Evaluate");

	stream << "int64_t " << GetSize << "();\n";
	stream << "int64_t " << GetSymbolOffset << "(int64_t);\n";
	stream << "void " << Init << "(" << instTy << ");\n";
	stream << "void " << Eval << "(" << instTy << ", const " << argTy << "* arg, " << resTy << "* result);\n";

	std::stringstream updaters;

	std::stringstream inputTable;
	std::stringstream inputEnum;
	std::string Cons = syms("Cons");
	std::stringstream ctorParams, cxxCtorParams, ctorRelay, symInit;

	bool First = true;
	int idx = 1;

	idx = 1;
	stream << "\n/* Slot accessors */\n";
	for (auto&& input : cls.GetListOfVars( )) {
		std::string varName(CSanitize(input.AsString( )));
		std::string ivar(members(input.AsString( )));
		std::string ivarTy(typeDefs(cls.TypeOfVar(input)));

		auto SlotAccessor = syms("Get" + varName + "Input");
		stream << "const void **" << SlotAccessor << "(" << instTy << " self) { \n\t"
		<< "return (const void**)((char *)self + " << GetSymbolOffset << "( " << cls.GetVarIndex(input) << " ));\n}\n";

		auto endOfList = Kronos::TypeIter(Kronos::GetNil());
		if (std::find(Kronos::TypeIter(cls.GetUndefinedVars( )), endOfList, input) == endOfList) {
			if (First) First = false;
			else { ctorParams << ", "; ctorRelay << ", "; cxxCtorParams << ", "; }
			ctorParams << "const " << ivarTy << " *" << ivar;
			symInit << "\t*" << SlotAccessor << "( mem ) = " << ivar << ";\n";
			//symInit << "\t" << "*(" << GetSymbolOffset << "( " << cls.GetVarIndex(ivar) << " )  = " << ivar << ";\n";
			cxxCtorParams << "const " << ivarTy << " *" << ivar;
			ctorRelay << ivar;
		}
	}

	stream << "\n/* Trigger Entry Points */ \n";
	for (auto&& trigger : cls.GetListOfTriggers( )) {
		std::string trigName = cls.GetTriggerName(trigger);

		stream << "void " << prefix << trigName << "(" << instTy << "," << resTy << "* outputVector, int32_t frames);\n";

		std::string trigSym = syms(trigName.substr(4));
		inputEnum << "\t" << trigSym << ",\n";

		std::string procName = cls.GetTriggerName(trigger).substr(4);
		if (isalpha(procName.front( ))) procName.front( ) = toupper(procName.front( ));

		if (cls.HasVar(trigger)) {
			/* dual input / trigger key */
			std::string typeName = typeDefs(cls.TypeOfVar(trigger));

			updaters << "\tvoid Process" << procName << "(const " << typeName;
			updaters << "* inputVector, OutFrameTy* outputVector, int32_t vectorLength = 1) "
				"{ \n\t\t*(void**)(self + " << GetSymbolOffset << "( " << cls.GetVarIndex(trigger) << " )) = inputVector;\n"
				"\t\t" << prefix << cls.GetTriggerName(trigger) << "(self, outputVector, vectorLength);\n\t"
				"}\n\n";

			inputTable << "\t{\"" << trigName.substr(4) << "\", " << prefix << trigName << ", " << cls.TypeOfVar(trigger).SizeOf( ) << ", sizeof(void*)*" << cls.GetVarIndex(trigger) << "}, /* input: " << cls.TypeOfVar(trigger).AsString( ) << "*/\n";
		} else {
			updaters << "\tvoid Process" << procName << "(OutFrameTy* outputVector, int32_t vectorLength = 1) { " << prefix << cls.GetTriggerName(trigger) << "(self, outputVector, vectorLength); }\n\n";

			inputTable << "\t{\"" << trigName.substr(4) << "\", " << prefix << trigName << ", 0, -1}, /* no input */\n";
		}
	}

	std::string symEnum = syms("Symbol");

	stream << "\n/* Convenience Constructor */\n";
	stream << "static " << instTy << " " << Cons << "(" << ctorParams.str( ) << ") {\n"
		"	void *mem = calloc(1," << prefix << "GetSize());\n" << symInit.str() <<
		"	" << Init << "(mem);\n" << 
		"	return mem;\n"
		"}\n\n";

	stream << "#pragma pack(pop)\n";

	stream << "#ifdef __cplusplus\n}\n\n";
	stream << "/* Class wrapper */\n"
		"class " << prefix << " {\n"
		"	void *self;\n"
		"	int64_t GetSize() { return " << GetSize << "(); } \n\n"

		"public:\n"
		"	typedef " << argTy << " ArgTy;\n"
		"	typedef " << resTy << " OutFrameTy;\n"
		"	" << prefix << "(" << cxxCtorParams.str( ) << ")"
		" { \n"
		"		self = " << Cons << "(" << ctorRelay.str( ) << ");\n"
		"	}\n\n"
		"	" << prefix << "(const " << prefix << "& copyFrom) {\n"
		"		self = malloc(GetSize()); *this = copyFrom; \n"
		"	}\n\n"
		"	~" << prefix << "() {\n"
		"		free(self);\n"
		"	}\n\n"
		"	" << prefix << "& operator=(const " << prefix << "& copyFrom) { \n"
		"		std::memcpy(self,copyFrom.self,GetSize());\n"
		"	}\n\n"
		"	OutFrameTy operator()(" << (arg.IsNil() == false ? "const ArgTy& arg":"") <<") {\n"
		"		OutFrameTy tmp; operator()("<<(arg.IsNil()?"":"arg, ")<<"tmp); \n"
		"		return tmp; \n"
		"   }\n\n"
		"	void operator()(" << (arg.IsNil() == false ? "const ArgTy& arg, " : "") << "OutFrameTy& result) {\n"
		"		" << Eval << "(self, "<<(arg.IsNil()?"nullptr, ":"&arg, ")<<"&result); \n"
		"	}\n\n" << updaters.str( ) << "};\n\n";
	stream << "#endif\n";
	stream << "#endif\n";
}
