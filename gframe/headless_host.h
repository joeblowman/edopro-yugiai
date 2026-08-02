#ifndef HEADLESS_HOST_H
#define HEADLESS_HOST_H

#include <string>
#include "cli_args.h"
#include "network.h"

namespace ygo {

enum class HeadlessHostStatus {
	OK,
	INVALID_CONFIG,
	SERVER_START_FAILED,
	BROADCAST_START_FAILED,
};

struct HeadlessHostConfig {
	uint16_t port{ 0 };
	std::wstring room_name;
	std::wstring room_password;
	std::wstring room_notes;
	HostInfo host_info{};
};

struct HeadlessHostResult {
	HeadlessHostStatus status{ HeadlessHostStatus::INVALID_CONFIG };
	std::wstring error_message;
};

[[nodiscard]] bool ValidateHeadlessHostConfig(const HeadlessHostConfig& config, std::wstring* error_message = nullptr);
[[nodiscard]] HeadlessHostResult StartHeadlessHost(const HeadlessHostConfig& config);
[[nodiscard]] bool InitializeHeadlessDuelRuntime(std::wstring* error_message = nullptr);
[[nodiscard]] OCG_Duel SetupHeadlessDuelRuntime(OCG_DuelOptions opts, std::wstring* error_message = nullptr);

}

int headless_host_main(const args_t& args);

#endif // HEADLESS_HOST_H