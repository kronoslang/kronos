#pragma once

#include <sstream>

namespace Kronos {
	namespace REPL {
		class EntryBuffer {
			std::stringstream buffer;
			int parens = 0;
			int squares = 0;
			int braces = 0;
			enum Mode {
				Syntax,
				Quoted,
				Commented
			} mode = Syntax;
		public:
			void Process(const std::string&);
			std::string ReadLine(const std::string& prefix = "");
			bool IsComplete();
			void Clear();
			std::string Swap() { std::string s = buffer.str(); buffer.str(""); return s; }
		};
	}
}
