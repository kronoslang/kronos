#include "runtime/kronosrtxx.h"
#include "runtime/scheduler.h"
#include "ReplEnvironment.h"
#include "llvm/Support/DynamicLibrary.h"
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#define COMPILER_LOGGING 0
#define CACHE_LOGGING 0

KRONOS_ABI_EXPORT void** link_kvm(void *ptr);

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx);

namespace Kronos {
	namespace REPL {
		namespace JiT {
            std::vector<GenericGraph> Compiler::Parse(const std::string& fragment, ChangeCallback changeCallback) {
				std::vector<GenericGraph> results;
				{
					std::lock_guard<std::recursive_mutex> lg(contextLock);
					cx.Parse(fragment.c_str(), true, [&results](const char *sym, GenericGraph imm) {
						results.emplace_back(std::move(imm));
					});
				}
				auto str = cx.DrainRecentSymbolChanges();
				char *ptr = (char*)str->data();
				auto token = ptr;

				while (*ptr) {
					if (*ptr++ == ' ') {
						auto sym = std::make_unique<std::string>(token, ptr - 1);
                        changeCallback(*sym, 0);
						dependencies.update_in(*sym, [&](auto bks) -> pcoll::optional<pcoll::llist<BuildKey>> {
							if (bks.has_value) {
								pcoll::llist<BuildKey> nbks;
								for (auto &bk : *bks) {
									bool didRemove = false;

									buildCache.update_in(bk, [&didRemove](const auto& val) {
										didRemove = val.has_value;
										return pcoll::none{};
									});

									if (didRemove) {
#if CHANGE_LOGGING
                                        std::clog << "* Invalidated " << bk << " (" << sym << ")\n";
#endif
										changeCallback(*sym, std::get<std::int64_t>(bk));
									} else {
										nbks.push_front(bk);
									}
								}
								bks = nbks;
							}
							return bks;
						});
						token = ptr;
					}
				}

		
				return results;
			}

			static const char *strchr0(const char *ptr, char delimiter) {
				while (*ptr && *ptr != delimiter) ++ptr;
				return ptr;
			}
			
			void Compiler::Compile(const Build& buildTask) {
				auto closureType = buildTask.closureUid
					? cx.TypeFromUID(buildTask.closureUid)
					: GetNil();

                // having these on stack triggers a strange
                // crash on MSVC. Memory corruption?
                auto token = std::make_unique<std::string>();
                auto traceStr = std::make_unique<std::string>();

                try {

					parentTask = &buildTask;
#if COMPILER_LOGGING
                    std::clog << "<< Compiling " << std::hex << buildTask.closureUid << " >>" << std::dec;
#endif
					auto typed = cx.Specialize(evaluatorGraph, closureType, nullptr, 0);

					auto code = std::make_shared<Runtime::ClassCode>(cx.Make("llvm", typed, buildTask.flags));

					// call environment to finalize
					buildTask.postProcessor(*code);

                    // save symbol trace before dependent builds trash it
                    *traceStr = cx.GetResolutionTrace()->c_str();

                    // execute any pending deterministic builds
					auto builds = additionalBuilds.equal_range(&buildTask);
					for (auto i = builds.first; i != builds.second; ++i) {
						Compile(i->second);
					}
					additionalBuilds.erase(&buildTask);

#if COMPILER_LOGGING
					std::clog << "<< Fulfilled " << std::hex << buildTask.closureUid << " >>\n" << std::dec;
#endif
					buildTask.promise->set_value(code);
				} catch (Kronos::IProgramError&) {
					std::stringstream log;
					try {
						cx.Specialize(evaluatorGraph, closureType, &log, 2);
						buildTask.promise->set_exception(std::current_exception());
					} catch (Kronos::IProgramError& pe) {
						if (auto sfp = pe.GetSourceFilePosition()) {
							std::string attachSfp = " in " + cx.GetModuleAndLineNumberText(sfp);
							std::string caret = cx.ShowModuleLine(sfp);

							TypeError npe(pe.GetErrorCode(), pe.GetErrorMessage(),
										  pe.GetSourceFilePosition()); 
							std::string sendLog = log.str();
                            
							if (logFormatter) {
								log.str("");
								logFormatter(cx, sendLog, log);
								sendLog = log.str();
							}

							npe.AttachErrorLog(sendLog.c_str());
							buildTask.promise->set_exception(std::make_exception_ptr(npe));
						} else {
							buildTask.promise->set_exception(std::current_exception());
						}
					}
				} catch (...) {
					buildTask.promise->set_exception(std::current_exception());
				}
				// mark symbol dependencies for invalidation
				auto buildKey = BuildKey{
					buildTask.closureUid,
					buildTask.flags
				};

				std::istringstream tokenStream(*traceStr);
				for (;std::getline(tokenStream, *token, ' ');) {
					dependencies.update_in(*token, [&](pcoll::optional<pcoll::llist<BuildKey>> deps) {
						pcoll::llist<BuildKey> ds;
#if CACHE_LOGGING
                        std::clog << "+ " << std::get<0>(buildKey) << " depends on " << *token << "\n";
#endif
						if (deps.has_value) ds = *deps;
						return ds.push_front(buildKey);
					});
				}
			}

