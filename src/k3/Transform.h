#pragma once

#include "SmallContainer.h"
#include <functional>
#include <cstdint>
#include <list>

#ifndef NDEBUG
#include <iostream>
#endif

namespace K3 {

	/* Transform base classes represent the state of an ongoing graph transformation */
	template <class SOURCE, class RESULT>
	class GraphTransform{
	public:		
		typedef SOURCE source_t;
		typedef RESULT result_t;

		virtual RESULT operate(const SOURCE* src) = 0;
		virtual RESULT operator()(const SOURCE* src) {return operate(src);}

		virtual void QueuePostProcessing(const std::function<RESULT(RESULT)>& step) {assert(0 && "Transform doesn't support post processing");}
	};



	/* Cached Transform will make sure that no node is evaluated twice (in case of diamond) 
	   It can process nodes that implement bool ShouldCacheTransformResult() */

	template <class SRC, class RES> class TentativeResult;

	template <typename SRC, typename RES> class CachedTransformState : public Sml::Map<const SRC*,RES> {
		std::list<std::function<RES(RES)>> postProcessingQueue;
	public:
//		~CachedTransformState() { assert(HasQueuedPostProcessing() == false && "Didn't apply post processing"); }
		
		RES PostProcess(RES tmp) {
			while(postProcessingQueue.size()) 
			{ auto f(postProcessingQueue.back());
				postProcessingQueue.pop_back();
				tmp = f(tmp); 
			}
			return tmp;
		}

		bool HasQueuedPostProcessing() const { return postProcessingQueue.empty() == false; }
		void QueuePostProcessing(const std::function<RES(RES)> &pp) { postProcessingQueue.push_back(pp); } 
		void ClearPostProcessingQueue() { postProcessingQueue.clear(); }
		void Add(const CachedTransformState<SRC,RES>& rhs) {
			rhs.for_each([this](const SRC* key, const RES& value) {
				insert(key,value);
			});
			for(auto pp : rhs.postProcessingQueue) postProcessingQueue.push_back(pp);
		}
	};

	template <class SOURCE, class RESULT, bool CACHE_EVERYTHING = false>
	class CachedTransformBase : public GraphTransform<SOURCE,RESULT> {
		friend class TentativeResult<SOURCE,RESULT>;
#ifndef NDEBUG
		std::unordered_set<const SOURCE*> _dbg_processed;
#endif
	public:
		CachedTransformBase(const CachedTransformBase&) = delete;
		CachedTransformBase& operator=(const CachedTransformBase&) = delete;
		typedef SOURCE source_t;
		typedef RESULT result_t;
		typedef CachedTransformState<SOURCE,RESULT> map_t;
	protected:
		map_t &cache;
		const SOURCE* initial;

		virtual RESULT _operateInsertCache(const SOURCE* src) {
			return cache.insert(std::make_pair(src,this->operate(src))).second;
		}

		RESULT _operateCached(const SOURCE* src) {
			auto entry(cache.find(src));
			if (entry) {
				return entry->second;
			} else {
				return _operateInsertCache(src);
			}
		}
	public:
		const SOURCE* GetRoot() const {return initial;}
		
		RESULT Rebase(const SOURCE* src) {
			initial = src; return Go();
		}

		explicit CachedTransformBase(const SOURCE* root,map_t& c):cache(c),initial(root) { }

		RESULT operator()(const SOURCE* src) {
			if (initial != src && CACHE_EVERYTHING == false && src->MayHaveMultipleDownstreamConnections() == false) {
#ifndef NDEBUG
				if (_dbg_processed.find(src) == _dbg_processed.end( )) {
				}
				_dbg_processed.insert(src);
#endif
				return this->operate(src);
			} else {
				assert(src == initial || src->HasNoDownstreamConnections( ) == false);
				return _operateCached(src);
			}
		}

		virtual RESULT Go() { return cache.PostProcess((*this)(initial)); }
		void QueuePostProcessing(const std::function<RESULT(RESULT)>& pp) { cache.QueuePostProcessing(pp); }
	};

