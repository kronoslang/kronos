#pragma once
#include <stack>
#include <functional>

#pragma warning(disable : 4584)

namespace K3 {

	template <typename TResult> class CSTransform;

	template <typename TResult> 
	class ICSTransformNode {
		friend class CSTransform<TResult>;
	public:
		virtual void Schedule(CSTransform<TResult>& t) const = 0;
		virtual void Process(CSTransform<TResult>&) const = 0;

	};

	template <typename TResult, typename TBase>
	class CSTransformNode : public ICSTransformNode<TResult>, public TBase { 
	public:
		void Schedule(CSTransform<TResult>& t) const override {
			t.Push(this, &ICSTransformNode<TResult>::Process);
			for (int i = 0; i < this->GetNumCons(); ++i) {
				const TBase* up = this->GetUp(i);
				auto n = (const CSTransformNode<TResult, TBase>*)up;
				t.Push(n, &ICSTransformNode<TResult>::Schedule);
			}
		}

		void Process(CSTransform<TResult>& t) const override = 0;
	};

	template <typename TResult>
	class CSTransform {
	public:
		using WorkFuncTy = decltype(&ICSTransformNode<TResult>::Schedule);

		static auto ConstructWorkUnit(const ICSTransformNode<TResult>* object, WorkFuncTy func) {
			return std::bind(func, object, std::placeholders::_1);
		}

		using WorkUnit = decltype(ConstructWorkUnit(nullptr, std::declval<WorkFuncTy>()));

		template <typename TFunc>
		void Push(const ICSTransformNode<TResult>* object, TFunc func) {
			workStack.push(ConstructWorkUnit(object, func));
		}

		void Push(const ICSTransformNode<TResult>* object) {
			Push(object, &ICSTransformNode<TResult>::Schedule);
		}

		void Do() {
			while (workStack.empty() == false) {
				auto wu{ workStack.top() };
				workStack.pop();
				wu(*this);
			}
		}
	private:
		std::stack<WorkUnit> workStack;
		std::stack<TResult> resultStack;
	};
}