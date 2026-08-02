#define private public
#define protected public
#include "headless_host.h"
#include "generic_duel.h"
#include "netserver.h"
#undef protected
#undef private

#include <algorithm>
#include <charconv>
#include <cctype>
#include <climits>
#include <chrono>
#include <cwchar>
#include <cstdlib>
#include <iostream>
#include <ctime>
#include <limits>
#include <memory>
#if !defined(_WIN32)
#include <signal.h>
#endif
#include <thread>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "bufferio.h"
#include "data_manager.h"
#include "deck_manager.h"
#include "dllinterface.h"
#include "file_stream.h"
#include "game_config.h"
#include "host_start.h"
#include "logging.h"
#include "utils.h"

namespace {
constexpr size_t kRoomNameLimit = 19;
constexpr size_t kRoomPasswordLimit = 19;
constexpr size_t kRoomNotesLimit = 199;

#if !defined(_WIN32)
volatile sig_atomic_t g_termination_requested = 0;

void handleTerminationSignal(int) {
	g_termination_requested = 1;
}

bool installTerminationSignalHandlers() {
	struct sigaction action {};
	action.sa_handler = handleTerminationSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if(sigaction(SIGINT, &action, nullptr) != 0)
		return false;
	if(sigaction(SIGTERM, &action, nullptr) != 0)
		return false;
	return true;
}
#else
bool installTerminationSignalHandlers() {
	return true;
}
#endif

struct LaunchOptions {
	bool has_name{ false };
	bool has_port{ false };
	bool has_password{ false };
	bool has_mode{ false };
	bool has_best_of{ false };
	bool has_config_file{ false };
	std::wstring name;
	std::wstring password;
	std::string mode;
	std::string config_file;
	uint16_t port{ 0 };
	int best_of{ 1 };
};

bool fail(std::wstring* error_message, std::wstring_view message) {
	if(error_message)
		*error_message = message;
	return false;
}

template<typename T>
bool parseIntegerValue(std::string_view text, T& value) {
	T parsed{};
	auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
	if(result.ec != std::errc() || result.ptr != text.data() + text.size())
		return false;
	value = parsed;
	return true;
}

std::string trimAscii(std::string_view text) {
	auto begin = text.find_first_not_of(" \t\r\n\f\v");
	if(begin == std::string_view::npos)
		return {};
	auto end = text.find_last_not_of(" \t\r\n\f\v");
	return std::string(text.substr(begin, end - begin + 1));
}

std::string normalizeKey(std::string_view key) {
	std::string normalized;
	normalized.reserve(key.size());
	for(char c : key) {
		if(c == '_' || c == '-')
			continue;
		normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return normalized;
}

bool tokenizeLaunchString(std::string_view input, std::vector<std::string>& tokens, std::wstring* error_message) {
	size_t index = 0;
	while(index < input.size()) {
		while(index < input.size() && std::isspace(static_cast<unsigned char>(input[index])))
			++index;
		if(index >= input.size())
			break;

		std::string token;
		bool in_quotes = false;
		char quote_char = 0;
		while(index < input.size()) {
			const char c = input[index++];
			if(in_quotes) {
				if(c == '\\' && index < input.size()) {
					token.push_back(input[index++]);
					continue;
				}
				if(c == quote_char) {
					in_quotes = false;
					continue;
				}
				token.push_back(c);
				continue;
			}
			if(c == '"' || c == '\'') {
				in_quotes = true;
				quote_char = c;
				continue;
			}
			if(std::isspace(static_cast<unsigned char>(c)))
				break;
			token.push_back(c);
		}
		if(in_quotes)
			return fail(error_message, L"headless host arguments contain an unterminated quoted value");
		if(!token.empty())
			tokens.push_back(std::move(token));
	}
	return true;
}

bool splitKeyValue(const std::string& token, std::string& key, std::string& value) {
	const auto pos = token.find('=');
	if(pos == std::string::npos)
		return false;
	key = trimAscii(std::string_view(token).substr(0, pos));
	value = trimAscii(std::string_view(token).substr(pos + 1));
	return !key.empty();
}

bool looksLikeCombinedPayload(std::string_view payload) {
	for(size_t i = 0; i < payload.size(); ++i) {
		if(!std::isspace(static_cast<unsigned char>(payload[i])))
			continue;
		size_t token_begin = i + 1;
		while(token_begin < payload.size() && std::isspace(static_cast<unsigned char>(payload[token_begin])))
			++token_begin;
		if(token_begin >= payload.size())
			break;
		size_t cursor = token_begin;
		while(cursor < payload.size() && !std::isspace(static_cast<unsigned char>(payload[cursor])) && payload[cursor] != '=')
			++cursor;
		if(cursor > token_begin && cursor < payload.size() && payload[cursor] == '=')
			return true;
	}
	return false;
}

bool collectLaunchTokens(const Option& host_option, std::vector<std::string>& tokens, std::wstring* error_message) {
	if(host_option.arguments.empty()) {
		const auto raw_payload = ygo::Utils::ToUTF8IfNeeded(host_option.argument);
		return tokenizeLaunchString(raw_payload, tokens, error_message);
	}

	if(host_option.arguments.size() == 1) {
		const auto single_arg = ygo::Utils::ToUTF8IfNeeded(host_option.arguments.front());
		if(looksLikeCombinedPayload(single_arg))
			return tokenizeLaunchString(single_arg, tokens, error_message);
		if(!single_arg.empty())
			tokens.push_back(single_arg);
		return true;
	}

	tokens.reserve(host_option.arguments.size());
	for(const auto& raw_argument : host_option.arguments)
		tokens.push_back(ygo::Utils::ToUTF8IfNeeded(raw_argument));
	return true;
}

std::string currentTimestampUtc() {
	auto now = std::chrono::system_clock::now();
	auto time = std::chrono::system_clock::to_time_t(now);
	std::tm utc{};
#if defined(_WIN32)
	gmtime_s(&utc, &time);
#else
	gmtime_r(&time, &utc);
#endif
	char buffer[32]{};
	if(std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
		return "1970-01-01T00:00:00Z";
	return buffer;
}

void writeLifecycleEvent(const std::string& event, const nlohmann::json& detail) {
	const nlohmann::json payload = {
		{ "event", event },
		{ "timestamp", currentTimestampUtc() },
		{ "detail", detail },
	};
	std::cout << payload.dump() << '\n';
	std::cout.flush();
}

std::string toUtf8(const std::wstring& text) {
	return ygo::Utils::ToUTF8IfNeeded(text);
}

void emitErrorEvent(std::wstring_view reason) {
	writeLifecycleEvent("ERROR", {
		{ "reason", toUtf8(std::wstring(reason)) },
	});
}

class HeadlessRoomEventMonitor final {
public:
	explicit HeadlessRoomEventMonitor(uint16_t port) : port(port) {}

	bool hasRuntimeError() const {
		return runtime_error_detected;
	}

	void requestStop() {
		shutdown_requested = true;
	}

	bool shouldRequestStop() const {
		return stop_requested;
	}

	void emitRoomStarted() {
		if(room_started)
			return;
		room_started = true;
		writeLifecycleEvent("ROOM_STARTED", {
			{ "port", port },
		});
	}

	void poll() {
		if(room_closed)
			return;

		if(shutdown_requested) {
			if(ygo::NetServer::net_evbase || ygo::NetServer::listener || ygo::NetServer::broadcast_ev)
				return;

			room_closed = true;
			writeLifecycleEvent("ROOM_CLOSED", {
				{ "reason", "stopped" },
			});
			return;
		}

		for(const auto& entry : ygo::NetServer::users) {
			const auto* key = entry.first;
			const auto& player = entry.second;
			if(!player.game)
				continue;
			if(!joined_clients.insert(key).second)
				continue;

			wchar_t player_name[20]{};
			BufferIO::DecodeUTF16(player.name, player_name, 20);
			nlohmann::json detail = {
				{ "name", toUtf8(player_name) },
				{ "seat", player.type },
			};
			writeLifecycleEvent("CLIENT_JOINED", detail);
		}

		auto* duel_mode = static_cast<ygo::GenericDuel*>(ygo::NetServer::duel_mode);
		const bool duel_active = duel_mode && duel_mode->pduel;
		if(duel_active && !duel_started) {
			duel_started = true;
			duel_ended_pending_close = false;
			writeLifecycleEvent("DUEL_STARTED", {});
		}
		if(!duel_active && duel_started) {
			duel_started = false;
			duel_ended_pending_close = true;
			stop_requested = true;
			int winner_seat = -1;
			if(duel_mode && !duel_mode->match_result.empty()) {
				const auto last_result = duel_mode->match_result.back();
				if(last_result < 2)
					winner_seat = static_cast<int>(last_result);
			}
			nlohmann::json detail = nlohmann::json::object();
			if(winner_seat >= 0)
				detail["winner_seat"] = winner_seat;
			else
				detail["winner_seat"] = nullptr;
			writeLifecycleEvent("DUEL_ENDED", detail);
		}

		if(ygo::NetServer::net_evbase || ygo::NetServer::listener || ygo::NetServer::broadcast_ev)
			return;

		room_closed = true;
		if(!shutdown_requested && !duel_ended_pending_close) {
			runtime_error_detected = true;
			writeLifecycleEvent("ERROR", {
				{ "reason", "headless host room closed unexpectedly" },
			});
			writeLifecycleEvent("ROOM_CLOSED", {
				{ "reason", "error" },
			});
			return;
		}

		writeLifecycleEvent("ROOM_CLOSED", {
			{ "reason", duel_ended_pending_close ? "duel_ended" : "stopped" },
		});
	}

	bool closed() const {
		return room_closed;
	}

private:
	uint16_t port;
	bool room_started{ false };
	bool duel_started{ false };
	bool duel_ended_pending_close{ false };
	bool room_closed{ false };
	bool shutdown_requested{ false };
	bool stop_requested{ false };
	bool runtime_error_detected{ false };
	std::unordered_set<const bufferevent*> joined_clients;
};

bool isServerShutdownComplete() {
	return ygo::NetServer::net_evbase == nullptr &&
		ygo::NetServer::listener == nullptr &&
		ygo::NetServer::broadcast_ev == nullptr &&
		ygo::NetServer::duel_mode == nullptr &&
		ygo::NetServer::users.empty();
}

bool waitForServerShutdown(std::chrono::milliseconds timeout) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while(std::chrono::steady_clock::now() < deadline) {
		if(isServerShutdownComplete())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return isServerShutdownComplete();
}

bool verifyPortReusable(uint16_t port, std::wstring* error_message) {
	if(!ygo::NetServer::StartServer(port)) {
		if(error_message)
			*error_message = L"headless host port remains unavailable after shutdown";
		return false;
	}
	ygo::NetServer::StopServer();
	if(!waitForServerShutdown(std::chrono::milliseconds(2000))) {
		if(error_message)
			*error_message = L"headless host port rebind probe did not shut down cleanly";
		return false;
	}
	return true;
}

bool isKnownBanlist(const uint32_t hash) {
	if(!ygo::gdeckManager || ygo::gdeckManager->_lfList.empty())
		return false;
	return std::any_of(ygo::gdeckManager->_lfList.begin(), ygo::gdeckManager->_lfList.end(), [hash](const auto& entry) {
		return entry.hash == hash;
	});
}

class HeadlessRuntimeState final {
public:
	std::unique_ptr<ygo::DataManager> data_manager;
	std::vector<epro::path_string> script_dirs;
	std::vector<epro::path_string> init_scripts;
#ifdef YGOPRO_BUILD_DLL
	void* ocgcore{ nullptr };
#endif
	bool initialized{ false };

	~HeadlessRuntimeState() {
#ifdef YGOPRO_BUILD_DLL
		if(ocgcore)
			UnloadCore(ocgcore);
#endif
	}
};

HeadlessRuntimeState& headlessRuntimeState() {
	static HeadlessRuntimeState state;
	return state;
}

void populateHeadlessScriptDirectories(HeadlessRuntimeState& state) {
	state.script_dirs.clear();
	state.init_scripts.clear();

	if(ygo::Utils::FileExists(EPRO_TEXT("./init.lua")))
		state.init_scripts.push_back(EPRO_TEXT("./init.lua"));

	state.script_dirs.push_back(EPRO_TEXT("./expansions/script/"));
	auto expansion_subdirs = ygo::Utils::FindSubfolders(EPRO_TEXT("./expansions/script/"));
	state.script_dirs.insert(state.script_dirs.end(), std::make_move_iterator(expansion_subdirs.begin()), std::make_move_iterator(expansion_subdirs.end()));

	state.script_dirs.push_back(EPRO_TEXT("./script/"));
	auto script_subdirs = ygo::Utils::FindSubfolders(EPRO_TEXT("./script/"));
	state.script_dirs.insert(state.script_dirs.end(), std::make_move_iterator(script_subdirs.begin()), std::make_move_iterator(script_subdirs.end()));
}

epro::path_string findHeadlessScriptPath(const HeadlessRuntimeState& state, epro::path_stringview name) {
	for(const auto& base : state.script_dirs) {
		auto candidate = base + name.data();
		if(ygo::Utils::FileExists(candidate))
			return candidate;
	}
	if(ygo::Utils::FileExists(name))
		return name.data();
	return EPRO_TEXT("");
}

std::vector<char> readHeadlessScript(const HeadlessRuntimeState& state, epro::stringview name) {
	auto path = findHeadlessScriptPath(state, ygo::Utils::ToPathString(name));
	if(path.empty())
		return {};

	FileStream script{ path, FileStream::in | FileStream::binary };
	if(script.fail())
		return {};

	char bom[3]{};
	script.read(bom, 3);
	if(bom[0] != '\xEF' || bom[1] != '\xBB' || bom[2] != '\xBF')
		script.seekg(0);

	return { std::istreambuf_iterator<char>(script), std::istreambuf_iterator<char>() };
}

bool loadHeadlessScript(OCG_Duel duel, const HeadlessRuntimeState& state, epro::stringview script_name) {
	auto buffer = readHeadlessScript(state, script_name);
	if(buffer.empty())
		return false;
	return OCG_LoadScript(duel, buffer.data(), static_cast<uint32_t>(buffer.size()), script_name.data()) != 0;
}

int headlessScriptReader(void* payload, OCG_Duel duel, const char* name) {
	auto* state = static_cast<HeadlessRuntimeState*>(payload);
	if(!state)
		return 0;
	return loadHeadlessScript(duel, *state, name) ? 1 : 0;
}

void headlessCoreMessageHandler(void*, const char* string, int type) {
	if(type > 1)
		ygo::ErrorLog("{}", string ? string : "");
}

bool loadHeadlessDatabases() {
	if(!ygo::gDataManager)
		return false;

	if(ygo::Utils::FileExists(EPRO_TEXT("./cards.cdb")))
		ygo::gDataManager->LoadDB(EPRO_TEXT("./cards.cdb"));

	for(auto& file : ygo::Utils::FindFiles(EPRO_TEXT("./expansions/"), { EPRO_TEXT("cdb") }, 2)) {
		epro::path_string db = EPRO_TEXT("./expansions/") + file;
		ygo::gDataManager->LoadDB(db);
	}

	return true;
}

uint32_t resolveBanlistHash(uint32_t hash) {
	if(hash != 0)
		return hash;
	if(ygo::gdeckManager && !ygo::gdeckManager->_lfList.empty())
		return ygo::gdeckManager->_lfList.front().hash;
	return 0;
}

bool isUnknownNonzeroBanlist(uint32_t hash) {
	return hash != 0 && !isKnownBanlist(hash);
}

void applyDefaultHostInfo(ygo::HeadlessHostConfig& config) {
	auto& host_info = config.host_info;

	if(host_info.best_of == 0)
		host_info.best_of = 1;
	if(host_info.team1 == 0)
		host_info.team1 = 1;
	if(host_info.team2 == 0)
		host_info.team2 = 1;
	if(host_info.start_lp == 0)
		host_info.start_lp = 8000;
	if(host_info.start_hand == 0)
		host_info.start_hand = 5;
	if(host_info.draw_count == 0)
		host_info.draw_count = 1;
	if(host_info.sizes.main.min == 0 && host_info.sizes.main.max == 0) {
		host_info.sizes.main.min = 40;
		host_info.sizes.main.max = 60;
		host_info.sizes.extra.min = 0;
		host_info.sizes.extra.max = 15;
		host_info.sizes.side.min = 0;
		host_info.sizes.side.max = 15;
	}
	host_info.lflist = resolveBanlistHash(host_info.lflist);
}

bool validateConfig(const ygo::HeadlessHostConfig& config, std::wstring* error_message) {
	auto failMessage = [error_message](std::wstring_view message) {
		return fail(error_message, message);
	};

	if(isUnknownNonzeroBanlist(config.host_info.lflist))
		return failMessage(L"headless host banlist hash is not loaded");

	if(config.port == 0)
		return failMessage(L"headless host port must be in the range 1-65535");
	if(config.room_name.empty())
		return failMessage(L"headless host room name must not be empty");
	if(config.room_name.size() > kRoomNameLimit)
		return failMessage(L"headless host room name must be 19 characters or fewer");
	if(config.room_password.size() > kRoomPasswordLimit)
		return failMessage(L"headless host password must be 19 characters or fewer");
	if(config.room_notes.size() > kRoomNotesLimit)
		return failMessage(L"headless host notes must be 199 characters or fewer");
	if(config.host_info.mode > 3)
		return failMessage(L"headless host mode must map to a supported duel mode");
	if(config.host_info.best_of == 0)
		return failMessage(L"headless host best-of must be at least 1");
	if(config.host_info.team1 < 1 || config.host_info.team1 > 3 || config.host_info.team2 < 1 || config.host_info.team2 > 3)
		return failMessage(L"headless host team counts must be between 1 and 3");
	if(config.host_info.sizes.main.min > config.host_info.sizes.main.max ||
	   config.host_info.sizes.extra.min > config.host_info.sizes.extra.max ||
	   config.host_info.sizes.side.min > config.host_info.sizes.side.max)
		return failMessage(L"headless host deck size limits are invalid");
	if(!isKnownBanlist(config.host_info.lflist))
		return failMessage(L"headless host banlist is not loaded");
	return true;
}

bool parseModeValue(const std::string& raw_value, ygo::HeadlessHostConfig& config, std::wstring* error_message) {
	const auto mode = normalizeKey(raw_value);
	if(mode == "single") {
		config.host_info.mode = MODE_SINGLE;
		return true;
	}
	if(mode == "match") {
		config.host_info.mode = MODE_MATCH;
		return true;
	}
	if(mode == "tag") {
		config.host_info.mode = MODE_TAG;
		return true;
	}
	if(mode == "rush") {
		config.host_info.mode = MODE_SINGLE;
		config.host_info.duel_flag_low = DUEL_MODE_RUSH;
		return true;
	}
	return fail(error_message, L"headless host mode must be one of single, match, tag, or rush");
}

template<typename T>
bool readJsonInteger(const nlohmann::json& object, const char* key, T& destination, std::wstring* error_message) {
	const auto it = object.find(key);
	if(it == object.end())
		return true;
	if(!it->is_number_integer() && !it->is_number_unsigned() && !it->is_boolean())
		return fail(error_message, L"headless host config values must be numeric");

	if constexpr(std::is_signed_v<T>) {
		long long parsed = 0;
		if(it->is_boolean())
			parsed = it->get<bool>() ? 1 : 0;
		else if(it->is_number_unsigned()) {
			const auto unsigned_value = it->get<uint64_t>();
			if(unsigned_value > static_cast<uint64_t>(std::numeric_limits<long long>::max()))
				return fail(error_message, L"headless host config value is out of range");
			parsed = static_cast<long long>(unsigned_value);
		} else {
			parsed = it->get<int64_t>();
		}
		if(parsed < static_cast<long long>(std::numeric_limits<T>::min()) || parsed > static_cast<long long>(std::numeric_limits<T>::max()))
			return fail(error_message, L"headless host config value is out of range");
		destination = static_cast<T>(parsed);
		return true;
	}

	unsigned long long parsed = 0;
	if(it->is_boolean())
		parsed = it->get<bool>() ? 1ull : 0ull;
	else if(it->is_number_unsigned())
		parsed = it->get<uint64_t>();
	else {
		const auto signed_value = it->get<int64_t>();
		if(signed_value < 0)
			return fail(error_message, L"headless host config value is out of range");
		parsed = static_cast<unsigned long long>(signed_value);
	}
	if(parsed > static_cast<unsigned long long>(std::numeric_limits<T>::max()))
		return fail(error_message, L"headless host config value is out of range");
	destination = static_cast<T>(parsed);
	return true;
}

bool loadHostInfoConfig(const epro::path_string& path, ygo::HeadlessHostConfig& config, std::wstring* error_message) {
	if(!ygo::Utils::FileExists(path)) {
		if(error_message)
			*error_message = L"headless host config file does not exist: " + ygo::Utils::ToUnicodeIfNeeded(path);
		return false;
	}

	FileStream file{ path, FileStream::in };
	if(file.fail()) {
		if(error_message)
			*error_message = L"failed to open headless host config file: " + ygo::Utils::ToUnicodeIfNeeded(path);
		return false;
	}

	nlohmann::json json;
	try {
		file >> json;
	}
	catch(const std::exception&) {
		if(error_message)
			*error_message = L"failed to parse headless host config file as JSON: " + ygo::Utils::ToUnicodeIfNeeded(path);
		return false;
	}

	if(!json.is_object())
		return fail(error_message, L"headless host config file must contain a JSON object");

	static constexpr std::string_view kAllowedKeys[] = {
		"lflist", "rule", "mode", "duelrule", "nocheckdeckcontent", "noshuffledeck",
		"startlp", "starthand", "drawcount", "timelimit", "duelflaghigh", "duelflaglow",
		"team1", "team2", "forbiddentypes", "extrarules", "sizes"
	};

	for(const auto& item : json.items()) {
		const auto key = normalizeKey(item.key());
		if(std::find(std::begin(kAllowedKeys), std::end(kAllowedKeys), key) == std::end(kAllowedKeys))
			return fail(error_message, L"headless host config file contains an unsupported key");
	}

	if(!readJsonInteger(json, "lflist", config.host_info.lflist, error_message))
		return false;
	if(!readJsonInteger(json, "rule", config.host_info.rule, error_message))
		return false;
	if(!readJsonInteger(json, "mode", config.host_info.mode, error_message))
		return false;
	if(config.host_info.mode > 3)
		return fail(error_message, L"headless host config mode must be one of the supported duel modes");
	if(!readJsonInteger(json, "duel_rule", config.host_info.duel_rule, error_message))
		return false;
	if(!readJsonInteger(json, "no_check_deck_content", config.host_info.no_check_deck_content, error_message))
		return false;
	if(!readJsonInteger(json, "no_shuffle_deck", config.host_info.no_shuffle_deck, error_message))
		return false;
	if(!readJsonInteger(json, "start_lp", config.host_info.start_lp, error_message))
		return false;
	if(!readJsonInteger(json, "start_hand", config.host_info.start_hand, error_message))
		return false;
	if(!readJsonInteger(json, "draw_count", config.host_info.draw_count, error_message))
		return false;
	if(!readJsonInteger(json, "time_limit", config.host_info.time_limit, error_message))
		return false;
	if(!readJsonInteger(json, "duel_flag_high", config.host_info.duel_flag_high, error_message))
		return false;
	if(!readJsonInteger(json, "duel_flag_low", config.host_info.duel_flag_low, error_message))
		return false;
	if(!readJsonInteger(json, "team1", config.host_info.team1, error_message))
		return false;
	if(!readJsonInteger(json, "team2", config.host_info.team2, error_message))
		return false;
	if(config.host_info.team1 < 1 || config.host_info.team1 > 3 || config.host_info.team2 < 1 || config.host_info.team2 > 3)
		return fail(error_message, L"headless host config team counts must be between 1 and 3");
	if(!readJsonInteger(json, "forbiddentypes", config.host_info.forbiddentypes, error_message))
		return false;
	if(!readJsonInteger(json, "extra_rules", config.host_info.extra_rules, error_message))
		return false;

	const auto sizes_it = json.find("sizes");
	if(sizes_it != json.end()) {
		if(!sizes_it->is_object())
			return fail(error_message, L"headless host config field 'sizes' must be an object");

		static constexpr std::string_view kSizeKeys[] = { "main", "extra", "side" };
		for(const auto& size_item : sizes_it->items()) {
			const auto normalized = normalizeKey(size_item.key());
			if(std::find(std::begin(kSizeKeys), std::end(kSizeKeys), normalized) == std::end(kSizeKeys))
				return fail(error_message, L"headless host config field 'sizes' contains an unsupported section");
		}

		auto read_size = [&](const char* key, auto& limits) {
			auto item = sizes_it->find(key);
			if(item == sizes_it->end())
				return true;
			if(!item->is_object())
				return fail(error_message, L"headless host config size section must be an object");
			if(!readJsonInteger(*item, "min", limits.min, error_message))
				return false;
			if(!readJsonInteger(*item, "max", limits.max, error_message))
				return false;
			return true;
		};

		if(!read_size("main", config.host_info.sizes.main))
			return false;
		if(!read_size("extra", config.host_info.sizes.extra))
			return false;
		if(!read_size("side", config.host_info.sizes.side))
			return false;
	}

	return true;
}

bool parseLaunchOptions(const args_t& args, LaunchOptions& options, std::wstring* error_message) {
	if(!args[LAUNCH_PARAM::HOST_HEADLESS].enabled)
		return fail(error_message, L"headless host mode requires --host-headless");

	std::vector<std::string> tokens;
	if(!collectLaunchTokens(args[LAUNCH_PARAM::HOST_HEADLESS], tokens, error_message))
		return false;
	if(tokens.empty())
		return fail(error_message, L"headless host mode requires Name, Port, and Mode arguments");

	for(const auto& token : tokens) {
		std::string key;
		std::string value;
		if(!splitKeyValue(token, key, value))
			return fail(error_message, L"headless host arguments must be supplied as Key=Value pairs");

		const auto normalized_key = normalizeKey(key);
		if(normalized_key == "name") {
			if(options.has_name)
				return fail(error_message, L"headless host name was specified more than once");
			options.has_name = true;
			options.name = BufferIO::DecodeUTF8(value);
			continue;
		}
		if(normalized_key == "port") {
			if(options.has_port)
				return fail(error_message, L"headless host port was specified more than once");
			uint32_t parsed = 0;
			if(!parseIntegerValue(value, parsed) || parsed < 1 || parsed > 65535)
				return fail(error_message, L"headless host port must be in the range 1-65535");
			options.has_port = true;
			options.port = static_cast<uint16_t>(parsed);
			continue;
		}
		if(normalized_key == "password") {
			if(options.has_password)
				return fail(error_message, L"headless host password was specified more than once");
			options.has_password = true;
			options.password = BufferIO::DecodeUTF8(value);
			continue;
		}
		if(normalized_key == "mode") {
			if(options.has_mode)
				return fail(error_message, L"headless host mode was specified more than once");
			if(value.empty())
				return fail(error_message, L"headless host mode must not be empty");
			options.has_mode = true;
			options.mode = std::move(value);
			continue;
		}
		if(normalized_key == "bestof") {
			if(options.has_best_of)
				return fail(error_message, L"headless host best-of was specified more than once");
			int parsed = 0;
			if(!parseIntegerValue(value, parsed) || parsed < 1)
				return fail(error_message, L"headless host best-of must be at least 1");
			options.has_best_of = true;
			options.best_of = parsed;
			continue;
		}
		if(normalized_key == "configfile") {
			if(options.has_config_file)
				return fail(error_message, L"headless host config file was specified more than once");
			if(value.empty())
				return fail(error_message, L"headless host config file path must not be empty");
			options.has_config_file = true;
			options.config_file = std::move(value);
			continue;
		}
		return fail(error_message, L"headless host arguments contain an unsupported key");
	}

	if(!options.has_name)
		return fail(error_message, L"headless host Name must be provided");
	if(!options.has_port)
		return fail(error_message, L"headless host Port must be provided");
	if(!options.has_mode)
		return fail(error_message, L"headless host Mode must be provided");
	return true;
}

bool prepareHeadlessHostConfig(const args_t& args, ygo::HeadlessHostConfig& config, std::wstring* error_message) {
	LaunchOptions options;
	if(!parseLaunchOptions(args, options, error_message))
		return false;

	if(options.has_config_file) {
		const auto config_path = ygo::Utils::ToPathString(options.config_file);
		if(!loadHostInfoConfig(config_path, config, error_message))
			return false;
	}

	config.room_name = std::move(options.name);
	config.room_password = options.has_password ? std::move(options.password) : std::wstring{};
	config.port = options.port;
	config.host_info.best_of = options.best_of;
	if(!parseModeValue(options.mode, config, error_message))
		return false;
	return true;
}

} // namespace

namespace ygo {

bool InitializeHeadlessDuelRuntime(std::wstring* error_message) {
	auto& state = headlessRuntimeState();
	if(state.initialized)
		return true;

#ifdef YGOPRO_BUILD_DLL
	state.ocgcore = LoadOCGcore(Utils::GetWorkingDirectory());
	if(!state.ocgcore) {
		const auto expansion_path = epro::format(EPRO_TEXT("{}/expansions/"), Utils::GetWorkingDirectory());
		state.ocgcore = LoadOCGcore(expansion_path);
	}
	if(!state.ocgcore)
		return fail(error_message, L"failed to load ocgcore for headless hosting");
#endif

	if(!state.data_manager)
		state.data_manager = std::make_unique<DataManager>();
	gDataManager = state.data_manager.get();

	bool strings_loaded = gDataManager->LoadStrings(EPRO_TEXT("./config/strings.conf"));
	strings_loaded = gDataManager->LoadStrings(EPRO_TEXT("./expansions/strings.conf")) || strings_loaded;
	if(!strings_loaded)
		return fail(error_message, L"failed to load strings for headless hosting runtime");

	if(!loadHeadlessDatabases())
		return fail(error_message, L"failed to load card databases for headless hosting runtime");

	if(!gdeckManager) {
		static std::unique_ptr<DeckManager> deck_manager_storage;
		deck_manager_storage = std::make_unique<DeckManager>();
		gdeckManager = deck_manager_storage.get();
		gdeckManager->LoadLFList();
	}

	populateHeadlessScriptDirectories(state);
	if(readHeadlessScript(state, "constant.lua").empty())
		return fail(error_message, L"failed to locate constant.lua for headless hosting runtime");
	if(readHeadlessScript(state, "utility.lua").empty())
		return fail(error_message, L"failed to locate utility.lua for headless hosting runtime");

	state.initialized = true;
	return true;
}

OCG_Duel SetupHeadlessDuelRuntime(OCG_DuelOptions opts, std::wstring* error_message) {
	if(!InitializeHeadlessDuelRuntime(error_message))
		return nullptr;

	auto& state = headlessRuntimeState();
	opts.cardReader = DataManager::CardReader;
	opts.payload1 = gDataManager;
	opts.scriptReader = headlessScriptReader;
	opts.payload2 = &state;
	opts.logHandler = headlessCoreMessageHandler;
	opts.payload3 = nullptr;
	opts.enableUnsafeLibraries = 1;

	OCG_Duel duel = nullptr;
	const auto status = OCG_CreateDuel(&duel, &opts);
	if(status != OCG_DUEL_CREATION_SUCCESS || duel == nullptr) {
		fail(error_message, L"failed to create duel in headless runtime");
		return nullptr;
	}

	if(!loadHeadlessScript(duel, state, "constant.lua") || !loadHeadlessScript(duel, state, "utility.lua")) {
		OCG_DestroyDuel(duel);
		fail(error_message, L"failed to load base scripts in headless runtime");
		return nullptr;
	}

	for(const auto& script : state.init_scripts) {
		FileStream file{ script, FileStream::in | FileStream::binary };
		if(file.fail())
			continue;
		std::vector<char> init_buffer{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
		if(init_buffer.empty())
			continue;
		const auto script_name = Utils::ToUTF8IfNeeded(script);
		if(!OCG_LoadScript(duel, init_buffer.data(), static_cast<uint32_t>(init_buffer.size()), script_name.data())) {
			OCG_DestroyDuel(duel);
			fail(error_message, L"failed to load init.lua in headless runtime");
			return nullptr;
		}
	}

	return duel;
}

bool ValidateHeadlessHostConfig(const HeadlessHostConfig& config, std::wstring* error_message) {
	auto normalized = config;
	applyDefaultHostInfo(normalized);
	return validateConfig(normalized, error_message);
}

HeadlessHostResult StartHeadlessHost(const HeadlessHostConfig& config) {
	HeadlessHostResult result;
	auto normalized = config;
	applyDefaultHostInfo(normalized);
	if(!validateConfig(normalized, &result.error_message)) {
		result.status = HeadlessHostStatus::INVALID_CONFIG;
		return result;
	}

	if(gGameConfig) {
		ApplySharedRoomConfig({
			normalized.port,
			normalized.room_name,
			normalized.room_password,
			true,
		});
	}

	normalized.host_info.handshake = SERVER_HANDSHAKE;
	normalized.host_info.version = { EXPAND_VERSION(CLIENT_VERSION) };

	if(!StartHostServer(normalized.port, &result.error_message)) {
		result.status = HeadlessHostStatus::SERVER_START_FAILED;
		return result;
	}
	auto duel_mode = std::make_unique<GenericDuel>(normalized.host_info.team1, normalized.host_info.team2,
		!!(normalized.host_info.duel_flag_low & DUEL_RELAY), normalized.host_info.best_of);
	duel_mode->host_info = normalized.host_info;
	BufferIO::CopyStr(normalized.room_name.c_str(), duel_mode->name, 20);
	BufferIO::CopyStr(normalized.room_password.c_str(), duel_mode->pass, 20);
	duel_mode->etimer = event_new(NetServer::net_evbase, 0, EV_TIMEOUT | EV_PERSIST, GenericDuel::GenericTimer, duel_mode.get());
	if(!duel_mode->etimer) {
		NetServer::StopServer();
		result.status = HeadlessHostStatus::SERVER_START_FAILED;
		result.error_message = L"failed to initialize the headless host room timer";
		return result;
	}
	timeval timeout = { 1, 0 };
	event_add(duel_mode->etimer, &timeout);
	NetServer::duel_mode = duel_mode.release();
	if(!NetServer::StartBroadcast()) {
		NetServer::StopServer();
		result.status = HeadlessHostStatus::BROADCAST_START_FAILED;
		result.error_message = L"failed to start LAN broadcast discovery";
		return result;
	}

	result.status = HeadlessHostStatus::OK;
	return result;
}

} // namespace ygo

int headless_host_main(const args_t& args) {
	static std::unique_ptr<ygo::GameConfig> game_config_storage;
	static std::unique_ptr<ygo::DeckManager> deck_manager_storage;
	game_config_storage = std::make_unique<ygo::GameConfig>();
	ygo::gGameConfig = game_config_storage.get();
	deck_manager_storage = std::make_unique<ygo::DeckManager>();
	ygo::gdeckManager = deck_manager_storage.get();
	ygo::gdeckManager->LoadLFList();

	std::wstring runtime_error;
	if(!ygo::InitializeHeadlessDuelRuntime(&runtime_error)) {
		emitErrorEvent(runtime_error);
		std::fwprintf(stderr, L"%ls\n", runtime_error.c_str());
		return EXIT_FAILURE;
	}

	ygo::HeadlessHostConfig config;
	applyDefaultHostInfo(config);

	std::wstring error_message;
	if(!prepareHeadlessHostConfig(args, config, &error_message)) {
		emitErrorEvent(error_message);
		std::fwprintf(stderr, L"%ls\n", error_message.c_str());
		return EXIT_FAILURE;
	}

	if(!ygo::ValidateHeadlessHostConfig(config, &error_message)) {
		emitErrorEvent(error_message);
		std::fwprintf(stderr, L"%ls\n", error_message.c_str());
		return EXIT_FAILURE;
	}

	if(!installTerminationSignalHandlers()) {
		error_message = L"failed to install termination signal handlers";
		emitErrorEvent(error_message);
		std::fwprintf(stderr, L"%ls\n", error_message.c_str());
		return EXIT_FAILURE;
	}

	auto result = ygo::StartHeadlessHost(config);
	if(result.status != ygo::HeadlessHostStatus::OK) {
		emitErrorEvent(result.error_message);
		std::fwprintf(stderr, L"%ls\n", result.error_message.c_str());
		return EXIT_FAILURE;
	}

	HeadlessRoomEventMonitor monitor{ config.port };
	monitor.emitRoomStarted();
	bool stop_requested = false;
	bool shutdown_completed = false;
	constexpr std::wstring_view kStoppedDisconnectReason = L"Room closed: host process stopped before duel completed.";

	while(!monitor.closed()) {
	#if !defined(_WIN32)
		if(g_termination_requested && !stop_requested) {
			stop_requested = true;
			ygo::NetServer::DisconnectAllPlayersWithReason(kStoppedDisconnectReason);
			monitor.requestStop();
			ygo::NetServer::StopServer();
		}
	#endif
		if(monitor.shouldRequestStop() && !stop_requested) {
			stop_requested = true;
			monitor.requestStop();
			ygo::NetServer::StopServer();
		}
		monitor.poll();
		if(monitor.closed())
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	if(!stop_requested)
		ygo::NetServer::StopServer();

	shutdown_completed = waitForServerShutdown(std::chrono::milliseconds(2000));
	if(!shutdown_completed) {
		error_message = L"headless host did not fully release server resources before exit";
		emitErrorEvent(error_message);
		std::fwprintf(stderr, L"%ls\n", error_message.c_str());
		return EXIT_FAILURE;
	}

	if(!verifyPortReusable(config.port, &error_message)) {
		emitErrorEvent(error_message);
		std::fwprintf(stderr, L"%ls\n", error_message.c_str());
		return EXIT_FAILURE;
	}

	if(monitor.hasRuntimeError())
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}