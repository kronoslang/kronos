#include "MetaDriver.h"
#include <iostream>
#include <sstream>

namespace Kronos {
	const CompileServer::BuildResult& MetaSequencer::Build(Kronos::BuildFlags flags, const Type& closure, GenericGraph runExpr, BuildCache& buildCache, IO::TimestampTy deadline) {
		auto f = buildCache.find(closure);
		if (f == buildCache.end()) {
			f = buildCache.emplace(closure, 
					csrv.Queue(io, CurrentTimestamp() + deadline, runExpr, closure, flags)).first;
		}
		return f->second.get();
	}

	void MetaSequencer::Invalidate(StringSet changed, BuildCache& bc, std::mutex& lock) {
		std::vector<std::tuple<Type,Closure,std::int64_t>> rebuildQueue;
		std::unique_lock<std::mutex> guard(lock);
		for (auto ci(bc.begin()); ci != bc.end();) {
			auto job = ci->second;
			if (job.valid()) {
				auto br = job.get();
				if (br.deps) {
					if (changed.Intersects(br.deps)) {
						for (auto d : dynamicInstances) {
							auto i = instances.find(d);
							if (i != instances.end() && i->second.con.i && i->second.con.i.TypeOf() == br.c) {
								rebuildQueue.emplace_back(ci->first, std::move(i->second), d);
							}
						}
						bc.erase(ci++);
						continue;
					}
				}
			}
			++ci;
		}
		guard.unlock();
		for (auto &b : rebuildQueue) {
			auto &fn(std::get<0>(b));
			auto &closure(std::get<1>(b));
			auto id(std::get<2>(b));
			if (closure.con.i) {
				Variant newFn(fn.GetFirst(),
							  closure.memory + closure.con.i.TypeOf().SizeOfInstance());
				StopInstance(id);
				StartInstance(newFn, id);
			}
		}
		std::lock_guard<std::recursive_mutex> g(GetMutex());
		rebuildQueue.clear();
	}

	void MetaSequencer::Invalidate(StringSet changed) {
		Invalidate(changed, instBuildCache, instBuildCacheLock);
		Invalidate(changed, evalBuildCache, evalBuildCacheLock);
	}

	CompileServer::BuildResult MetaSequencer::BuildEval(const Type& closure, IO::TimestampTy deadline) {
		std::lock_guard<std::mutex> l(evalBuildCacheLock);
		return Build(OmitReactiveDrivers, closure, evalExpr, evalBuildCache, deadline);
	}

	CompileServer::BuildResult MetaSequencer::BuildInst(const Type& closure, IO::TimestampTy deadline) {
		std::lock_guard<std::mutex> l(instBuildCacheLock);
		return Build(OmitEvaluate, closure, runActionExpr, instBuildCache, deadline);
	}

	void CompileServer::Stop() {
		shutdown = true;
        if (worker.joinable()) {
            workAdded.notify_all();
            worker.join();
        }
    }

	CompileServer::~CompileServer() {
		Stop();
	}

	CompileServer::CompileServer(Context context) :cx(context), shutdown(false) {
		worker = std::thread([this]() {
			WorkUnit wu;
            std::unique_lock<std::recursive_mutex> ul(queueLock);
			while (!shutdown) {
                if (!ul.owns_lock()) ul.lock();
                workAdded.wait(ul);
                while (true) {
                    if (queue.empty()) {
                        if (ul.owns_lock()) ul.unlock();
                        break;
                    }
                    wu = std::move(queue.front());
                    queue.pop_front();
                    if (ul.owns_lock()) ul.unlock();
                    BuildResult br;
                    try {
    #ifndef NDEBUG
                        std::clog << "<< Compiling @ " << wu.deadline << ">>\n";
    #endif
                        auto spec = cx.Specialize(wu.expr, wu.fn, nullptr, 0);
                        br.c = cx.Make("llvm", spec, wu.flags, "host", "host", "host");
                        br.connector = wu.io->Associate(br.c);
                    } catch (Kronos::IError& ie) {
                        std::stringstream error;
                        error << "<err code='" << ie.GetErrorCode() << "' msg='" << ie.GetErrorMessage() << "' at='" << ie.GetSourceFilePosition() << "'>";
                        try {
                            auto spec = cx.Specialize(wu.expr, wu.fn, &error, 0);
                            cx.Make("llvm", spec, wu.flags, "host", "host", "host");
                        } catch (...) {}
                        error << "</err>";
                        br.error_xml = error.str();
                    }
					br.deps = cx.GetResolutionTrace();
					wu.p.set_value(br);
                }
			}
		});
	}
    
    void CompileServer::Parse(const char* expr, std::function<void(const char*,GenericGraph)> immediateHandler) {
        std::unique_lock<std::recursive_mutex> lg(queueLock);
        cx.Parse(expr, true, [&](const char* sym, GenericGraph g){
            lg.unlock();
            immediateHandler(sym,g);
            lg.lock();
        });
    }
    
	std::shared_future<CompileServer::BuildResult> CompileServer::Queue(IO::IDriver& io, IO::TimestampTy deadline, GenericGraph expr, const Type& fn, BuildFlags flags) {
		std::lock_guard<std::recursive_mutex> lg(queueLock);
#ifndef NDEBUG
		std::clog << "<< Queuing a compile job for " << deadline << ">>\n";
#endif
		assert(!shutdown);
		if (queue.empty() || deadline >= queue.back().deadline) {
			queue.emplace_back(io, deadline, expr, fn, flags);
            workAdded.notify_one();
			return queue.back().p.get_future().share();
		}

		WorkUnit wu(io, deadline, expr, fn, flags);
		auto f = queue.insert(std::lower_bound(queue.begin(), queue.end(), wu),
				      std::move(wu))->p.get_future().share();
        workAdded.notify_one();
        return f;
	}
}
