#include <algorithm>
#include <cassert>
#include <cstdlib>
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

void assertIsoUtcTimestamp(const nlohmann::json& event) {
	assert(event.contains("timestamp"));
	assert(event.at("timestamp").is_string());
	const auto timestamp = event.at("timestamp").get<std::string>();
	assert(timestamp.size() == 20);
	assert(timestamp[4] == '-');
	assert(timestamp[7] == '-');
	assert(timestamp[10] == 'T');
	assert(timestamp[13] == ':');
	assert(timestamp[16] == ':');
	assert(timestamp[19] == 'Z');
}

void assertBaseEventShape(const nlohmann::json& event, const std::string& expected_name) {
	assert(event.contains("event"));
	assert(event.at("event").is_string());
	assert(event.at("event") == expected_name);
	assertIsoUtcTimestamp(event);
	assert(event.contains("detail"));
	assert(event.at("detail").is_object());
}

args_t makeArgs(const std::string& payload) {
	args_t args{};
	args[LAUNCH_PARAM::HOST_HEADLESS].enabled = true;
	args[LAUNCH_PARAM::HOST_HEADLESS].argument = ygo::Utils::ToPathString(payload);
	return args;
}

void runLifecycleObservabilityContractTest() {
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
	setPlayerName(joined_player, L"[AI]Observer");

	HeadlessRoomEventMonitor monitor{ 7911 };
	monitor.emitRoomStarted();
	monitor.poll();
	monitor.poll();

	duel_mode.pduel = reinterpret_cast<OCG_Duel>(0x1);
	duel_mode.match_result = { 0 };
	monitor.poll();
	monitor.poll();

	duel_mode.pduel = nullptr;
	monitor.poll();

	ygo::NetServer::broadcast_ev = nullptr;
	ygo::NetServer::listener = nullptr;
	ygo::NetServer::net_evbase = nullptr;
	monitor.poll();

	const auto events = parseEvents(redirect.str());
	assert(events.size() == 5);

	assertBaseEventShape(events[0], "ROOM_STARTED");
	assert(events[0].at("detail").at("port") == 7911);

	assertBaseEventShape(events[1], "CLIENT_JOINED");
	assert(events[1].at("detail").at("name") == "[AI]Observer");
	assert(events[1].at("detail").at("seat") == NETPLAYER_TYPE_PLAYER1);

	assertBaseEventShape(events[2], "DUEL_STARTED");
	assert(events[2].at("detail").empty());

	assertBaseEventShape(events[3], "DUEL_ENDED");
	assert(events[3].at("detail").at("winner_seat") == 0);

	assertBaseEventShape(events[4], "ROOM_CLOSED");
	assert(events[4].at("detail").at("reason") == "duel_ended");

	resetNetServerState();
}

void runErrorEventObservabilityContractTest() {
	ScopedCoutRedirect redirect{ std::cout };
	const auto exit_code = headless_host_main(makeArgs("Name=Room Port=0 Mode=single"));
	const auto events = parseEvents(redirect.str());

	assert(exit_code != EXIT_SUCCESS);
	assert(events.size() == 1);
	assertBaseEventShape(events[0], "ERROR");
	assert(events[0].at("detail").at("reason").is_string());
	assert(events[0].at("detail").at("reason").get<std::string>().find("1-65535") != std::string::npos);
}

} // namespace

int main() {
	runLifecycleObservabilityContractTest();
	runErrorEventObservabilityContractTest();
}