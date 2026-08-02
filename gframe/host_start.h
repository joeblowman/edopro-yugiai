#ifndef HOST_START_H
#define HOST_START_H

#include <cstdint>
#include <string>

namespace ygo {

struct SharedRoomConfig {
	uint16_t port{ 0 };
	std::wstring room_name;
	std::wstring room_password;
	bool set_room_password{ true };
};

void ApplySharedRoomConfig(const SharedRoomConfig& config);
[[nodiscard]] bool StartHostServer(uint16_t port, std::wstring* error_message = nullptr);

}

#endif // HOST_START_H