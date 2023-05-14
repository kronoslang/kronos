#include <string.h>
#include <sys/stat.h>

int compile_module(const char*, const char*);
int compile_page(const char*, const char*);
int link_page(const char**, int, const char* db = nullptr, const char *auth = nullptr);

struct stat exeStat;

#if !(defined(KDCC_MODULE) || defined(KDCC_PAGE) || defined(KDCC_LINK) || defined(KDCC_UPLOAD))
#define SUBCOMMAND(CMD) strcmpi(argv[1], CMD) == 0
#define KDCC_MODULE SUBCOMMAND("module")
#define KDCC_PAGE SUBCOMMAND("build")
#define KDCC_LINK SUBCOMMAND("link")
#define KDCC_UPLOAD SUBCOMMAND("upload")
#define FIRST_ARG 2
#else 
#define FIRST_ARG 1
#endif

int main(int argn, const char *argv[]) {
	stat(argv[0], &exeStat);
	if (argn < FIRST_ARG) return -1;

#ifdef KDCC_MODULE
	for (;KDCC_MODULE;) {
		if (argn < FIRST_ARG + 2) return -1;
		return compile_module(argv[FIRST_ARG], argv[FIRST_ARG + 1]);
	}
#endif

#ifdef KDCC_PAGE
	for (; KDCC_PAGE;) {
		if (argn < FIRST_ARG + 1) {
			return -1;
		} if (argn < FIRST_ARG + 2) {
			return compile_page(argv[FIRST_ARG], "-");
		} else {
			return compile_page(argv[FIRST_ARG], argv[FIRST_ARG + 1]);
		}
	}
#endif

#ifdef KDCC_LINK
	for (; KDCC_LINK;) {
		if (argn < FIRST_ARG + 1) return -1;
		return link_page(argv + FIRST_ARG, argn - FIRST_ARG);
	}
#endif

#ifdef KDCC_UPLOAD
	for (; KDCC_UPLOAD;) {
		if (argn < FIRST_ARG + 3) return -1;
		return link_page(argv + FIRST_ARG + 2, argn - FIRST_ARG - 2, argv[FIRST_ARG], argv[FIRST_ARG + 1]);
	}
#endif

	return -1;
}
