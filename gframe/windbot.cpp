#include "windbot.h"
#include <cerrno>
#include <cstdlib>
#include "utils.h"
#include "config.h"
#if EDOPRO_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif EDOPRO_ANDROID
#include "deck_manager.h"
#include "porting.h"
#include <nlohmann/json.hpp>
#elif EDOPRO_LINUX || EDOPRO_MACOS
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if !EDOPRO_ANDROID
#include "Base64.h"
#endif
#include "config.h"
#include "bufferio.h"
#include "logging.h"

namespace ygo {

bool LocalAiProcessHandle::IsRunning() {
#if EDOPRO_WINDOWS
	if(value == invalid)
		return false;
	if(WaitForSingleObject(value, 0) == WAIT_TIMEOUT)
		return true;
	CloseHandle(value);
	value = invalid;
	return false;
#elif EDOPRO_LINUX || EDOPRO_MACOS
	if(value == invalid)
		return false;
	int status{};
	pid_t result;
	do {
		result = waitpid(value, &status, WNOHANG);
	} while(result < 0 && errno == EINTR);
	if(result == 0)
		return true;
	if(result == value || (result < 0 && errno == ECHILD)) {
		// The process-wide SIGCHLD handler may win the waitpid race.
		value = invalid;
		return false;
	}
	if(kill(value, 0) == 0 || errno == EPERM)
		return true;
	value = invalid;
	return false;
#else
	return false;
#endif
}

void LocalAiProcessHandle::Terminate() {
	if(!IsRunning())
		return;
#if EDOPRO_WINDOWS
	if(value != invalid) {
		if(TerminateProcess(value, EXIT_FAILURE) || GetLastError() == ERROR_ACCESS_DENIED)
			(void)WaitForSingleObject(value, INFINITE);
		CloseHandle(value);
	}
#elif EDOPRO_LINUX || EDOPRO_MACOS
	if(value != invalid) {
		const auto pid = value;
		if(kill(pid, SIGKILL) == 0 || errno == ESRCH) {
			int status{};
			while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {
			}
		}
	}
#endif
	value = invalid;
}

void LocalAiProcessHandle::Release() {
#if EDOPRO_WINDOWS
	if(value != invalid)
		CloseHandle(value);
#elif EDOPRO_LINUX || EDOPRO_MACOS
	if(value != invalid) {
		int status{};
		(void)waitpid(value, &status, WNOHANG);
	}
#endif
	value = invalid;
}

#if EDOPRO_LINUX || EDOPRO_MACOS
epro::path_string WindBot::executablePath{};
#endif
static constexpr uint32_t version{ CLIENT_VERSION };
#if !EDOPRO_ANDROID && !EDOPRO_IOS
nlohmann::ordered_json WindBot::databases{};
bool WindBot::serialized{ false };
decltype(WindBot::serialized_databases) WindBot::serialized_databases{};
#endif

WindBot::launch_ret_t WindBot::Launch(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const {
#if !EDOPRO_ANDROID && !EDOPRO_IOS
	if(!serialized) {
		serialized = true;
		serialized_databases = base64_encode<decltype(serialized_databases)>(databases.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace));
	}
#endif
#if EDOPRO_WINDOWS
	//Windows can modify this string
	auto args = Utils::ToPathString(epro::format(
		L"WindBot.exe HostInfo=\"{}\" Deck=\"{}\" Port={} Version={} name=\"[AI] {}\" Chat={} Hand={} DbPaths={}{} AssetPath=./WindBot",
		pass, deck, port, version, name, chat, hand, serialized_databases, overridedeck ? epro::format(L" DeckFile=\"{}\"", overridedeck) : L""));
	STARTUPINFO si{ sizeof(si) };
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi;
	if(!CreateProcess(EPRO_TEXT("./WindBot/WindBot.exe"), &args[0], nullptr, nullptr, false, 0, nullptr, nullptr, &si, &pi))
		return false;
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return true;
#elif EDOPRO_ANDROID
	nlohmann::json param({
							{"HostInfo", BufferIO::EncodeUTF8(pass)},
							{"Deck", BufferIO::EncodeUTF8(deck)},
							{"Port", epro::to_string(port)},
							{"Version", epro::to_string(version)},
							{"Name", BufferIO::EncodeUTF8(name)},
							{"Chat", epro::to_string(static_cast<int>(chat))},
							{"Hand", epro::to_string(hand)}
						  });
	if(overridedeck) {
		auto overridedeck_utf8 = BufferIO::EncodeUTF8(overridedeck);
		if(porting::pathIsUri(overridedeck_utf8)) {
			Deck out;
			DeckManager::LoadDeckFromFile(overridedeck_utf8, out, true);
			param["DeckFile"] = BufferIO::EncodeUTF8(DeckManager::ExportDeckYdke(out));
		} else {
			param["DeckFile"] = overridedeck_utf8;
		}
	}
	porting::launchWindbot(param.dump());
	return true;
#elif EDOPRO_LINUX || EDOPRO_MACOS
	std::string argPass = epro::format("HostInfo={}", BufferIO::EncodeUTF8(pass));
	std::string argDeck = epro::format("Deck={}", BufferIO::EncodeUTF8(deck));
	std::string argPort = epro::format("Port={}", port);
	std::string argVersion = epro::format("Version={}", version);
	std::string argName = epro::format("name=[AI] {}", BufferIO::EncodeUTF8(name));
	std::string argChat = epro::format("Chat={}", chat);
	std::string argHand = epro::format("Hand={}", hand);
	std::string argDbPaths = epro::format("DbPaths={}", serialized_databases);
	std::string argDeckFile;
	if(overridedeck)
		argDeckFile = epro::format("DeckFile={}", BufferIO::EncodeUTF8(overridedeck));
	std::string oldpath;
	if(executablePath.size()) {
		oldpath = getenv("PATH");
		std::string envPath = epro::format("{}:{}", oldpath, executablePath);
		setenv("PATH", envPath.data(), true);
	}
	pid_t pid;
	{
		const char* argPass_cstr = argPass.data();
		const char* argDeck_cstr = argDeck.data();
		const char* argPort_cstr = argPort.data();
		const char* argVersion_cstr = argVersion.data();
		const char* argName_cstr = argName.data();
		const char* argChat_cstr = argChat.data();
		const char* argHand_cstr = argHand.data();
		const char* argDbPaths_cstr = argDbPaths.data();
		const char* argDeckFile_cstr = overridedeck ? argDeckFile.data() : nullptr;
		pid = vfork();
		if(pid == 0) {
			execlp("mono", "WindBot.exe", "./WindBot/WindBot.exe",
				   argPass_cstr, argDeck_cstr, argPort_cstr, argVersion_cstr, argName_cstr, argChat_cstr,
				   argDbPaths_cstr, "AssetPath=./WindBot", argHand_cstr, argDeckFile_cstr, nullptr);
			_exit(EXIT_FAILURE);
		}
	}
	if(executablePath.size())
		setenv("PATH", oldpath.data(), true);
	if(pid < 0 || waitpid(pid, nullptr, WNOHANG) != 0)
		pid = 0;
	return pid;
#else
	return {};
#endif
}

namespace {
#if EDOPRO_WINDOWS
// Encode one argv element according to the CommandLineToArgvW/MSVCRT rules used
// by CreateProcess. This preserves whitespace, quotes, and trailing backslashes.
epro::path_string QuoteWindowsArgument(epro::path_stringview argument) {
	epro::path_string quoted{ EPRO_TEXT('"') };
	size_t backslashes = 0;
	for(const auto character : argument) {
		if(character == EPRO_TEXT('\\')) {
			++backslashes;
			continue;
		}
		if(character == EPRO_TEXT('"')) {
			quoted.append(backslashes * 2 + 1, EPRO_TEXT('\\'));
			quoted.push_back(character);
		} else {
			quoted.append(backslashes, EPRO_TEXT('\\'));
			quoted.push_back(character);
		}
		backslashes = 0;
	}
	quoted.append(backslashes * 2, EPRO_TEXT('\\'));
	quoted.push_back(EPRO_TEXT('"'));
	return quoted;
}
#endif
}

LocalAiLaunchResult AiPlayerEngineEntry::Launch(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const {
	const bool launch_executable = !launch_args.executable.empty();
	const bool launch_module = !launch_args.interpreter.empty() && !launch_args.module.empty();
	if(!launch_executable && !launch_module)
		return { LocalAiLaunchStatus::INVALID_CONFIGURATION, {}, EINVAL };

#if EDOPRO_WINDOWS || EDOPRO_LINUX || EDOPRO_MACOS
	std::vector<epro::path_string> command;
	command.reserve(3 + 8);
	command.emplace_back(launch_executable ? launch_args.executable : launch_args.interpreter);
	if(!launch_executable) {
		command.emplace_back(EPRO_TEXT("-m"));
		command.emplace_back(launch_args.module);
	}
	for(const auto& parameter : GetLaunchParameters(port, pass, chat, hand, overridedeck))
		command.emplace_back(Utils::ToPathString(parameter));

#if EDOPRO_WINDOWS
	epro::path_string command_line;
	for(const auto& argument : command) {
		if(!command_line.empty())
			command_line.push_back(EPRO_TEXT(' '));
		command_line.append(QuoteWindowsArgument(argument));
	}

	STARTUPINFO startup_info{ sizeof(startup_info) };
	startup_info.dwFlags = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process_info{};
	if(!CreateProcess(command.front().c_str(), command_line.data(), nullptr, nullptr, false, 0,
	                  nullptr, nullptr, &startup_info, &process_info)) {
		return { LocalAiLaunchStatus::PROCESS_CREATION_FAILED, {}, GetLastError() };
	}
	CloseHandle(process_info.hThread);
	return { LocalAiLaunchStatus::CREATED, { process_info.hProcess }, 0 };
#else
	std::vector<char*> argv;
	argv.reserve(command.size() + 1);
	for(auto& argument : command)
		argv.push_back(argument.data());
	argv.push_back(nullptr);

	pid_t pid{};
	const int spawn_error = posix_spawnp(&pid, command.front().c_str(), nullptr, nullptr, argv.data(), environ);
	if(spawn_error != 0)
		return { LocalAiLaunchStatus::PROCESS_CREATION_FAILED, {}, static_cast<uint32_t>(spawn_error) };
	return { LocalAiLaunchStatus::CREATED, { pid }, 0 };
#endif
#else
	(void)port;
	(void)pass;
	(void)chat;
	(void)hand;
	(void)overridedeck;
	return { LocalAiLaunchStatus::UNSUPPORTED_PLATFORM, {}, 0 };
#endif
}

std::vector<std::string> AiPlayerEngineEntry::GetLaunchParameters(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const {
	const auto& args = launch_args;
	std::vector<std::string> parameters;
	parameters.reserve(8);
	parameters.emplace_back(epro::format("Host={}", BufferIO::EncodeUTF8(args.host)));
	parameters.emplace_back(epro::format("HostInfo={}", BufferIO::EncodeUTF8(pass)));
	if(overridedeck)
		parameters.emplace_back(epro::format("DeckFile={}", BufferIO::EncodeUTF8(overridedeck)));
	else
		parameters.emplace_back(epro::format("Deck={}", BufferIO::EncodeUTF8(args.deck)));
	parameters.emplace_back(epro::format("Port={}", port));
	parameters.emplace_back(epro::format("Version={}", args.protocol_version));
	parameters.emplace_back(epro::format("name={}", BufferIO::EncodeUTF8(args.display_name)));
	parameters.emplace_back(epro::format("Chat={}", chat));
	parameters.emplace_back(epro::format("Hand={}", hand));
	return parameters;
}

std::wstring WindBot::GetLaunchParameters(int port, epro::wstringview pass, bool chat, int hand, const wchar_t* overridedeck) const {
#if !EDOPRO_ANDROID && !EDOPRO_IOS
	if(!serialized) {
		serialized = true;
		serialized_databases = base64_encode<decltype(serialized_databases)>(databases.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace));
	}
	const auto assets_path = Utils::GetAbsolutePath(EPRO_TEXT("./WindBot"sv));
	const auto override_deck = overridedeck ? epro::format(L" DeckFile=\"{}\"", overridedeck) : L"";
	return epro::format(
		L"HostInfo=\"{}\" Deck=\"{}\" Port={} Version={} name=\"[AI] {}\" Chat={} Hand={} DbPaths={}{} AssetPath=\"{}\"",
		pass, deck, port, version, name, chat, hand, Utils::ToUnicodeIfNeeded(serialized_databases), override_deck, Utils::ToUnicodeIfNeeded(assets_path));
#else
	return {};
#endif
}

void WindBot::AddDatabase(epro::path_stringview database) {
#if EDOPRO_ANDROID
	porting::addWindbotDatabase(Utils::GetAbsolutePath(database));
#elif !EDOPRO_IOS
	serialized = false;
	databases.push_back(Utils::ToUTF8IfNeeded(Utils::GetAbsolutePath(database)));
#endif
}

}
