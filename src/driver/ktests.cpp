#include "common/PlatformUtils.h"
#include "common/bitstream.h"
#include "driver/CmdLineOpts.h"
#include "driver/TestInstrumentation.h"
#include "driver/package.h"
#include "driver/picojson.h"
#include "runtime/inout.h"
#include "network/http.h"
#include "config/system.h"
#include "config/corelib.h"
#include "paf/PAF.h"
#include "CompareTestResultJSON.h"

#include <stdexcept>
#include <iostream>
#include <fstream>
#include <sstream>
#include <list>
#include <regex>
#include <chrono>

using namespace std::string_literals;

std::string Green, Red, RedBG, Yellow, Blue, ResetColor;

#define EXPAND_PARAMS \
	F(demo, D, false, "", "play kseq tests as audio demos") \
	F(dbserver, db, ""s, "<http(s)://url:port>", "couchdb instance for test results") \
	F(dbauth, dba, ""s, "<auth-header>", "authorization header to pass to couchdb") \
	F(dbtable, t, "/ci"s, "<database>", "test result database on server") \
	F(submit, S, false, "", "submit test run to the database.") \
	F(bless, B, false, "", "submit as reference result") \
	F(filter_tests, F, ".*"s, "<regex>", "only run tests that match <regex>") \
	F(output_file, o, "test_run.json"s, "<file>", "Write tests along with results to <file>; use - for stdout") \
	F(package, p, std::string(KRONOS_CORE_LIBRARY_REPOSITORY), "<package>", "Package to test") \
	F(package_version, pv, ""s, "<version>", "Tag for the revision of package to test") \
	F(submodule, M, ".*"s, "<submodule-regex>", "Test only <submodule-regex>") \
	F(verbose, v, false, "", "echo the command lines used for driving the tests") \
	F(build_hash, sha, ""s, "<commit-hash>", "Add this commit hash to the test run information") \
	F(help, h, false, "", "help; display this user guide")

namespace CLOpts {
	using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}

int krepl_main(CmdLine::IRegistry& optionParser, Kronos::IO::IConfiguringHierarchy* io, int argn, const char *carg[]);

extern const char *BuildBranch;
extern const char *BuildTags;
extern const char *BuildRevision;
extern const char *BuildIdentifier;

struct FileManager {
	std::unordered_map<std::string, std::string> audioFileAttachments;
	~FileManager() {
		for (auto &f : audioFileAttachments) {
			remove(f.second.c_str());
		}
	}

	void Attach(const std::string& uid, const std::string& localName) {
		audioFileAttachments.emplace(uid, localName);
	}

	std::string Obtain(const std::string& uid) {
		auto f = audioFileAttachments.find(uid);
		if (f == audioFileAttachments.end()) {
			auto dl = WebRequest("GET", CLOpts::dbserver(), CLOpts::dbtable() + "/" + uid + "/binary");
			if (!dl.Ok()) throw std::runtime_error("While obtaining " + uid + ": " + picojson::value{ dl }.serialize());
			Packages::MakeMultiLevelPath(GetCachePath() + "/test_asset/");
			std::string fileName = GetCachePath() + "/test_asset/" + uid + ".wav";

			audioFileAttachments.emplace(uid, fileName);

			std::ofstream cachedFile{ fileName, std::ios_base::binary };
			cachedFile.write(dl.data.data(), dl.data.size());

			return fileName;
		} else {
			return f->second;
		}
	}
} fm;

using StringSet = std::list<std::string>;

struct {
	StringSet didPass, didFail, unknown;

	std::string Count(const StringSet& tests) {
		switch (tests.size()) {
		case 0: return "no" + ResetColor + " tests";
		case 1: return "1" + ResetColor + " test";
		default:
			return std::to_string(tests.size()) + ResetColor + " tests";
		}
	}

	std::string Count(const StringSet& tests, std::string color) {
		auto verbal = Count(tests);
		if (tests.size()) {
			verbal = color + verbal;
		}
		return verbal;
	}

