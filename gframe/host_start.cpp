#include "host_start.h"

#include <event2/util.h>

#include "bufferio.h"
#include "game_config.h"
#include "netserver.h"

namespace ygo {

void ApplySharedRoomConfig(const SharedRoomConfig& config) {
	if(!gGameConfig)
		return;
	gGameConfig->gamename = config.room_name;
	gGameConfig->serverport = std::to_wstring(config.port);
	if(config.set_room_password)
		gGameConfig->roompass = config.room_password;
}

bool StartHostServer(uint16_t port, std::wstring* error_message) {
	if(NetServer::StartServer(port))
		return true;

	if(error_message) {
		*error_message = L"failed to bind the host listener";
		const auto socket_error = evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
		if(socket_error && *socket_error) {
			*error_message += L": ";
			*error_message += BufferIO::DecodeUTF8(socket_error);
		}
	}
	return false;
}

}