			void Compiler::Invalidate(std::int64_t buildTy, BuildFlags flags) {
				std::lock_guard<std::recursive_mutex> lg(contextLock);
				buildCache.update_in({ buildTy, flags }, [](auto v) {
					return pcoll::none{};
				});
			}

			void Compiler::AdditionalBuild(const Build& parentTask, int64_t closureUid, int flags) {
#if COMPILER_LOGGING
				std::clog << "<< Additionally " << closureUid << " >>\n";
#endif
				MakeBuildTask(parentTask.postProcessor, parentTask.priority + 1, closureUid, flags, [this, &parentTask](const Build& build) {
					additionalBuilds.emplace(&parentTask, build);
				});
			}

			Compiler::Compiler(Kronos::Context& cx, const char* vm, LogFormatterTy formatter) :cx(cx), logFormatter(formatter), parentTask(nullptr) {
                cx.ImportFile(vm);
				evaluatorGraph = Parse("Eval(arg nil)")[0];

				cx.RegisterSpecializationCallback("kvm_anticipate_after", [](void *ptr, KRONOS_INT, const IType* tyPtr, int64_t tyUid) {
					auto self = (Compiler*)ptr;
					
					if (tyUid == 0) return;

					if (true || (self->parentTask->flags & DeterministicBuild) == DeterministicBuild) {
						self->AdditionalBuild(*self->parentTask, tyUid, self->parentTask->flags);
					} else {
						(*self)(self->parentTask->postProcessor, self->parentTask->priority + 1, tyUid, self->parentTask->flags);
					}
				}, this);

				cx.RegisterSpecializationCallback("kvm_anticipate_start", [](void* ptr, KRONOS_INT, const IType* tyPtr, int64_t tyUid) {
					if (tyUid == 0) return;
					auto self = (Compiler*)ptr;
					int flags = (int)self->parentTask->flags;
					flags |= OmitEvaluate;
					flags &= ~OmitReactiveDrivers;
					self->AdditionalBuild(*self->parentTask, tyUid, flags);
				}, this);

				worker = std::thread([this]() mutable {
					for (;;) {
						Build buildTask;
						std::unique_lock<std::mutex> ul{ workQueueLock };
						while (!workQueue.try_pop_front(buildTask)) {
							workAvailable.wait(ul);
						}
						ul.unlock();

						if (buildTask.closureUid == 0) {
							return;
						}
						std::lock_guard<std::recursive_mutex> lg{ contextLock };
						Compile(buildTask);
					}
				});
			}

