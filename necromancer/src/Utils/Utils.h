#pragma once

#include "Assert/Assert.h"
#include "Color/Color.h"
#include "Config/Config.h"
#include "CrashHandler/CrashHandler.h"
#include "Storage/Storage.h"
#include "Hash/Hash.h"
#include "HookManager/HookManager.h"
#include "InterfaceManager/InterfaceManager.h"
#include "Math/Math.h"
#include "Memory/Memory.h"
#include "SignatureManager/SignatureManager.h"
#include "Singleton/Singleton.h"
#include "Vector/Vector.h"

#include <intrin.h>
#include <random>
#include <chrono>
#include <filesystem>
#include <deque>

namespace Utils
{
    static std::wstring ConvertUtf8ToWide(const std::string& ansi)
    {
        const int size = MultiByteToWideChar(CP_UTF8, 0, ansi.c_str(), -1, nullptr, 0);
		std::wstring result(size, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, ansi.c_str(), -1, result.data(), size);
		return result;
    }

    static std::string ConvertWideToUTF8(const std::wstring& unicode)
    {
        const int size = WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), -1, nullptr, 0, nullptr, nullptr);
		std::string result(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, unicode.c_str(), -1, result.data(), size, nullptr, nullptr);
		return result;
    }

    // Uses a static generator to avoid creating a new random_device + mt19937 per call
    // The old version queried OS entropy every invocation which is extremely slow
    static int RandInt(int min, int max)
    {
        static thread_local std::mt19937 gen{ std::random_device{}() };
        std::uniform_int_distribution<> distr(min, max);
        return distr(gen);
    }
}