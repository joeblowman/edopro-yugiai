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
}