			int64_t Compiler::UID(const Type& t) {
				std::lock_guard<std::recursive_mutex> lg(contextLock);
				return cx.UIDFromType(t);
			}

			BuildResultFuture Compiler::MakeBuildTask(BuildPostProcessor pp, int64_t priority, int64_t closureUid, int flags, std::function<void(const Build&)> perform) {
				auto buildKey = BuildKey{
					closureUid,
					(BuildFlags)flags
				};

				bool needToCompile;
				BuildResultFuture future;

				auto buildPromise = std::make_shared<BuildPromise>();

				buildCache.update_in(buildKey, [&](const pcoll::optional<BuildResultFuture>& prior) {
					needToCompile = !prior.has_value;
				
					if (!needToCompile) {
						return future = *prior;
					} else {
						return future = buildPromise->get_future().share();
					}
				});

				if (needToCompile) {
					Build newBuild{
						std::move(pp),
						priority,
						closureUid,
						(BuildFlags)flags,
						buildPromise,
					};
					perform(newBuild);
				}
				return future;
			}

			BuildResultFuture Compiler::operator()(BuildPostProcessor pp, int64_t priority, int64_t closureUid, int flags) {				
				return MakeBuildTask(pp, priority, closureUid, flags, [this](const Build& newBuild) {
					workQueue.insert_into(newBuild);
					LGuard lg{ workQueueLock };
					workAvailable.notify_one();
				});
			}

			Compiler::~Compiler() {
				if (worker.joinable()) {
					workQueue.insert_into(Build{
						BuildPostProcessor(),
						0,
						0,
						BuildFlags::Default,
						std::make_shared<BuildPromise>()
					});
					{
						LGuard lg{ workQueueLock };
						workAvailable.notify_one();
					}
					worker.join();
				}
			}
		}

		JiTEnvironment::JiTEnvironment(IO::IHierarchy* parent, JiT::Compiler& c) :JiT(c), Runtime::Environment(parent, c, 0, 8) {
			std::stringstream replMagic;
			replMagic << ":VM:Output(arg { :Audio:Outputs }) :VM:Adapter({ :Audio:Outputs } arg)";
			replOutput = JiT.Parse(replMagic.str())[0];
			instanceWrapper = JiT.Parse(replMagic.str())[1];
		}



        std::vector<Runtime::OwnedValue> JiTEnvironment::Parse(void* host, GenericGraph evaluator, const std::string& fragment, JiT::Compiler::ChangeCallback symCb) {
			std::vector<Runtime::OwnedValue> result;

			for (auto &imm : JiT.Parse(fragment, symCb)) {
				auto statement = evaluator.Empty()? imm : evaluator.Compose(imm);
				result.emplace_back(RunImmediately(host, JiT.UID(statement.AsType()), nullptr, 0));
			};
			return result;
		}

		void JiTEnvironment::SetCompilerDebugTrace(std::string flt) {
			JiT.SetDebugTrace(flt);
		}

		std::int64_t JiTEnvironment::MakeInstance(const std::string& code) {
			auto parse = JiT.Parse(code);
			if (parse.empty()) return 0;
			auto expr = parse[0];
			return JiT.UID(instanceWrapper.Compose(expr).AsType());
		}
        
        std::int64_t JiTEnvironment::ParseToUID(const std::string& code) {
            return JiT.UID(JiT.Parse(code)[0].AsType());
        }

		Console::Console(IO::IHierarchy* io, JiT::Compiler& c) :OstreamEnvironment(io, c, std::cout, std::cerr) {
			std::stringstream sndMagic;
			sndMagic << ":VM:Adapter({ :Audio:Outputs } { :snd })";
			JiT.Parse("snd = nil");
			sndClosureTy = ParseToUID(sndMagic.str());
			instanceHandle = Start(sndClosureTy, 0, 0);
		}