	template <class SOURCE, class RESULT, bool CACHE_EVERYTHING = false>
	class CachedTransform : public CachedTransformBase<SOURCE,RESULT,CACHE_EVERYTHING> {
		CachedTransformState<SOURCE,RESULT> owned;
	public:
		CachedTransformState<SOURCE,RESULT>& GetMap() { return owned; }
		const CachedTransformState<SOURCE,RESULT>& GetMap() const { return owned; }
		typedef SOURCE source_t;
		typedef RESULT result_t;
		CachedTransform(const SOURCE* root):CachedTransformBase<SOURCE,RESULT,CACHE_EVERYTHING>(root,owned) { }
	};

	template <class SOURCE, class RESULT, class BASE = CachedTransform<SOURCE,RESULT>>
	class LambdaTransform : public BASE{
	public:
		typedef std::function<RESULT(const SOURCE*)> function_type;
		typedef SOURCE source_t;
		typedef RESULT result_t;
	private:
		function_type lmbd;
	public:
		LambdaTransform(const SOURCE* root):CachedTransform<SOURCE,RESULT>(root) { }
		LambdaTransform(const SOURCE* root, const function_type& f) :CachedTransform<SOURCE, RESULT>(root), lmbd(f) { }
		void SetTransform(const function_type &f) { lmbd=f; }
		RESULT operate(const SOURCE* n) { return lmbd(n); }
	};

	template <class BASE_TRANSFORM>
	class PartialTransform : public BASE_TRANSFORM {
	public:
		typedef typename BASE_TRANSFORM::source_t source_t;
		typedef typename BASE_TRANSFORM::result_t result_t;
	private:
		Sml::Map<const source_t*, result_t> externalSpecification;
	public:
		virtual result_t FinalizeSpecifiedNode(result_t specified) { return specified; }
		PartialTransform(const source_t* root):BASE_TRANSFORM(root) { }
		result_t operator()(const source_t* c) { return operateSpecified(c); }
		void Specify(source_t* src, result_t r) { externalSpecification.insert(std::make_pair(src,r)); }
		result_t operateSpecified(const source_t* src) {
			auto entry(externalSpecification.find(src));
			if (entry) return FinalizeSpecifiedNode(entry->second);
			else return this->operate(src);
		}
	};

	/* 
		CachedTransformNode

		A node that keeps track of global nodes referring to it
		tracking is not accurate if graph sharing occurs.

		However, a node always has *at least* this many downstream
		nodes in any subgraph.

		the counter should only be used to optimizations such as the ones in
		CachedTransform and not relied upon for accurate downstream counts */

	template <class BASE>
	class CachedTransformNode : public BASE {
	protected:
		mutable uint32_t globalDownstreamCount;
		uint32_t GetGlobalDownstreamCount() const { return globalDownstreamCount; }
	public:
		bool HasNoDownstreamConnections( ) const { return globalDownstreamCount == 0; }
		bool MayHaveMultipleDownstreamConnections() const { return globalDownstreamCount > 1;}
		void HasInvisibleConnections() const { globalDownstreamCount+=2; }
		virtual bool MayHaveRecursion() const { return false; }
		CachedTransformNode():globalDownstreamCount(0) { }
		CachedTransformNode(const CachedTransformNode<BASE>& src) :BASE(src), globalDownstreamCount(0) { }
	};
	
	/* Visitor pattern for CachedTransformNodes */
	enum FlowControl{
		Continue,
		AbortThis,
		AbortAll
	};

	template <class VISITOR, class NODE>
	FlowControl ForGraphSub(NODE* root, VISITOR& vs, Sml::Set<NODE*> &visited) {
		if (visited.find(root) && vs.Recursive(root)) return Continue;
		else visited.insert(root);

		auto flow(vs.Visit(root));
		if (flow == Continue) {
			for(unsigned int i(0);i<root->GetNumCons();++i) {
				if (ForGraphSub((NODE*)root->GetCon(i),vs,visited) == AbortAll)
					return AbortAll;
			}
			vs.Back(root);
		}
		return flow;
	}

	template <class VISITOR, class NODE> VISITOR& ForGraph(NODE* root, VISITOR& visitor) {
		VISITOR& vs((VISITOR&)visitor);
		Sml::Set<NODE*> tmp;
		ForGraphSub(root,vs,tmp);
		return vs;
	}
};