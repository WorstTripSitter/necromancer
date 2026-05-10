#pragma once
#include "../../Utils/Memory/Memory.h"
#include "interface.h"

class IDemoRecorder
{
public:
	bool IsRecording()
	{
		return Memory::get_vfunc<bool(__thiscall*)(void*)>(this, 4)(this);
	}

	void RecordUserInput(int cmdnumber)
	{
		return Memory::get_vfunc<void(__thiscall*)(void*, int)>(this, 9)(this, cmdnumber);
	}
};

class IDemoPlayer
{
public:
	bool IsPlayingBack()
	{
		return Memory::get_vfunc<bool(__thiscall*)(void*)>(this, 6)(this);
	}
};

MAKE_INTERFACE_SIGNATURE(IDemoPlayer, DemoPlayer, "engine.dll", "48 8B 0D ? ? ? ? 40 B7", 0, 1);
MAKE_INTERFACE_SIGNATURE(IDemoRecorder, DemoRecorder, "engine.dll", "48 8B 0D ? ? ? ? 8D 56", 0, 1);
