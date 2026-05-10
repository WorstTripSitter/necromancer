#pragma once

#include <Windows.h>

#include <cstdint>

typedef unsigned __int64 QWORD;

namespace Memory
{
	std::uintptr_t FindSignature(const char *szModule, const char *szPattern);
	PVOID FindInterface(const char *szModule, const char *szObject);

	inline void* GetVFunc(void* instance, size_t index)
	{
		const auto vtable = *static_cast<void***>(instance);
		return vtable[index];
	}

	inline std::uintptr_t RelToAbs(const std::uintptr_t address)
	{
		return *reinterpret_cast<std::int32_t*>(address + 0x3) + address + 0x7;
	}

	inline void**& get_vtable(void* inst, const unsigned int offset)
	{
		return *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(inst) + offset);
	}

	inline const void** get_vtable(const void* inst, const unsigned int offset)
	{
		return *reinterpret_cast<const void***>(reinterpret_cast<uintptr_t>(inst) + offset);
	}

	inline QWORD rel2abs(QWORD Address, QWORD RVAOffset)
	{
		return *reinterpret_cast<int32_t*>(Address + RVAOffset) + Address + (RVAOffset + sizeof(int32_t));
	}

	template<typename T>
	inline T get_vfunc(void* inst, const unsigned int index, const unsigned int offset = 0u) {
		return reinterpret_cast<T>(get_vtable(inst, offset)[index]);
	}
}