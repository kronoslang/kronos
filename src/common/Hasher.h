#pragma once

template <typename TDest, typename TSrc>
static void ScrambleHash(TDest& dest, TSrc key) {
	static_assert(std::is_signed<TDest>::value == false, "Hash value must be unsigned");
	dest *= 2654435761ul; dest ^= key;
}

#define HASHER(dest,key) ScrambleHash(dest,key)