	int Summarize(std::ostream& os) {
		if (didPass.size() || didFail.size()) {
			auto passPercentage = (didPass.size() * 100) / (didPass.size() + didFail.size());
			os << Green << Count(didPass) << " passed (" << passPercentage << "%),\n"
				<< Red << Count(didFail) << " failed.\n\n";
			for (auto &t : didFail) os << " - " << Red << t << ResetColor << "\n";
			if (unknown.size()) {
				os << "\n" << Count(unknown) << " had a missing reference (" + CLOpts::dbserver() << CLOpts::dbtable() << ")\n";
				for (auto &t : unknown) os << " - " << t << "\n";
			}
			return -(int)didFail.size();
		} else {
			if (unknown.size()) {
				os << "\n" << Count(unknown) << " had a missing reference (" + CLOpts::dbserver() << CLOpts::dbtable() << ")\n";
				for (auto &t : unknown) os << " - " << t << "\n";
			}
			os << "\nDid not complete any tests.\n";
			return -1;
		}
	}

	bool Has(const std::string& test) {
		for (auto &l : { &didPass, &didFail, &unknown }) {
			if (std::find(l->begin(), l->end(), test) != l->end()) return true;
		}
		return false;
	}

	void Fail(const std::string& test) {
		assert(!Has(test));
		didFail.emplace_back(test);
	}

	void Pass(const std::string& test) {
		assert(!Has(test));
		didPass.emplace_back(test);
	}

	void Unknown(const std::string& test) {
		assert(!Has(test));
		unknown.emplace_back(test);
	}

} TestSummary;

static uint64_t AudioFileDistance(const std::string& file1, const std::string& file2) {
	auto a = PAF::AudioFileReader(file1.c_str());
	auto b = PAF::AudioFileReader(file2.c_str());

	std::vector<float> tmp;
	uint64_t distance = 0;
	a->Stream([&b,&tmp,&distance](const float* buffer, int numSamples) {
		if (tmp.size() < numSamples) tmp.resize(numSamples);
		auto common = b(tmp.data(), numSamples);
		int i = 0;
		for (;i < common;++i) {
			float delta = buffer[i] - tmp[i];
			distance += uint64_t(delta * delta * 0xffffffff);
		}
		for (;i < numSamples;++i) {
			float delta = buffer[i];
			distance += uint64_t(delta * delta * 0xffffffff);
		}
		return common;
	});
	b->Stream([&distance](const float *buffer, int numSamples) {
		for (int i = 0;i < numSamples;++i) {
			float delta = buffer[i];
			distance += uint64_t(delta * delta * 0xffffffff);
		}
		return numSamples;
	});
	return (uint64_t)sqrt(distance);
}

static std::string GetSystemName();
std::string GetMachineName();
static std::string RunShell(std::vector<std::string> commandLine, std::string dump = "");

using CaseRunnerTy = picojson::object(*)(const std::string& testFile, const std::string& testCase, const picojson::value&);
static picojson::object EvaluationTest(const std::string& testFile, const std::string& testCase, const picojson::value&);
static picojson::object AudioTest(const std::string& testFile, const std::string& testCase, const picojson::value&);

static picojson::object BatchRunner(const char *scheme, const picojson::object& Tests, CaseRunnerTy CaseRunner);

picojson::object GetRunInfo();
picojson::array SplitSemVer(const std::string& a);

Packages::DefaultClient bbClient;

void ProcessIncludes(picojson::value& val) {
	if (val.is<picojson::object>()) {
		picojson::object& obj{ val.get<picojson::object>() };
		auto inc = obj.find("$include");
		if (inc != obj.end() && inc->second.is<picojson::array>()) {
			auto incpath = inc->second.get<picojson::array>();
			if (incpath.empty()) throw std::runtime_error("Bad include path");
			auto filepath = incpath[0].to_str();

			std::string includeJsonPath = bbClient.Resolve(CLOpts::package(), filepath, CLOpts::package_version());
			std::ifstream includeJsonStream{ includeJsonPath };
			if (includeJsonStream.is_open() == false) 
				throw std::runtime_error("Could not open included '" + includeJsonPath + "'");

			picojson::value includeJson;
			std::string err;
			err = picojson::parse(includeJson, includeJsonStream);
			if (err.size()) {
				throw std::runtime_error("Parse error while reading '" + includeJsonPath + "': " + err);
			}

			for (int i = 1; i < incpath.size(); ++i) {
				auto pseg = incpath[i].to_str();
				if (includeJson.contains(pseg)) {
					// convert from reference to make a copy
					includeJson = (picojson::value)includeJson.get(pseg);
				} else {
					throw std::runtime_error("Included file does not contain '" + incpath[i].to_str() + "'");
				}
			}
			val = includeJson;
		} else {
			for (auto&& kv : obj) {
				ProcessIncludes(kv.second);
			}
		}
	}
}

