#include <iostream>
#include <fstream>
#include <sstream>
#include "driver/package.h"
#include "driver/picojson.h"
#include "lithe/grammar/kronos.h"

picojson::object symbols;
Packages::DefaultClient bbClient;
std::unordered_set<std::string> files;
std::vector<std::string> file_queue;


void read_package(std::string path, lithe::node& pack) {
	using namespace lithe::grammar::kronos;
	for (auto &n : pack.children) {
		if (n.strbeg == tag::defn) {
			if (n[1].children.size()) {
				std::string argList;
				for (auto& a : n[0][1].children) {
					if (argList.size()) argList.push_back(' ');
					argList += a.get_string();
				}

				if (n[1].children.size() && n[1].children.back().strbeg == tag::docstring && n[1].children.back().size()) {
					std::string symbol = path + n[0][0].get_string();
					if (symbols.count(symbol) == 0) symbols[symbol] = picojson::object{};
					auto& md{ symbols[symbol].get<picojson::object>() };

					std::string doc;

					for (auto &d : n[1].children.back().children) {
						if (doc.size()) doc.push_back('\n');
						doc += d.get_string();
					}
					md[argList] = doc;
				}
			}
		} else if (n.strbeg == tag::package) {
			read_package(path + n[0].get_string() + ":", n);
		} else if (n.strbeg == tag::import) {
			if (n[0].strend) {
				auto fn = n[0].get_string() + ".k";
				if (files.count(fn)) continue;
				std::clog << " - Transitive import: " << fn << "\n";
				files.emplace(fn);
				file_queue.emplace_back(fn);
			}
		}
	}
}

void read_file(std::string mname, std::string version, std::string file) {
	auto grammar = lithe::grammar::kronos::parser();
	auto filePath = bbClient.Resolve(mname, file, version);
	if (!filePath) {
		std::clog << "Could not open " << file;
		return;
	}
	std::ifstream readFile(filePath);
	if (readFile.is_open() == false) {
		std::clog << "Could not open "  << file;
		return;
	}
	std::stringstream readSource;
	readSource << readFile.rdbuf();
	auto src = readSource.str();
	auto pack = grammar->parse(src);
	if (pack.is_error()) throw std::runtime_error("Parser error while reading " + mname);

	read_package("", pack);
}

int compile_module(const char *mod, const char *path) {

	std::string mname = mod;
	std::string version = mname.substr(mname.find(' ') + 1);
	version.pop_back();
	mname = mname.substr(1, mname.find(' ') - 1);

	std::clog << "[" << mname << " " << version << "] -> " << path << "\n";

	auto moduleManifestPath = bbClient.Resolve(mname, "module.json", version);
	if (moduleManifestPath) {
		std::ifstream moduleManifestStream(moduleManifestPath);
		picojson::value moduleManifest;
		picojson::parse(moduleManifest, moduleManifestStream);
		if (moduleManifest.contains("packages")) {
			auto p = moduleManifest.get("packages").get<picojson::object>();
			for (auto& f : p) {
				file_queue.emplace_back(f.first + ".k");
			}
		}
	} 

	if (bbClient.Resolve(mname, "main.k", version)) {
		file_queue.emplace_back("main.k");
	}

	if (file_queue.empty()) throw std::runtime_error("Did not find any packages in module");

	std::clog << "Indexing files ";
	for (auto& f : file_queue) {
		std::clog << f << " ";
	}
	std::clog << "\n";

	while(file_queue.size()) {
		auto file = file_queue.back();
		file_queue.pop_back();
		try {
			read_file(mname, version, file);

			using tag = picojson::array;
			picojson::array page{ "div" };
			page.emplace_back(tag{ "h1", mname });
			page.emplace_back(tag{ "h3", "Version " + version });

			page.emplace_back(tag{ "h3", "Namespaces" });

			const auto tocIndex = page.size();
			page.emplace_back(tag{ "ol" });
			auto toc = page.back().get<picojson::array>();

			page.emplace_back(tag{ "h2", "Global namespace" });

			std::string prefix = "";

			picojson::object symbol_xr;

			const char* referenceRoot = "Resources/Reference/";

			for (bool root : {true, false}) {
				for (auto& sym : symbols) {
					std::string name = sym.first;

					if (name.find(":Brief") != name.size() - strlen(":Brief")) {
						std::string in_pack;
						bool is_in_pack = (name.find(':') != name.npos);
						if (is_in_pack == root) continue;

						if (is_in_pack) {
							in_pack = name.substr(0, name.find(':'));
							name = name.substr(name.find(':') + 1);
						}

						if (in_pack != prefix) {
							page.emplace_back(tag{ "h2#" + in_pack, in_pack });
							prefix = in_pack;

							auto briefDoc = symbols.find(prefix + ":Brief");
							if (briefDoc != symbols.end()) {
								for (auto& form : briefDoc->second.get<picojson::object>()) {
									page.emplace_back(tag{ "p", form.second });
									toc.emplace_back(
										tag{ "li",
											tag{"div",
												tag{"h4",
												tag{"link",
													prefix,
													"#" + prefix}},
												tag{"p", form.second}} });
								}
							}
						}


						symbol_xr[sym.first] = referenceRoot + mname + " " + version;

						auto& md{ sym.second.get<picojson::object>() };
						for (auto& form : md) {
							page.emplace_back(tag{ "code#" + sym.first, name + "(" + form.first + ")" });
							page.emplace_back(tag{ "p", form.second });
						}
					} 
				}
			}

			page[tocIndex] = toc;

			time_t rawtime;
			time(&rawtime);
			auto ltime = gmtime(&rawtime);

			char buf[128];
			strftime(buf, sizeof(buf), "%FT%TZ", ltime);
			std::string isodate = buf;

			picojson::object data{
				{ "modified",
					picojson::object {
						{ referenceRoot + mname + " " + version, isodate }
					}
				},
				{ "public",
					picojson::object {
						{ referenceRoot + mname + " " + version, page }}
				},
				{"symbols", symbol_xr }
			};

			std::ofstream write{ path };
			if (!write.is_open()) {
				std::cerr << "Can't write " << path << std::endl;
				return -1;
			}
			write << picojson::value{ data };
		} catch (std::exception& e) {
			std::clog << "* Network repository '" << mname << "' not available: " << e.what() << "\n";
			return -1;
		}
	}
	return 0;
}