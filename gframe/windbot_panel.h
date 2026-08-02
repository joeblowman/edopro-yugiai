#ifndef WINDBOT_PANEL_H
#define WINDBOT_PANEL_H

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include "windbot.h"
#include "config.h"
#if EDOPRO_LINUX || EDOPRO_MACOS
#include <sys/types.h>
#endif

namespace irr {
namespace gui {
class IGUIWindow;
class IGUIComboBox;
class IGUICheckBox;
class IGUIStaticText;
class IGUIButton;
}
}

namespace ygo {

struct LocalAiEngineSelection {
	LocalAiEngineKind kind{ LocalAiEngineKind::WINDBOT };
	uint32_t index{};
	bool valid{};
};

// WindBot item data remains its legacy vector index. The high bit tags
// AI-player indices, keeping both index spaces stable and non-overlapping.
inline constexpr uint32_t AI_PLAYER_ENGINE_ITEM_TAG = uint32_t{ 1 } << 31;
inline constexpr uint32_t LOCAL_AI_ENGINE_INDEX_MASK = AI_PLAYER_ENGINE_ITEM_TAG - 1;

constexpr uint32_t EncodeAiPlayerEngineItemData(uint32_t index) {
	return AI_PLAYER_ENGINE_ITEM_TAG | index;
}

constexpr LocalAiEngineSelection DecodeLocalAiEngineItemData(uint32_t data) {
	return {
		(data & AI_PLAYER_ENGINE_ITEM_TAG) ? LocalAiEngineKind::AI_PLAYER : LocalAiEngineKind::WINDBOT,
		data & LOCAL_AI_ENGINE_INDEX_MASK,
		true
	};
}

static_assert(DecodeLocalAiEngineItemData(0).kind == LocalAiEngineKind::WINDBOT);
static_assert(DecodeLocalAiEngineItemData(42).index == 42);
static_assert(DecodeLocalAiEngineItemData(EncodeAiPlayerEngineItemData(42)).kind == LocalAiEngineKind::AI_PLAYER);
static_assert(DecodeLocalAiEngineItemData(EncodeAiPlayerEngineItemData(42)).index == 42);

constexpr int EncodeLocalAiEngineSelectionForConfig(const LocalAiEngineSelection& selection) {
	if(!selection.valid)
		return 0;
	if(selection.kind == LocalAiEngineKind::AI_PLAYER)
		return static_cast<int>(EncodeAiPlayerEngineItemData(selection.index));
	return static_cast<int>(selection.index);
}

constexpr LocalAiEngineSelection DecodeLocalAiEngineSelectionFromConfig(int saved_selection) {
	if(saved_selection >= 0)
		return { LocalAiEngineKind::WINDBOT, static_cast<uint32_t>(saved_selection), true };
	return DecodeLocalAiEngineItemData(static_cast<uint32_t>(saved_selection));
}

inline bool IsValidLocalAiEngineSelection(const LocalAiEngineSelection& selection,
	                                      const std::vector<uint32_t>& available_windbot_indices,
	                                      size_t available_ai_player_count) {
	if(!selection.valid)
		return false;
	if(selection.kind == LocalAiEngineKind::AI_PLAYER)
		return selection.index < available_ai_player_count;
	return std::find(available_windbot_indices.begin(), available_windbot_indices.end(), selection.index)
		!= available_windbot_indices.end();
}

struct PreferredAiPlayerSelection {
	std::optional<uint32_t> index;
	bool multiple{};
};

inline PreferredAiPlayerSelection FindPreferredAiPlayerSelection(const std::vector<AiPlayerEngineEntry>* ai_players) {
	if(!ai_players)
		return {};
	PreferredAiPlayerSelection preferred{};
	for(uint32_t index = 0; index < ai_players->size(); ++index) {
		if(!(*ai_players)[index].preferred_default)
			continue;
		if(!preferred.index.has_value())
			preferred.index = index;
		else
			preferred.multiple = true;
	}
	return preferred;
}

inline LocalAiEngineSelection ResolveLocalAiDefaultEngineSelection(
	const std::vector<uint32_t>& available_windbot_indices,
	size_t available_ai_player_count,
	std::optional<uint32_t> preferred_ai_player,
	int saved_selection) {
	if(preferred_ai_player.has_value() && preferred_ai_player.value() < available_ai_player_count)
		return { LocalAiEngineKind::AI_PLAYER, preferred_ai_player.value(), true };

	const auto decoded_saved = DecodeLocalAiEngineSelectionFromConfig(saved_selection);
	if(IsValidLocalAiEngineSelection(decoded_saved, available_windbot_indices, available_ai_player_count))
		return decoded_saved;

	if(!available_windbot_indices.empty())
		return { LocalAiEngineKind::WINDBOT, available_windbot_indices.front(), true };

	if(available_ai_player_count > 0)
		return { LocalAiEngineKind::AI_PLAYER, 0u, true };

	return {};
}

inline constexpr uint32_t AI_PLAYER_JOIN_TIMEOUT_MS = 15000;
inline constexpr std::wstring_view AI_PLAYER_FAILURE_MESSAGE =
	L"The AI player failed to start or join the room. The seat remains open.";

enum class AiPlayerPendingDecision : uint8_t {
	WAITING,
	JOINED,
	PROCESS_EXITED,
	TIMED_OUT,
};

constexpr AiPlayerPendingDecision EvaluateAiPlayerPending(bool process_running,
                                                          bool participant_joined,
                                                          uint32_t elapsed_ms) {
	if(participant_joined)
		return AiPlayerPendingDecision::JOINED;
	if(!process_running)
		return AiPlayerPendingDecision::PROCESS_EXITED;
	if(elapsed_ms >= AI_PLAYER_JOIN_TIMEOUT_MS)
		return AiPlayerPendingDecision::TIMED_OUT;
	return AiPlayerPendingDecision::WAITING;
}

static_assert(EvaluateAiPlayerPending(true, false, AI_PLAYER_JOIN_TIMEOUT_MS - 1) == AiPlayerPendingDecision::WAITING);
static_assert(EvaluateAiPlayerPending(true, true, AI_PLAYER_JOIN_TIMEOUT_MS) == AiPlayerPendingDecision::JOINED);
static_assert(EvaluateAiPlayerPending(false, false, 0) == AiPlayerPendingDecision::PROCESS_EXITED);
static_assert(EvaluateAiPlayerPending(true, false, AI_PLAYER_JOIN_TIMEOUT_MS) == AiPlayerPendingDecision::TIMED_OUT);

// One record per successful launch; duplicate engine indices represent
// independent seats and retain distinct native process handles.
struct AiPlayerProcessRecord {
	uint32_t engine_index{};
	LocalAiProcessHandle process_handle;
	std::wstring expected_name;
	uint32_t launched_at_ms{};
	bool participant_joined{};
	bool failure_reported{};
	AiPlayerPendingDecision observed_failure{ AiPlayerPendingDecision::WAITING };
};

inline constexpr size_t AI_PLAYER_PROCESS_NOT_FOUND = static_cast<size_t>(-1);

// Lobby updates carry names but no process identifier. Scanning insertion order
// deterministically assigns duplicate names to the oldest unresolved launch.
inline size_t FindPendingAiPlayerLaunch(const std::vector<AiPlayerProcessRecord>& processes,
                                        epro::wstringview name) {
	for(size_t index = 0; index < processes.size(); ++index) {
		const auto& process = processes[index];
		if(!process.participant_joined && !process.failure_reported && process.expected_name == name)
			return index;
	}
	return AI_PLAYER_PROCESS_NOT_FOUND;
}

struct WindBotPanel {
	std::vector<WindBot> bots;
	const std::vector<AiPlayerEngineEntry>* aiPlayerEngines{};
	std::vector<AiPlayerProcessRecord> aiPlayerProcesses;
#if EDOPRO_LINUX || EDOPRO_MACOS
	std::vector<pid_t> windbotsPids;
#endif

	WindBot* genericEngine{};

	~WindBotPanel();

	irr::gui::IGUIWindow* window;
	irr::gui::IGUIComboBox* cbBotDeck;
	irr::gui::IGUIComboBox* cbBotEngine;
	irr::gui::IGUICheckBox* chkThrowRock;
	irr::gui::IGUICheckBox* chkMute;
	irr::gui::IGUIStaticText* stBotEngine;
	irr::gui::IGUIStaticText* deckProperties;
	irr::gui::IGUIButton* btnAdd;
	irr::gui::IGUIButton* btnCommand;

	int CurrentIndex();
	int CurrentEngine();
	LocalAiEngineSelection CurrentEngineSelection();
	void Refresh(int filterMasterRule = 0, int lastIndex = 0);
	void UpdateDescription();
	void UpdateEngine();
	bool LaunchSelected(int port, epro::wstringview pass, uint32_t now_ms);
	bool UpdatePendingAiPlayers(uint32_t now_ms);
	void NotifyParticipantJoined(epro::wstringview name);
	void ClearAiPlayerProcesses();
	std::wstring GetParameters(int port, epro::wstringview pass);
private:
	std::mutex aiPlayerProcessesMutex;
	int genericEngineIdx{ -1 };
};

}

#endif
