#pragma once

#include "pcoll/treap.h"
#include "pcoll/hamt.h"

#include "runtime/Environment.h"

#include "ReplEntryBuffer.h"
#include "kronos.h"

#include <string>
#include <future>
#include <thread>
#include <unordered_map>
#include <functional>
#include <condition_variable>

#ifndef _MSC_VER
// this should pull kvm into the app
KRONOS_ABI_EXPORT void** link_kvm(void *ptr);
static void** use_kvm() __attribute__((used));
void** use_kvm() {
	return link_kvm(nullptr);
}
#endif

namespace Kronos {
	namespace REPL {
		class Environment;
		namespace JiT {

			using Runtime::Blob;
			using Runtime::BlobView;
			using Runtime::BlobRef;
			using Runtime::ClassCode;
			using Runtime::ClassRef;
			using Runtime::BuildResultFuture;

			struct BuildKey : public std::tuple<int64_t, BuildFlags> {
				template <typename... ARGS> BuildKey(ARGS&&... args) :tuple(std::forward<ARGS>(args)...) {}
				struct Hash {
					size_t operator()(const BuildKey& bk) const noexcept {
						return hash_combine(std::get<0>(bk), (int)std::get<1>(bk));
					}
				};
			};

			using BuildPromise = std::promise<Runtime::ClassRef>;
			using BuildPromiseRef = std::shared_ptr<BuildPromise>;
			using BuildPostProcessor = std::function<void(ClassCode&)>;

			class Compiler : public Runtime::IBuilder {
				std::mutex workQueueLock;
				std::recursive_mutex contextLock;
				Kronos::Context& cx;
				using LGuard = std::lock_guard<std::mutex>;
				std::condition_variable workAvailable;
				GenericGraph evaluatorGraph;
				using LogFormatterTy = std::function<void(Kronos::Context&, const std::string&, std::ostream&)>;
				LogFormatterTy logFormatter;
			
				struct Build {
					BuildPostProcessor postProcessor;
					int64_t priority;
					int64_t closureUid;
					Kronos::BuildFlags flags;
					BuildPromiseRef promise;					

					struct Hash {
						size_t operator()(const Build& b) const {
							return hash_combine(b.priority, b.closureUid, (int)b.flags);
						}
					};

					struct Less {
						bool operator()(const Build& l, const Build &r) const {
							#define CMP(prop) if (l.prop < r.prop) return true; if (r.prop < l.prop) return false;
							CMP(priority) CMP(closureUid) CMP(flags)
							#undef CMP
							return false;
						}
					};
				};

				const Build* parentTask;
				std::unordered_multimap<const Build*, Build> additionalBuilds;
				
				pcoll::treap<Build, Build::Less, Build::Hash> workQueue;
				pcoll::hamt<BuildKey, BuildResultFuture, BuildKey::Hash> buildCache;
				pcoll::hamt<std::string, pcoll::llist<BuildKey>> dependencies;

				void Compile(const Build&);
				BuildResultFuture MakeBuildTask(BuildPostProcessor pp, int64_t priority, int64_t closureUid, int flags, std::function<void(const Build&)> perform);

			public:
				static constexpr int DeterministicBuild = UserFlag1;
				BuildResultFuture operator()(BuildPostProcessor, int64_t priority, int64_t closureUid, int flags) override;
				void AdditionalBuild(const Build& parentTask, int64_t closureUid, int flags);
				explicit Compiler(Context&, const char *vm, LogFormatterTy formatter = {});
				~Compiler();
				void SetLogFormatter(LogFormatterTy lf) { logFormatter = lf; }

                using ChangeCallback = std::function<void(const std::string&, std::int64_t)>;
                std::vector<GenericGraph> Parse(const std::string& fragment, ChangeCallback changeCb = [](const std::string&, std::int64_t) {});

