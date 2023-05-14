#include "CmdLineOpts.h"
#include "common/PlatformUtils.h"
#include "picojson.h"
#include <iomanip>
#include <fstream>
#include <memory>
#ifndef NDEBUG
#include <iostream>
#endif

namespace CmdLine {
    IRegistry::~IRegistry() {
        
    }
    
    IOptionParser::~IOptionParser() {
        
    }
    
	struct RegistryImpl : public IRegistry {
		std::list<IOptionParser*> r;
		picojson::object defaultSettings;
		void Add(IOptionParser& opt) {
#ifndef NDEBUG
            bool conflicts = false;
			for (auto &op : r) {
                if (op->GetShort() == opt.GetShort()) {
                    std::cerr << "conflicting command line switch " << op->GetShort() << "\n";
                }
                if (op->GetLong() == opt.GetLong()) {
                    std::cerr << "conflicting command line switch " << op->GetLong() << "\n";
                }
			}
            assert(conflicts == false && "colliding command line switches");
#endif
			r.emplace_back(&opt);
			auto ds = defaultSettings.find(opt.GetLong().substr(2));
			if (ds != defaultSettings.end()) {
				opt.SetDefault(ds->second.to_str());
			}
		}

		void AddParsersTo(IRegistry& other) {
            if (this == &other) return;
			for (auto &op : r) {
				other.Add(*op);
			}
		}

		const char* Parse(std::list<const char*>& args) {
			auto cpy(args);
			std::list<const char*> positionals;
			while (cpy.size()) {
				for (auto o : r) {
					if (o->Parse(cpy)) goto did_parse;
				}
				if (cpy.front()[0] == '-') {
					return cpy.front();
				} else {
					positionals.emplace_back(std::move(cpy.front()));
					cpy.pop_front();
				}
			did_parse:;
			}
			args = positionals;
			return nullptr;
		}

		void ShowHelp(std::ostream& out, const char *banner) {
			using std::left; using std::setw;
			out << banner;
			for (auto o : r) {
				out << "    " << left << setw(6) << o->GetShort() << setw(16) << o->GetLong() << setw(10) << o->GetLabel() << "    " << o->GetDescription() << "\n";
			}
			out << std::endl;
		}

		RegistryImpl() {
			std::ifstream config{ GetConfigPath() + "/kronos.json" };
			if (config.is_open()) {
				picojson::value p;
				picojson::parse(p, config);
				if (p.is<picojson::object>()) {
					defaultSettings = p.get<picojson::object>();
				}
			}
		}
        
        virtual ~RegistryImpl() {
            
        }
	};

	IRegistry& Registry() {
		static RegistryImpl r;
		return r;
	}

	std::unique_ptr<IRegistry> NewRegistry() {
		return std::make_unique<RegistryImpl>();
	}
}
