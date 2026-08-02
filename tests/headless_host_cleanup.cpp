#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#include "netserver.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

bool canBindTcpPort(uint16_t port) {
	const auto fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0)
		return false;

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(port);

	const bool ok = bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;

#if defined(_WIN32)
	closesocket(fd);
#else
	close(fd);
#endif

	return ok;
}

uint16_t findAvailablePort() {
	for(uint16_t port = 41000; port < 51000; ++port) {
		if(canBindTcpPort(port))
			return port;
	}
	return 0;
}

bool waitForServerShutdown() {
	using namespace std::chrono_literals;
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while(std::chrono::steady_clock::now() < deadline) {
		if(ygo::NetServer::net_evbase == nullptr &&
		   ygo::NetServer::listener == nullptr &&
		   ygo::NetServer::broadcast_ev == nullptr &&
		   ygo::NetServer::duel_mode == nullptr &&
		   ygo::NetServer::users.empty()) {
			return true;
		}
		std::this_thread::sleep_for(10ms);
	}

	return ygo::NetServer::net_evbase == nullptr &&
	       ygo::NetServer::listener == nullptr &&
	       ygo::NetServer::broadcast_ev == nullptr &&
	       ygo::NetServer::duel_mode == nullptr &&
	       ygo::NetServer::users.empty();
}

} // namespace

int main() {
	const auto port = findAvailablePort();
	assert(port != 0);

	assert(ygo::NetServer::StartServer(port));
	assert(ygo::NetServer::net_evbase != nullptr);
	assert(ygo::NetServer::listener != nullptr);

	// While running, attempting to start another listener must fail.
	assert(!ygo::NetServer::StartServer(port));

	ygo::NetServer::StopServer();
	assert(waitForServerShutdown());

	// After a clean stop, the same port should be reusable immediately.
	assert(ygo::NetServer::StartServer(port));
	ygo::NetServer::StopServer();
	assert(waitForServerShutdown());
}