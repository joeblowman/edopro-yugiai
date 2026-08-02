#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "deck_manager.h"
#include "headless_host.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

args_t MakeArgs(const std::string& payload) {
	args_t args{};
	args[LAUNCH_PARAM::HOST_HEADLESS].enabled = true;
	args[LAUNCH_PARAM::HOST_HEADLESS].argument = ygo::Utils::ToPathString(payload);
	return args;
}

args_t MakeArgs(const std::vector<std::string>& payload_tokens) {
	args_t args{};
	auto& option = args[LAUNCH_PARAM::HOST_HEADLESS];
	option.enabled = true;
	option.arguments.reserve(payload_tokens.size());
	for(const auto& token : payload_tokens)
		option.arguments.push_back(ygo::Utils::ToPathString(token));
	if(!option.arguments.empty())
		option.argument = option.arguments.front();
	return args;
}

struct ScopedDeckManager {
	ygo::DeckManager deck_manager;
	ygo::DeckManager* previous{ nullptr };

	ScopedDeckManager() {
		previous = ygo::gdeckManager;
		ygo::gdeckManager = &deck_manager;
	}

	~ScopedDeckManager() {
		ygo::gdeckManager = previous;
	}
};

#if !defined(_WIN32)
struct StreamCapture {
	int saved_stdout{ -1 };
	int saved_stderr{ -1 };
	int stdout_pipe[2]{ -1, -1 };
	int stderr_pipe[2]{ -1, -1 };

	StreamCapture() {
		assert(pipe(stdout_pipe) == 0);
		assert(pipe(stderr_pipe) == 0);
		std::fflush(stdout);
		std::fflush(stderr);
		saved_stdout = dup(STDOUT_FILENO);
		saved_stderr = dup(STDERR_FILENO);
		assert(saved_stdout != -1);
		assert(saved_stderr != -1);
		assert(dup2(stdout_pipe[1], STDOUT_FILENO) != -1);
		assert(dup2(stderr_pipe[1], STDERR_FILENO) != -1);
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
	}

	~StreamCapture() {
		if(saved_stdout != -1) {
			std::fflush(stdout);
			std::fflush(stderr);
			dup2(saved_stdout, STDOUT_FILENO);
			dup2(saved_stderr, STDERR_FILENO);
			close(saved_stdout);
			close(saved_stderr);
		}
		if(stdout_pipe[0] != -1)
			close(stdout_pipe[0]);
		if(stderr_pipe[0] != -1)
			close(stderr_pipe[0]);
	}

	static std::string Drain(int fd) {
		std::string output;
		char buffer[4096];
		for(;;) {
			const auto bytes_read = read(fd, buffer, sizeof(buffer));
			if(bytes_read == 0)
				break;
			if(bytes_read < 0)
				break;
			output.append(buffer, static_cast<size_t>(bytes_read));
		}
		return output;
	}

	std::pair<std::string, std::string> Finish() {
		std::fflush(stdout);
		std::fflush(stderr);
		dup2(saved_stdout, STDOUT_FILENO);
		dup2(saved_stderr, STDERR_FILENO);
		close(saved_stdout);
		close(saved_stderr);
		saved_stdout = -1;
		saved_stderr = -1;
		close(stdout_pipe[1]);
		close(stderr_pipe[1]);
		const auto stdout_text = Drain(stdout_pipe[0]);
		const auto stderr_text = Drain(stderr_pipe[0]);
		close(stdout_pipe[0]);
		close(stderr_pipe[0]);
		stdout_pipe[0] = -1;
		stderr_pipe[0] = -1;
		return { stdout_text, stderr_text };
	}
};

size_t CountNewlines(const std::string& text) {
	return static_cast<size_t>(std::count(text.begin(), text.end(), '\n'));
}
#endif

