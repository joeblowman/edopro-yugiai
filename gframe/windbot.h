#ifndef WINDBOT_H
#define WINDBOT_H

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "config.h"
#if EDOPRO_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif EDOPRO_LINUX || EDOPRO_MACOS
#include <sys/types.h>
#endif
#if EDOPRO_WINDOWS || EDOPRO_MACOS || EDOPRO_LINUX
#include <nlohmann/json.hpp>
#endif
#include "text_types.h"

namespace ygo {

enum class LocalAiEngineKind : uint8_t {
	WINDBOT,
	AI_PLAYER,
};

struct LocalAiProcessHandle {
#if EDOPRO_WINDOWS
	using native_type = HANDLE;
	static constexpr native_type invalid = nullptr;
#elif EDOPRO_LINUX || EDOPRO_MACOS
	using native_type = pid_t;
	static constexpr native_type invalid = 0;
#else
	using native_type = std::nullptr_t;
	static constexpr native_type invalid = nullptr;
#endif

	native_type value{ invalid };

	// Polling owns completion cleanup: exited children are reaped on POSIX and
	// signaled process handles are closed on Windows.
	bool IsRunning();
	void Terminate();
	void Release();
};

enum class LocalAiLaunchStatus : uint8_t {
	CREATED,
	INVALID_CONFIGURATION,
	PROCESS_CREATION_FAILED,
	UNSUPPORTED_PLATFORM,
};

// A CREATED result transfers process lifecycle ownership to the caller: close
// the HANDLE on Windows, or terminate/wait the PID on POSIX as appropriate.
struct LocalAiLaunchResult {
	LocalAiLaunchStatus status{ LocalAiLaunchStatus::PROCESS_CREATION_FAILED };
	LocalAiProcessHandle process_handle;
	// GetLastError() on Windows or the posix_spawnp() error number on POSIX.
	uint32_t error_code{};

	explicit operator bool() const {
		return status == LocalAiLaunchStatus::CREATED;
	}
};

// Stable process configuration. Room-specific values such as the port and
// HostInfo password are supplied for each launch and are intentionally absent.
struct AiPlayerLaunchArgs {
	epro::path_string executable;
	epro::path_string interpreter;
	epro::path_string module;
	std::wstring host{ L"127.0.0.1" };
	uint32_t protocol_version{ CLIENT_VERSION };
	std::wstring display_name{ L"[AI] AI Player" };
	std::wstring deck;
};

struct AiPlayerEngineEntry {
	std::wstring label;
	LocalAiEngineKind engine_kind{ LocalAiEngineKind::AI_PLAYER };
	AiPlayerLaunchArgs launch_args;

	LocalAiLaunchResult Launch(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const;
	std::vector<std::string> GetLaunchParameters(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const;
};

struct WindBot {
	static constexpr LocalAiEngineKind engine_kind = LocalAiEngineKind::WINDBOT;

	std::wstring name;
	std::wstring deck;
	std::wstring deckfile;
	int difficulty;
	std::set<int> masterRules;

#if EDOPRO_WINDOWS || EDOPRO_ANDROID || EDOPRO_IOS
	using launch_ret_t = bool;
#elif EDOPRO_MACOS || EDOPRO_LINUX
	using launch_ret_t = pid_t;
	static epro::path_string executablePath;
#endif
	launch_ret_t Launch(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const;
	std::wstring GetLaunchParameters(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const;

#if EDOPRO_WINDOWS || EDOPRO_MACOS || EDOPRO_LINUX
	static nlohmann::ordered_json databases;
	static bool serialized;
#if EDOPRO_WINDOWS
	static std::wstring serialized_databases;
#else
	static std::string serialized_databases;
#endif
#endif

	static void AddDatabase(epro::path_stringview database);
};

}

#endif
