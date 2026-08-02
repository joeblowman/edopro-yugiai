#include <algorithm>
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../gframe/headless_host.cpp"

namespace {

class ScopedCoutRedirect final {
public:
	explicit ScopedCoutRedirect(std::ostream& stream) : stream(stream), original_buffer(stream.rdbuf(buffer.rdbuf())) {}

	~ScopedCoutRedirect() {
		stream.rdbuf(original_buffer);
	}

	std::string str() const {
		return buffer.str();
	}

private:
	std::ostream& stream;
	std::ostringstream buffer;
	std::streambuf* original_buffer;
};

using EventLog = std::vector<nlohmann::json>;

EventLog parseEvents(const std::string& output) {
	EventLog events;
	std::istringstream stream{ output };
	std::string line;
	while(std::getline(stream, line)) {
		if(line.empty())
			continue;
		events.emplace_back(nlohmann::json::parse(line));
	}
	return events;
}

void setPlayerName(ygo::DuelPlayer& player, std::wstring_view name) {
	std::fill(std::begin(player.name), std::end(player.name), 0);
	const auto limit = std::min(name.size(), std::size(player.name) - 1);
	for(size_t index = 0; index < limit; ++index)
		player.name[index] = static_cast<uint16_t>(name[index]);
}

void resetNetServerState() {
	ygo::NetServer::users.clear();
	ygo::NetServer::net_evbase = nullptr;
	ygo::NetServer::broadcast_ev = nullptr;
	ygo::NetServer::listener = nullptr;
	ygo::NetServer::duel_mode = nullptr;
}

void assertEvent(const nlohmann::json& event, const std::string& expected_name) {
	assert(event.at("event") == expected_name);
	assert(event.contains("timestamp"));
	assert(event.at("timestamp").is_string());
	assert(event.contains("detail"));
}

void runLifecycleSequenceTest() {
	ScopedCoutRedirect redirect{ std::cout };
	resetNetServerState();

	ygo::GenericDuel duel_mode{ 1, 1, false, 1 };
	ygo::NetServer::duel_mode = &duel_mode;
	ygo::NetServer::net_evbase = reinterpret_cast<event_base*>(0x1);
	ygo::NetServer::broadcast_ev = reinterpret_cast<event*>(0x1);
	ygo::NetServer::listener = reinterpret_cast<evconnlistener*>(0x1);

	auto& joined_player = ygo::NetServer::users[reinterpret_cast<bufferevent*>(0x1)];
	joined_player.game = &duel_mode;
	joined_player.type = NETPLAYER_TYPE_PLAYER1;
	setPlayerName(joined_player, L"[AI]Bot");

	HeadlessRoomEventMonitor monitor{ 7911 };
	monitor.emitRoomStarted();
	monitor.poll();

	duel_mode.pduel = reinterpret_cast<OCG_Duel>(0x1);
	duel_mode.match_result = { 1 };
	monitor.poll();

	duel_mode.pduel = nullptr;
	monitor.poll();

	ygo::NetServer::broadcast_ev = nullptr;
	ygo::NetServer::listener = nullptr;
	ygo::NetServer::net_evbase = nullptr;
	monitor.poll();

	const auto events = parseEvents(redirect.str());
	assert(events.size() == 5);
	assertEvent(events[0], "ROOM_STARTED");
	assert(events[0].at("detail").at("port") == 7911);

	assertEvent(events[1], "CLIENT_JOINED");
	assert(events[1].at("detail").at("name") == "[AI]Bot");
	assert(events[1].at("detail").at("seat") == NETPLAYER_TYPE_PLAYER1);

	assertEvent(events[2], "DUEL_STARTED");
	assert(events[2].at("detail").is_object());
	assert(events[2].at("detail").empty());

	assertEvent(events[3], "DUEL_ENDED");
	assert(events[3].at("detail").at("winner_seat") == 1);

	assertEvent(events[4], "ROOM_CLOSED");
	assert(events[4].at("detail").at("reason") == "duel_ended");
	assert(monitor.closed());

	resetNetServerState();
}

void runExplicitStopTest() {
	ScopedCoutRedirect redirect{ std::cout };
	resetNetServerState();

	ygo::GenericDuel duel_mode{ 1, 1, false, 1 };
	ygo::NetServer::duel_mode = &duel_mode;
	ygo::NetServer::net_evbase = reinterpret_cast<event_base*>(0x1);
	ygo::NetServer::broadcast_ev = reinterpret_cast<event*>(0x1);
	ygo::NetServer::listener = reinterpret_cast<evconnlistener*>(0x1);

	HeadlessRoomEventMonitor monitor{ 7911 };
	monitor.emitRoomStarted();
	monitor.requestStop();
	monitor.poll();
	assert(!monitor.closed());

	ygo::NetServer::broadcast_ev = nullptr;
	ygo::NetServer::listener = nullptr;
	ygo::NetServer::net_evbase = nullptr;
	monitor.poll();

	const auto events = parseEvents(redirect.str());
	assert(events.size() == 2);
	assertEvent(events[0], "ROOM_STARTED");
	assertEvent(events[1], "ROOM_CLOSED");
	assert(events[1].at("detail").at("reason") == "stopped");
	assert(monitor.closed());

	resetNetServerState();
}

} // namespace

int main() {
	runLifecycleSequenceTest();
	runExplicitStopTest();
}