int main(int argc, const char* arg[]) {
	
	if (IsStdoutTerminal()) {
		// use colors
		Red = "\x1b[91m";
		RedBG = "\x1b[41m";
		Green = "\x1b[92m";
		Yellow = "\x1b[93m";
		Blue = "\x1b[94m";
		ResetColor = "\x1b[m";
	}

	try {
		std::list<const char*> args;
		for (int i(1);i < argc;++i) args.emplace_back(arg[i]);
		if (auto badOpt = CmdLine::Registry().Parse(args)) {
			throw std::invalid_argument("Unknown command line option: "s + badOpt);
		}

		if (CLOpts::help()) {
			CmdLine::Registry().ShowHelp(std::cout,
										 "KTESTS; Kronos "
										 KRONOS_PACKAGE_VERSION
										 " Test Harness\n(c) 2017-" KRONOS_BUILD_YEAR 
										 " Vesa Norilo, University of Arts Helsinki\n\n"
										 "PARAMETERS\n\n");
			return 0;
		}

		if (CLOpts::dbauth().size()) {
			CLOpts::dbauth() = "Authorization: " + CLOpts::dbauth();
		} 

		if (CLOpts::dbserver().empty()) {
			CLOpts::dbserver() = "https://db.kronoslang.io";
		}

		std::clog << "Testing [" << CLOpts::package() << " " << CLOpts::package_version() << "]\n";
		if (CLOpts::package() == KRONOS_CORE_LIBRARY_REPOSITORY) {
			// if we are testing core, use it as the default package
			if (CLOpts::package_version().empty()) {
				// if unspecified, test the canonical version
				CLOpts::package_version = KRONOS_CORE_LIBRARY_VERSION;
			} 
            static std::string coreVersion = "KRONOS_CORE_LIBRARY_VERSION=" + CLOpts::package_version();
			putenv((char*)coreVersion.c_str());
		} else {
			if (CLOpts::package_version().empty()) {
				// test the tip
				CLOpts::package_version = "tip~";
			}
		}

		std::clog << "* Test server " << CLOpts::dbserver() << (CLOpts::dbauth().empty() ? "\n" : "(authenticated)\n");

		if (CLOpts::submit()) {
			// implicit leading /
			if (CLOpts::dbtable().size() && CLOpts::dbtable().front() != '/') {
				CLOpts::dbtable() = "/" + CLOpts::dbtable();
			}
			std::clog << "* Submit " << (CLOpts::bless() ? "reference" : "result") << "\n";
		}

		std::clog << "\n";

		auto testDataFilePath = bbClient.Resolve(CLOpts::package(), "tests.json", CLOpts::package_version());
		std::ifstream testDataStream(testDataFilePath);
		if (!testDataStream.is_open()) throw std::runtime_error("Could not load tests for module [" + CLOpts::package() + " " + CLOpts::package_version() + "]");

		picojson::value testData;
		auto jsonError = picojson::parse(testData, testDataStream);
        testDataStream.close();

		ProcessIncludes(testData);
        
		if (jsonError.size()) {
			throw std::runtime_error(jsonError);
		}
                
		auto ri = GetRunInfo();
		ri["package"] = picojson::array{ CLOpts::package(), SplitSemVer(CLOpts::package_version()) };

		if (getenv("BUILD_COMMIT_HASH")) ri["commit"] = getenv("BUILD_COMMIT_HASH");
		if (CLOpts::build_hash().size()) ri["commit"] = CLOpts::build_hash();

		picojson::object RunResult;
		RunResult["run"] = ri;
		RunResult["type"] = "test_run";
		RunResult["blessed"] = CLOpts::bless();

		// batch run
        if (testData.contains("eval")) {
            RunResult["eval"] = BatchRunner("eval",
				testData.get<picojson::object>()["eval"].get<picojson::object>(), 
				EvaluationTest);
        }
            
        if (testData.contains("audio")) {
            RunResult["audio"] = BatchRunner("audio",
				testData.get<picojson::object>()["audio"].get<picojson::object>(), 
				AudioTest);
        }

		if (CLOpts::demo()) {
			return -1;
		}

		if (CLOpts::submit()) {
			picojson::value submission{ RunResult };
			auto run = submission.serialize();
			auto subReq = WebRequest("POST", CLOpts::dbserver(), CLOpts::dbtable() + "/", 
											  run.data(), run.size(), 
											  { "Content-Type: application/json",
												CLOpts::dbauth() });

			if (!subReq.Ok()) throw std::runtime_error("Error while submitting results to " + subReq.uri + ": " + subReq.Text());

			auto resp = subReq.JSON();
			if (resp.contains("ok")) {
				std::clog << "\n- Results submitted to " << CLOpts::dbserver() << CLOpts::dbtable() << "\n";
				
			} else {
				throw std::runtime_error("Error while submitting results to " + subReq.uri + ": " + resp.get("reason").to_str());
				return -1;
			}
		} else if (CLOpts::output_file() == "-") {
			std::ostream_iterator<char> writer { std::cout };
			picojson::value{ RunResult }.serialize(writer);
			std::cout << std::endl;
        } else {
            std::clog << "\nWriting results to " << CLOpts::output_file() << "... ";
            std::ofstream results(CLOpts::output_file());
			std::ostream_iterator<char> writer{ results };
			picojson::value{ RunResult }.serialize(writer);
            results.close();
            std::clog << "Ok\n";
        }

		std::clog << "\n";
		return TestSummary.Summarize(std::clog);
	} catch (std::exception& e) {
		std::cerr << Red << "* " << e.what() << ResetColor << std::endl; 
		return -1;
	}
}

