#include "DynamicScope.h"

std::unordered_map<void*,void*>* __dynscp_tls()
{
		static thread_local std::unordered_map<void*,void*> *tls_hash = 0;		
		if (tls_hash == 0) tls_hash = new std::unordered_map<void*,void*>;
		return tls_hash;
}

void __dynscp_tls_dtor(void *key)
{
	auto map = __dynscp_tls();
	map->erase(key);
}
