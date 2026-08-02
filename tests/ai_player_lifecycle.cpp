#include <cassert>
#include <vector>
#include "windbot_panel.h"

int main() {
	using namespace ygo;
	std::vector<AiPlayerProcessRecord> launches{
		{ 0, {}, L"[AI] Same", 100 },
		{ 0, {}, L"[AI] Same", 200 },
		{ 1, {}, L"[AI] Other", 300 },
	};

	// Duplicate configured names correlate one-at-a-time in launch order.
	assert(FindPendingAiPlayerLaunch(launches, L"[AI] Same") == 0);
	launches[0].participant_joined = true;
	assert(FindPendingAiPlayerLaunch(launches, L"[AI] Same") == 1);

	// A failed seat is isolated and cannot consume another seat join.
	launches[1].failure_reported = true;
	assert(FindPendingAiPlayerLaunch(launches, L"[AI] Same") == AI_PLAYER_PROCESS_NOT_FOUND);
	assert(FindPendingAiPlayerLaunch(launches, L"[AI] Other") == 2);
	assert(EvaluateAiPlayerPending(true, false, AI_PLAYER_JOIN_TIMEOUT_MS - 1)
	       == AiPlayerPendingDecision::WAITING);
	assert(EvaluateAiPlayerPending(false, false, 0) == AiPlayerPendingDecision::PROCESS_EXITED);
	assert(EvaluateAiPlayerPending(true, false, AI_PLAYER_JOIN_TIMEOUT_MS)
	       == AiPlayerPendingDecision::TIMED_OUT);
	assert(EvaluateAiPlayerPending(false, true, AI_PLAYER_JOIN_TIMEOUT_MS)
	       == AiPlayerPendingDecision::JOINED);

	std::vector<uint32_t> available_windbots{ 0u, 2u, 7u };

	// preferred AI available -> preferred wins.
	const auto preferred_selected = ResolveLocalAiDefaultEngineSelection(
		available_windbots, 3u, 1u, 2);
	assert(preferred_selected.valid);
	assert(preferred_selected.kind == LocalAiEngineKind::AI_PLAYER);
	assert(preferred_selected.index == 1u);

	// preferred AI unavailable -> saved valid AI selection is restored.
	const int saved_ai = EncodeLocalAiEngineSelectionForConfig({ LocalAiEngineKind::AI_PLAYER, 2u, true });
	const auto saved_ai_selected = ResolveLocalAiDefaultEngineSelection(
		available_windbots, 3u, 9u, saved_ai);
	assert(saved_ai_selected.valid);
	assert(saved_ai_selected.kind == LocalAiEngineKind::AI_PLAYER);
	assert(saved_ai_selected.index == 2u);

	// preferred AI invalid and saved value invalid -> falls back to first available WindBot.
	const auto fallback_windbot = ResolveLocalAiDefaultEngineSelection(
		available_windbots, 1u, 3u, 5);
	assert(fallback_windbot.valid);
	assert(fallback_windbot.kind == LocalAiEngineKind::WINDBOT);
	assert(fallback_windbot.index == 0u);

	// saved AI selection remains selected when valid and no preferred default is configured.
	const auto saved_ai_again = ResolveLocalAiDefaultEngineSelection(
		available_windbots, 3u, std::nullopt, saved_ai);
	assert(saved_ai_again.valid);
	assert(saved_ai_again.kind == LocalAiEngineKind::AI_PLAYER);
	assert(saved_ai_again.index == 2u);

	// corrupted/unknown saved value falls back safely.
	const auto corrupted_saved = ResolveLocalAiDefaultEngineSelection(
		available_windbots, 0u, std::nullopt, -123456789);
	assert(corrupted_saved.valid);
	assert(corrupted_saved.kind == LocalAiEngineKind::WINDBOT);
	assert(corrupted_saved.index == 0u);

	// Preferred selection scan identifies first preferred and flags multi-preferred configs.
	std::vector<AiPlayerEngineEntry> ai_entries(3);
	ai_entries[0].preferred_default = true;
	ai_entries[1].preferred_default = true;
	const auto preferred_scan = FindPreferredAiPlayerSelection(&ai_entries);
	assert(preferred_scan.index.has_value());
	assert(preferred_scan.index.value() == 0u);
	assert(preferred_scan.multiple);
}