picojson::object EvaluationTest(const std::string& testFile, const std::string& testCase, const picojson::value&) {
	auto output = RunShell({
		"krepl",
		"Import [" + CLOpts::package() + " " + CLOpts::package_version() + " " + "tests/" + testFile + ".k]",
		"Actions:Output-JSON(String:Concat() Eval(" + testCase + " nil))"
	});

	if (CLOpts::demo()) {
		std::cout << "\n" << output << std::endl;
	}
	
	auto hash = Packages::fnv1a(output.data(), output.size());

	return {
		{ "text", output },
		{ "uid", hash.to_str() }
	};
}

std::string Deduplicate(const std::string& file, const std::string& identifier) {
	std::ifstream fs{ file, std::ios_base::binary };
	if (!fs.is_open()) throw std::runtime_error("'" + file + "' not found");

	char buffer[4096];
	auto hash = Packages::fnv1a();
	while (size_t didRead = (size_t)fs.rdbuf()->sgetn(buffer, 4096)) {
		hash = Packages::fnv1a(buffer, didRead);
	}

	if (CLOpts::submit()) {
		auto docfile = Sxx::http::UrlEncode(CLOpts::dbtable()) + "/" + hash.to_str();
		auto webr = WebRequest("GET", CLOpts::dbserver(), docfile);
		if (!webr.Ok() || !((picojson::value)webr).contains("_id")) {
			std::clog << "\n* Uploading '" << file << "' to test database (" << hash.to_str() << ")\n";
			picojson::object blob{
				{ "type", "binary_blob" },
				{ "identifier", identifier},
				{ "filename", file }
			};
			auto blobStr = picojson::value{ blob }.serialize();

			auto resp = WebRequest("PUT", CLOpts::dbserver(), docfile, blobStr.data(), blobStr.size(), 
								   { CLOpts::dbauth() }).JSON();

			if (!resp.contains("ok")) {
				if (resp.get("error") == "conflict") return hash;
				throw std::runtime_error("Database error while deduplicating " + file + ": " + resp.serialize());
			}

			std::vector<char> fileData((size_t)fs.tellg());
			fs.seekg(0);
			fs.read(fileData.data(), fileData.size());

			picojson::value putr = WebRequest("PUT", CLOpts::dbserver(), docfile + "/binary?rev=" + resp.get("rev").to_str(),
												fileData.data(), fileData.size(),
												{ "Content-Type: audio/x-wav",
												  CLOpts::dbauth()});

			if (!putr.contains("ok")) {
				std::clog << WebRequest("DELETE", CLOpts::dbserver(), docfile + "?rev=" + putr.get("rev").to_str(),
										nullptr,0, { CLOpts::dbauth() })
					.data.data();
				throw std::runtime_error("* Failed to post attachment '" + file + "': " + resp.serialize());
			}
		}
	}
	return hash;
}