				int64_t NilUID() const { return 0ull; }
				int64_t UID(const Type&);
				void ExportLibraryMetadata(std::ostream& json) {
					std::lock_guard<std::recursive_mutex> lg(contextLock);
					cx.GetLibraryMetadataAsJSON(json);
				}

				void Invalidate(int64_t buildGraphTy, BuildFlags);

				void SetDebugTrace(std::string flt) {
					std::lock_guard<std::recursive_mutex> lg(contextLock);
					cx.SetCompilerDebugTraceFilter(flt.c_str());
				}

				std::string GetSourceFilePosition(const char *token) {
					if (!token) return "";
					std::lock_guard<std::recursive_mutex> lg(contextLock);
					return cx.GetModuleAndLineNumberText(token);
				}
			private:
				std::thread worker;
			};
		}

		class JiTEnvironment : public Runtime::Environment {
		protected:
			JiT::Compiler &JiT;
			GenericGraph replOutput;
			GenericGraph instanceWrapper;
		public:
			JiTEnvironment(IO::IHierarchy* ioParent, JiT::Compiler& c);
            using ChangeCallback = JiT::Compiler::ChangeCallback;
            std::vector<Runtime::OwnedValue> Parse(void*, GenericGraph evaluator, const std::string&, ChangeCallback symChangeCallback = [](const std::string&, std::int64_t) {});
            std::vector<Runtime::OwnedValue> Parse(void* host, const std::string& s, ChangeCallback symChangeCallback = [](const std::string&, std::int64_t) {}) { return Parse(host, replOutput, s, symChangeCallback); }
            std::vector<Runtime::OwnedValue> Parse(const std::string& s, ChangeCallback symChangeCallback = [](const std::string&, std::int64_t) {}) { return Parse(*GetHost(), replOutput, s, symChangeCallback); }
            int64_t ParseToUID(const std::string& code);
			virtual void Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz);
			virtual void ToErr(Kronos::IError&, const std::string& log = "") { }
			Runtime::OwnedValue RunImmediately(void*, int64_t closureTy,
										const void* closureArg, size_t closureSz);
			void ExportLibraryMetadata(std::ostream& s) {
				JiT.ExportLibraryMetadata(s);
			}
			std::int64_t MakeInstance(const std::string& innerCode);
			int64_t Start(int64_t closureType, const void *closureData, size_t closureSz) override;
			void SetCompilerDebugTrace(std::string filter);
		};

		class OstreamEnvironment : public JiTEnvironment {
			std::ostream &out, &err;
		public:
			OstreamEnvironment(IO::IHierarchy* io, JiT::Compiler& c, std::ostream& out, std::ostream& err) :JiTEnvironment(io, c),out(out),err(err) {}
			virtual void ToErr(Kronos::IError&, const std::string& log = "");
			virtual void ToOut(const char* pipe, const char* type, const void* blob, bool newline = false);
		};

		class Console : public OstreamEnvironment {
			EntryBuffer buffer;
			std::int64_t instanceHandle = 0;
			std::int64_t sndClosureTy = 0;
		public:
			Console(IO::IHierarchy* io, JiT::Compiler& c);
			std::string ReadLine();
			void Entry(const std::string&);
			bool HasPartialEntry() { return !buffer.IsComplete(); }
			void Shutdown();
		};

		class CompilerConfigurer : public Kronos::IO::IConfigurationDelegate {
			Kronos::REPL::JiT::Compiler& compiler;
			Kronos::IO::IConfiguringHierarchy* io;
		public:
			CompilerConfigurer(Kronos::REPL::JiT::Compiler& compiler, Kronos::IO::IConfiguringHierarchy* io) :compiler(compiler), io(io) {
				io->AddDelegate(*this);
			}

			~CompilerConfigurer() {
				io->RemoveDelegate(*this);
			}

			void Set(const std::string& key, const std::string& value) {
				compiler.Parse(key + " = " + value);
			}
		};
	}
}
