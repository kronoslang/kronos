#pragma once

#include <vector>
#include <unordered_map>

std::unordered_map<void*,void*>* __dynscp_tls();
void __dynscp_tls_dtor(void *key);

template <class T> class DynamicBinding;

template <class T>
class DynamicScope{
	template <class U> friend class DynamicBinding;
	const T init;
	std::vector<T>& GetStack()
	{
		auto tls_hash(__dynscp_tls());
		auto stack(tls_hash->find(this));
		if (stack==tls_hash->end())
		{
			auto tmp(new std::vector<T>);tmp->push_back(init);
			stack = tls_hash->insert(make_pair(this,tmp)).first;
		}
		return *(std::vector<T>*)stack->second;
	}
public:
	DynamicScope(const T& initial):init(initial){}
	~DynamicScope()
	{
		auto tls_hash(__dynscp_tls());auto stack((std::vector<T>*)(*tls_hash)[this]);
		if (stack) delete stack;
		__dynscp_tls_dtor(this);
	}
	operator const T&(){return GetStack().back();}
	const T& operator()(){return GetStack().back();}
	T GetInitial() {return init;}
    
    DynamicBinding<T> operator=(const T& value) {return DynamicBinding<T>(*this,value);}
};

template <class T>
class DynamicBinding{
	DynamicScope<T>& s;
	void* operator new(size_t);
public:
	DynamicBinding(DynamicScope<T>& scope, const T& value):s(scope) {scope.GetStack().push_back(value);}
	~DynamicBinding() {s.GetStack().pop_back();}
};

#define EXTERN_DYNAMIC_SCOPE(variable,type) extern DynamicScope<decltype(type)> variable
#define DYNAMIC_SCOPE(variable,value) DynamicScope<decltype(value)> variable(value)
#define DYNAMIC_SCOPE_BIND(var,value) auto DynamicBinding_##var(DynamicBinding<decltype(var.GetInitial())>(var,value))
#define DYNAMIC_SCOPE_WITH(binding,code) {auto __holdbinding(binding);code;}