		void Console::Entry(const std::string& line) {
			buffer.Process(line);
			if (buffer.IsComplete()) {
				try {
					bool recompileSnd = false;
					std::vector<std::string> validateTypes;
                    Parse(*GetHost(), replOutput, buffer.Swap(), [&](const std::string& sym, std::int64_t closureTy) {
						if (closureTy == sndClosureTy || sym == ":snd") {
							recompileSnd = true;
						} else {
							if (line.find("Import") == line.npos) {
								validateTypes.emplace_back(sym);
							}
						}
					});

					if (validateTypes.size()) {
						std::string expr = "Require(";
						for (auto& v : validateTypes) {
							expr += v; expr.push_back(' ');
						}
						Parse(*GetHost(), replOutput, expr + "\"ok\")");
					}

					if (recompileSnd) {
						if (instanceHandle) {
							Stop(instanceHandle);
						}
						instanceHandle = Start(sndClosureTy, 0, 0);
					}
				} catch (Kronos::IProgramError& pe) {
					auto log = pe.GetErrorLog();
					ToErr(pe, log ? log : "");
				}  catch (Kronos::IError& ie) {
					ToErr(ie);
				} 
			}
		}

		void Console::Shutdown() {
			if (instanceHandle) {
				Stop(instanceHandle);
			}

			OstreamEnvironment::Shutdown();
		}

		Runtime::OwnedValue JiTEnvironment::RunImmediately(void* useHost, int64_t closureTy, 
							  const void* closureData, size_t closureSz) {
			try {
				auto& class_ = *JiT([this](Runtime::ClassCode& cc) mutable {
					this->Finalize(cc);
				}, 0, closureTy, OmitReactiveDrivers).get().get();
				auto instanceMemory = alloca((size_t)class_->get_size());
				Runtime::OwnedValue result;
				result.data.resize((size_t)class_->result_type_size);
				auto dur = Now();
				auto usec = Runtime::MicroSecTy(dur);
				Runtime::VirtualTimePoint() = Runtime::TimePointTy(usec);
				Runtime::ScriptContext sc(Runtime::Frozen);
				Connect(class_, instanceMemory, {});
				class_->construct(instanceMemory, closureData);
				class_->eval(instanceMemory, closureData, result.data.data());
				result.descriptor = class_->result_type_descriptor;
				return result;
			} catch (...) {
				JiT.Invalidate(closureTy, OmitReactiveDrivers);
				throw;
			}
		}

		void JiTEnvironment::Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) {
			try {
				Runtime::Environment::Run(timestamp, closureTy, closureArg, closureSz);
			} catch (Kronos::IProgramError& pe) {
				auto log = pe.GetErrorLog();
				ToErr(pe, log ? log : "");
			} catch (Kronos::IError &ie) {
				ToErr(ie);
			}
		}

		int64_t JiTEnvironment::Start(int64_t closureType, const void *closureData, size_t closureSz) {
			try {
				return Runtime::Environment::Start(closureType, closureData, closureSz);
			} catch (Kronos::IProgramError& pe) {
				JiT.Invalidate(closureType, OmitEvaluate);
				auto log = pe.GetErrorLog();
				ToErr(pe, log ? log : "");
			} catch (Kronos::IError &ie) {
				JiT.Invalidate(closureType, OmitEvaluate);
				ToErr(ie);
			}
			return 0;
		}


		std::string Console::ReadLine() {
			return buffer.ReadLine();
		}

		void OstreamEnvironment::ToOut(const char* /*pipe*/, const char* type, const void* blob, bool newline) {
			out << Runtime::Value{ type, blob };
			if(newline) out << std::endl;
			else out.flush();
		}

		void OstreamEnvironment::ToErr(Kronos::IError& ie, const std::string& log) {
			err << "\n* E" << ie.GetErrorCode() << ": " << ie.GetErrorMessage() << "\n";
			err << std::endl;
			err << log;
			err << std::endl;
		}
	}
}