picojson::object AudioTest(const std::string& testFile, const std::string& testCase, const picojson::value& testParams) {
    std::stringstream replCmd;

    std::string sequence;
    if (testParams.is<picojson::object>() && testParams.contains("control")) {
        sequence = "Sequence:" + testParams.get("control").to_str();
    } else {
        sequence = "[]";
    }

	static int fileCounter = 0;
	auto audioFile = testFile + "_"  + GetProcessID() + "-" + std::to_string(fileCounter++) + ".wav";
    
	std::vector<std::string> cmdLine{
		"krepl",
		"Import [" + CLOpts::package() + " " + CLOpts::package_version() + " tests/" + testFile + ".k]",
		"Actions:Test( "s + (CLOpts::demo() ? "5" : "2") + " " + testCase + " " + sequence + ")",
		CLOpts::demo() ? "Actions:Sleep(5)" : ""
	};
    
	RunShell(cmdLine, audioFile);

	if (CLOpts::demo()) {
		return { { "audio", true } };
	} else {
		auto fileId = Deduplicate(audioFile, testFile + "." + testCase);
		fm.Attach(fileId, audioFile);
		return {
			{ "audio", true },
			{ "uid", fileId }
		};
	}
}

std::string Diff(const char *scheme, const std::string& pack, const std::string& testName, picojson::object& testData) {
	auto db = Sxx::http::UrlEncode(CLOpts::dbtable()) + "/_design/ci/_view/test_reference";
	const uint64_t permittedAudioDistance = 80000;

	auto getJSON = [&db](const std::string& query) -> picojson::value {
		return WebRequest("GET", CLOpts::dbserver(), db + query);
	};

	auto testId = scheme + ":"s + pack + "." + testName;

	try {

		{
			// try exact match
			picojson::array key{ pack, testName, testData["result"].get("uid") };
			std::string exactQuery = "?key=" + Sxx::http::UrlEncode(picojson::value{ key }.serialize());

			auto match = getJSON(exactQuery);
			if (match.contains("rows") && match.get("rows").get<picojson::array>().size()) {
				// we have an exact match in the database
				testData["status"] = "ok";
				TestSummary.Pass(testId);
				return "";
			}
		}

		{
			// compare with known results
			picojson::array
				range_start{ pack, testName },
				range_end{ pack, testName, picojson::object{} };

			std::string query = "?reduce=false&startkey=" +
				Sxx::http::UrlEncode(picojson::value{ range_start }.serialize()) +
				"&endkey=" + Sxx::http::UrlEncode(picojson::value{ range_end }.serialize());

			picojson::value result = getJSON(query);
			std::stringstream diff;
			if (result.contains("rows")) {
				auto rows = result.get("rows").get<picojson::array>();
				if (rows.size()) {
					if (!strcmp(scheme, "audio")) {
						auto currentFile = fm.Obtain(testData["result"].get("uid").to_str());
						uint64_t distance = 0;
						picojson::object distances;
						for (auto& r : rows) {
							auto uid = r.get("value").get("uid").to_str();
							try {
								auto file = fm.Obtain(uid);
								auto curDist = AudioFileDistance(file, currentFile);
								distances[uid] = (double)curDist;
								diff << "Distance from " << Blue << uid << ResetColor <<": ";
								if (curDist >= permittedAudioDistance) {
									diff << Red;
								} else {
									diff << Green;
								}
								diff << curDist << ResetColor << "\n";
								distance = std::min(distance, curDist);
							} catch (std::exception&) {
								std::clog << Red << "* Could not obtain reference for " << uid << ResetColor << "\n";
							}
						}
						testData["comparisons"] = distances;
						if (distances.size() && distance < permittedAudioDistance) {
							testData["status"] = "ok";
							TestSummary.Pass(testId);
							return diff.str();
						}
					} else {
						auto tested = testData["result"].get("text").to_str();
						diff << "* " << rows.size() << " known good results\n" 
						     << "<<< LOCAL <<<\n" << tested;
						for (auto& r : rows) {
							auto ref = r.get("value").get("text").to_str();
							std::stringstream conflicts;

							picojson::value lj, rj;
							std::string err;
							picojson::parse(lj, tested.cbegin(), tested.cend(), &err);
							if (err.empty()) {
								picojson::parse(rj, ref.cbegin(), ref.cend(), &err);

								if (err.empty()) {
									if (::equal(lj, rj, &conflicts)) {
										testData["status"] = "ok";
										testData["comparisons"] = ref;
										TestSummary.Pass(testId);
										return {};
									} 
								}
							}

							if (err.empty()) {
								diff << "\n=====JSON======\n";
							} else {
								diff << "\n===============\n";
							}

							bool same = true;
							for (int i = 0; i < std::max(ref.size(), tested.size()); ++i) {
								int t = i < tested.size() ? tested[i] : 0;
								int r = i < ref.size() ? ref[i] : 0;
								bool nowSame = t == r;
								if (nowSame != same) {
									if (!nowSame) diff << RedBG;
									else diff << ResetColor;
									same = nowSame;
								}
								diff << (char)r;
							}
							if (!same) diff << ResetColor;
							if (conflicts.str().size()) {
								diff << "\n---------------\n" << conflicts.str();
							}
						}
						diff << "\n>>>  REF  >>>\n";
					}
					testData["status"] = "fail";
					TestSummary.Fail(testId);
					return diff.str();
				}
			}
		}

		TestSummary.Unknown(testId);
		testData["status"] = "new";
		return ( testData["result"].contains("text") ? testData["result"].get("text").to_str() + "\n" : "" );
	} catch (std::exception & e) {
		testData["status"] = "fail";
		testData["diff_exception"] = e.what();
		TestSummary.Fail(testId);
		return e.what();
	}
}