ygo::HeadlessHostConfig MakeValidConfig() {
	ygo::HeadlessHostConfig config;
	config.room_name = L"Headless Contract Room";
	config.room_password = L"";
	config.port = 7911;
	config.host_info.lflist = 0xA11C0DEu;
	config.host_info.mode = MODE_SINGLE;
	config.host_info.best_of = 0;
	return config;
}

} // namespace

int main() {
	using namespace ygo;

	ScopedDeckManager deck_manager_scope;
	deck_manager_scope.deck_manager._lfList.push_back({ 0xA11C0DEu, L"Contract Test Banlist", {}, false });

	{
		auto config = MakeValidConfig();
		std::wstring error_message;
		assert(ValidateHeadlessHostConfig(config, &error_message));
		assert(error_message.empty());
		assert(config.room_name == L"Headless Contract Room");
		assert(config.port == 7911);
	}

	{
		auto config = MakeValidConfig();
		config.host_info.lflist = 0;
		std::wstring error_message;
		assert(ValidateHeadlessHostConfig(config, &error_message));
		assert(error_message.empty());
	}

	{
		auto config = MakeValidConfig();
		config.host_info.lflist = 0xDEADBEEFu;
		std::wstring error_message;
		assert(!ValidateHeadlessHostConfig(config, &error_message));
		assert(error_message == L"headless host banlist hash is not loaded");
	}

	{
		auto config = MakeValidConfig();
		config.room_name.clear();
		std::wstring error_message;
		assert(!ValidateHeadlessHostConfig(config, &error_message));
		assert(error_message == L"headless host room name must not be empty");
	}

	{
		auto config = MakeValidConfig();
		config.host_info.mode = 7;
		std::wstring error_message;
		assert(!ValidateHeadlessHostConfig(config, &error_message));
		assert(error_message == L"headless host mode must map to a supported duel mode");
	}

	{
		auto config = MakeValidConfig();
		config.port = 0;
		auto result = StartHeadlessHost(config);
		assert(result.status == HeadlessHostStatus::INVALID_CONFIG);
		assert(result.error_message == L"headless host port must be in the range 1-65535");
	}

#if !defined(_WIN32)
	{
		StreamCapture capture;
		const auto exit_code = headless_host_main(MakeArgs("Name=Room Port=0 Mode=single"));
		const auto [stdout_text, stderr_text] = capture.Finish();
		assert(exit_code != EXIT_SUCCESS);
		assert(CountNewlines(stdout_text) == 1);
		assert(stdout_text.find("\"event\":\"ERROR\"") != std::string::npos);
		assert(stdout_text.find("headless host port must be in the range 1-65535") != std::string::npos);
		assert(stderr_text.find("headless host port must be in the range 1-65535") != std::string::npos);
	}

	{
		StreamCapture capture;
		const auto exit_code = headless_host_main(MakeArgs("Name=Room Port=7911 Mode=single Extra=1"));
		const auto [stdout_text, stderr_text] = capture.Finish();
		assert(exit_code != EXIT_SUCCESS);
		assert(CountNewlines(stdout_text) == 1);
		assert(stdout_text.find("\"event\":\"ERROR\"") != std::string::npos);
		assert(stdout_text.find("headless host arguments contain an unsupported key") != std::string::npos);
		assert(stderr_text.find("headless host arguments contain an unsupported key") != std::string::npos);
	}

	{
		StreamCapture capture;
		const auto exit_code = headless_host_main(MakeArgs({
			"Name=Room With Space",
			"Port=0",
			"Mode=single",
		}));
		const auto [stdout_text, stderr_text] = capture.Finish();
		assert(exit_code != EXIT_SUCCESS);
		assert(CountNewlines(stdout_text) == 1);
		assert(stdout_text.find("\"event\":\"ERROR\"") != std::string::npos);
		assert(stdout_text.find("headless host port must be in the range 1-65535") != std::string::npos);
		assert(stderr_text.find("headless host port must be in the range 1-65535") != std::string::npos);
	}
#endif

	return EXIT_SUCCESS;
}