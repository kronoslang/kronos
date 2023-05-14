#include "config/system.h"
#include "config/corelib.h"
#include "driver/LanguageServer.h"

#include <iostream>
#include <sstream>
#include <thread>
#include "picojson.h"


int main(int argn, const char* carg[]) {
	using namespace Kronos;

    const char *repo = getenv("KRONOS_CORE_REPOSITORY");
    const char *repoVersion = getenv("KRONOS_CORE_REPOSITORY_VERSION");

    try {
        auto langServer = Kronos::LanguageServer::Make(
			repo ? repo : KRONOS_CORE_LIBRARY_REPOSITORY,
			repoVersion ? repoVersion : KRONOS_CORE_LIBRARY_VERSION);

		int retryCounter = 0;
		while (std::cin.good()) {
            try {
                auto in = JsonRPC::Get(std::cin);

				if (!std::cin.good()) {
					// It may be caused by redirecting the stdin, as in attaching a debugger
					if (retryCounter++ > 3) {
						std::clog << "Language Server terminated: broken pipe.";
						exit(0);
					} else {
						std::this_thread::sleep_for(std::chrono::milliseconds(200));
						std::clog << "Retrying stdin (" << retryCounter << ")\n";
						std::cin.clear();
					}
				} else {
					retryCounter = 0;
				}

                if (in.is<picojson::object>()) {
                    auto out = (*langServer)(in);			
					if (out.is<picojson::object>()) {
						JsonRPC::Put(std::cout, out);
					}
                } 
            } catch(std::exception& e) {
                std::cerr << "* " << e.what() << "\n";
            }

			picojson::value outGoing;
			if (langServer->GetPendingMessage(outGoing)) {
				JsonRPC::Put(std::cout, outGoing);
			}
		}      
	} catch (std::range_error& e) {
		std::cerr << "* " << e.what() << " *" << std::endl;
		std::cerr << "Try '" << carg[0] << " -h' for a list of parameters\n";
		return -3;
	} catch (std::exception& e) {
		std::cerr << "* Runtime error: " << e.what() << " *" << std::endl;
		return -1;
	}
	return 0;
}