picojson::object BatchRunner(const char* scheme, const picojson::object& Tests, CaseRunnerTy CaseRunner) {
	using namespace std::string_literals;
	std::regex filter{ CLOpts::filter_tests(), std::regex::icase };
	picojson::object Results;
	for (auto &inPackage : Tests) {

		if (!std::regex_search(inPackage.first, std::regex{ CLOpts::submodule() })) {
			continue;
		}

		std::clog << Yellow << "[" << inPackage.first << "]" << ResetColor << "\n";
		
		picojson::object PackageResults;

		if (inPackage.second.is<picojson::object>()) {
			auto& testCases = inPackage.second.get<picojson::object>();
			for (auto &test : testCases) {
				if (std::regex_search(scheme + ":"s + inPackage.first + "/" + test.first, filter)) {
					std::clog << " - " << (test.second.contains("label") ? test.second.get("label").to_str() : test.first) << " ... ";
					auto startTime = std::chrono::steady_clock::now();
					picojson::object testResult;
					auto testCase = "Test:" + test.first;

					if (test.second.contains("expr")) {
						testCase = "{ " + test.second.get("expr").to_str() + " }";
					}

					picojson::object result;

					try {
						result = CaseRunner(inPackage.first, testCase, test.second);
					} catch (std::exception& e) {
						testResult["result"] = picojson::object{ 
							{ "run_exception", e.what() }, 
							{ "uid", "0" } 
						};
					};

					auto endTime = std::chrono::steady_clock::now();

					testResult["result"] = result;
					testResult["duration"] = (double)std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count() / 1000;

					if (!CLOpts::demo()) {
						auto show = Diff(scheme, inPackage.first, test.first, testResult);
						PackageResults.emplace(test.first, testResult);
						
						auto ss = testResult["status"].to_str();
						
						if (ss == "ok") std::clog << Green;
						else if (ss == "fail") std::clog << Red;
						else if (ss == "new") std::clog << Yellow;
						
						std::clog << ss << ResetColor << "\n" << show;

					} else {
						std::clog << "done\n";
					}
				}
			}
		}

		if (PackageResults.size()) Results.emplace(inPackage.first, PackageResults);
	}
	return Results;
};



