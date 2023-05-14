#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <string.h>

static int
get_kinfo_proc(const pid_t pid, struct kinfo_proc *proc)
{
    int name[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    size_t size;
    
    size = sizeof(struct kinfo_proc);
    
    /* We use sysctl here instead of psi_sysctl because we know the size of
     * the destination already so we don't need to malloc anything. */
    if (sysctl((int*)name, 4, proc, &size, NULL, 0) == -1) {
        return -1;
    }
    
    /* sysctl stores 0 in the size if we can't find the process information */
    if (size == 0) {
        return -1;
    }
    
    return 0;
}

extern "C" pid_t getpid();

static bool MaybeXcodeDebugger(void)
// Returns true if the current process is being debugged (either
// running under the debugger or has a debugger attached post facto).
{
    kinfo_proc proc;
    for(auto p = getpid();p;) {
        get_kinfo_proc(p, &proc);
        p = proc.kp_eproc.e_ppid;
        if (strcmp(proc.kp_proc.p_comm, "Xcode") == 0) return true;
    }
    return false;
}
#else
static bool MaybeXcodeDebugger(void) {
    return false;
}
#endif

#include <iostream>
#include "config/system.h"

#if HAVE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "ReplEntryBuffer.h"

namespace Kronos {
	namespace REPL {
		void EntryBuffer::Process(const std::string& line) {
			int i(0);
			for(;;) {
			ModeSwitch:
				switch(mode) {
				case Syntax:
					for(; i < line.size();) {
						auto c = line[i++];
						buffer.put(c);
						switch(c) {
						case '(': parens++; break;
						case ')': parens--; break;
						case '[': squares++; break;
						case ']': squares--; break;
						case '{': braces++; break;
						case '}': braces--; break;
						case '\"': mode = Quoted; goto ModeSwitch;
						case ';': mode = Commented; goto ModeSwitch;
						default:break;
						}
					}
					return;
				case Quoted:
					for(; i < line.size();) {
						auto c = line[i++];
						buffer.put(c);
						switch(c) {
						case '\\':
							if(i < line.size()) buffer.put(line[i++]);
							break;
						case '\"': mode = Syntax; goto ModeSwitch;
						default: break;
						}
					}
					return;
				case Commented:
					buffer.write(line.data() + i, line.size() - i);
					mode = Syntax;
					return;
				}
			}
			buffer << line;
		}

		bool EntryBuffer::IsComplete() {
			return parens < 1 && squares < 1 && braces < 1 && mode == Syntax;
		}

		void EntryBuffer::Clear() {
			buffer.str("");
		}
        
#if HAVE_READLINE 
		std::string EntryBuffer::ReadLine(const std::string& prefix) {
			std::string pf = prefix + std::string(IsComplete() ? "> " : "~ ");
            if (!MaybeXcodeDebugger()) {
                const char *line = readline(pf.c_str());
                if (!line) return "";
                if(line[0]) add_history(line);
                return std::string(line);
            } else {
                std::string line;
                if(IsComplete()) {
                    std::cout << prefix << "> ";
                } else {
                    std::cout << prefix << "~ ";
                }
                std::getline(std::cin, line);
                return line;
            }
		}
#else
		std::string EntryBuffer::ReadLine(const std::string& prefix) {
			std::string line;
			if(IsComplete()) {
				std::cout << prefix << "> ";
			} else {
				std::cout << prefix << "~ ";
			}
			std::getline(std::cin, line);
			return line;
		}
#endif
	}
}