#ifdef WIN32
#include <Windows.h>
std::string GetSystemName() {
	return
#ifdef _WIN64
		std::string("Win64-")
#else
		std::string("Win32-")
#endif
		+ getenv("PROCESSOR_ARCHITECTURE");
}

std::string GetMachineName() {
	std::vector<wchar_t> name(1024);
	DWORD sz = (DWORD)name.size();
	GetComputerName(name.data(), &sz);
	std::wstring namestr{ name.data(), name.data() + sz };
	return encode_utf8(namestr);
}
#else
#include <sys/utsname.h>
std::string GetSystemName() {
	utsname un;
	if (uname(&un) == 0) {
		return std::string(un.sysname) + "-" + un.machine;
	} else return "unknown";
}

std::string GetMachineName() {
	utsname un;
	if (uname(&un) == 0) {
		return std::string(un.nodename);
	} else return "unknown";
}
#endif

std::string RunShell(std::vector<std::string> commands, std::string dumpFile) {
	if (CLOpts::verbose()) {
		std::clog << "$";
		for (auto& c : commands) {
			std::clog << " " << c;
		}
		std::clog << "\n";
	}


	std::vector<const char*> argv;
	for (auto &c : commands) argv.push_back(c.c_str());

	// redirect pipes
	std::stringstream substdout;
	std::flush(std::cout);
	auto oldcout = std::cout.rdbuf(substdout.rdbuf());

	try {

		auto subCmdLine = CmdLine::NewRegistry();
		static std::unique_ptr<Kronos::IO::IConfiguringHierarchy> io;
		InstrumentedIO* dumpAudio = nullptr;
		
		if (CLOpts::demo()) {
			if (!io) {
				io = Kronos::IO::CreateCompositeIO();
			}
		} else {
			auto iio = InstrumentedIO::Create(88200);
			dumpAudio = iio.get();
			io = std::move(iio);
		}
		if (int code = krepl_main(*subCmdLine, io.get(), (int)argv.size(), argv.data())) {
			throw std::runtime_error("Subtask failed with result " + std::to_string(code));
		}

		if (dumpAudio && dumpFile.size()) {
			if (!dumpAudio->DumpAudio(dumpFile.c_str())) throw std::runtime_error("Test did not produce an audio capture");
		}

	} catch (...) {
		std::flush(std::cout);
		std::cout.rdbuf(oldcout);
		throw;
	}
	std::flush(std::cout);
	std::cout.rdbuf(oldcout);
	return substdout.str();
}

picojson::object GetRunInfo() {
	time_t rawtime;
	struct tm * timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	char buf[128];
	strftime(buf, sizeof(buf), "%FT%TZ", timeinfo);
	std::string isodate = buf;

	return picojson::object{
	{ "date", isodate },
	{ "platform", GetSystemName() },
	{ "agent", GetMachineName() },
	{ "build", picojson::array{ BuildBranch, BuildTags, BuildRevision } }
	};
}

picojson::array SplitSemVer(const std::string& a) {
	picojson::array nums;
	size_t pos = 0, prev = 0;
	for (; a.npos != (pos = a.find('.', prev)); prev = pos + 1) {
		std::string item = a.substr(prev, pos - prev);
		nums.emplace_back((double)strtoul(item.c_str(), nullptr, 10));
	}

	int lastVer = 0;
	if (sscanf(a.data() + prev, "%i", &lastVer) == 1) {
		nums.emplace_back((double)lastVer);
	} else {
		nums.emplace_back(a.data() + prev);
	}

	return nums;
};
