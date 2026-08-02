#include "windbot_panel.h"
#include <IGUIComboBox.h>
#include <IGUIStaticText.h>
#include <IGUICheckBox.h>
#include "config.h"
#include "data_manager.h"
#include "deck_manager.h"
#include "fmt.h"
#include "logging.h"

namespace ygo {

namespace {
constexpr std::wstring_view kAiPlayerHostVisibleFailureMessage =
	L"AI-player launch failed: process exited or did not join within 15s. Host seat remains open.";

const char* AiPlayerPendingFailureReason(AiPlayerPendingDecision decision) {
	switch(decision) {
	case AiPlayerPendingDecision::PROCESS_EXITED:
		return "process exited before joining";
	case AiPlayerPendingDecision::TIMED_OUT:
		return "join timed out after 15s";
	default:
		return "unknown";
	}
}
}

WindBotPanel::~WindBotPanel() {
	ClearAiPlayerProcesses();
}

int WindBotPanel::CurrentIndex() {
	int selected = cbBotDeck->getSelected();
	return selected >= 0 ? cbBotDeck->getItemData(selected) : selected;
}

int WindBotPanel::CurrentEngine() {
	const auto selection = CurrentEngineSelection();
	return selection.valid && selection.kind == LocalAiEngineKind::WINDBOT
		? static_cast<int>(selection.index)
		: -1;
}

LocalAiEngineSelection WindBotPanel::CurrentEngineSelection() {
	const int selected = cbBotEngine->getSelected();
	if(selected < 0)
		return {};
	return DecodeLocalAiEngineItemData(cbBotEngine->getItemData(selected));
}

void WindBotPanel::Refresh(int filterMasterRule, int lastIndex) {
	int oldIndex = CurrentIndex();
	int lastDeckIndex = oldIndex >= 0 ? oldIndex : (lastIndex >= 0 ? lastIndex : -1);
	cbBotDeck->clear();
	cbBotEngine->clear();
	genericEngineIdx = -1;
	std::vector<uint32_t> availableWindBotIndices{};
	int i = 0;
	for (const auto& bot : bots) {
		if(genericEngine == &bot)
			continue;
		if (filterMasterRule == 0 || bot.masterRules.find(filterMasterRule) != bot.masterRules.end()) {
			int newIndex = cbBotDeck->addItem(bot.name.data(), i);
			cbBotEngine->addItem(bot.name.data(), i);
			availableWindBotIndices.push_back(static_cast<uint32_t>(i));
			if(i == lastDeckIndex) {
				cbBotDeck->setSelected(newIndex);
			}
		}
		++i;
	}
	if(genericEngine) {
		genericEngineIdx = cbBotEngine->addItem(genericEngine->name.data(), i);
		availableWindBotIndices.push_back(static_cast<uint32_t>(i));
	}
	const auto preferred_ai = FindPreferredAiPlayerSelection(aiPlayerEngines);
	if(preferred_ai.multiple) {
		ErrorLog("Multiple AI players are configured with preferredDefault=true; selecting the first valid entry.");
	}
	if(aiPlayerEngines) {
		for(uint32_t aiPlayerIndex = 0; aiPlayerIndex < aiPlayerEngines->size(); ++aiPlayerIndex) {
			const auto& aiPlayer = (*aiPlayerEngines)[aiPlayerIndex];
			cbBotEngine->addItem(aiPlayer.label.data(), EncodeAiPlayerEngineItemData(aiPlayerIndex));
		}
	}

	const auto resolved_engine = ResolveLocalAiDefaultEngineSelection(
		availableWindBotIndices,
		aiPlayerEngines ? aiPlayerEngines->size() : 0,
		preferred_ai.index,
		lastIndex);
	if(resolved_engine.valid) {
		const auto resolved_data = resolved_engine.kind == LocalAiEngineKind::AI_PLAYER
			? EncodeAiPlayerEngineItemData(resolved_engine.index)
			: resolved_engine.index;
		for(irr::u32 engine_item = 0; engine_item < cbBotEngine->getItemCount(); ++engine_item) {
			if(cbBotEngine->getItemData(engine_item) == resolved_data) {
				cbBotEngine->setSelected(static_cast<int>(engine_item));
				break;
			}
		}
	}

	for(auto& file : Utils::FindFiles(DeckManager::GetDeckFolder(), { EPRO_TEXT("ydk") })) {
		file.erase(file.size() - 4);
		cbBotDeck->addItem(Utils::ToUnicodeIfNeeded(file).data(), i);
		i++;
	}
	UpdateDescription();
}

void WindBotPanel::UpdateDescription() {
	int index = CurrentIndex();
	if (index < 0) {
		deckProperties->setText(L"");
		return;
	}
	if (index >= (int)(bots.size() - (genericEngine != nullptr)) || index != CurrentEngine()) {
		deckProperties->setText(L"???");
		return;
	}
	auto& bot = bots[index];
	std::wstring params = [&bot] {
		if(bot.difficulty != 0)
			return epro::format(gDataManager->GetSysString(2055), bot.difficulty);
		return std::wstring{ gDataManager->GetSysString(2056) };
	}();
	params.push_back(L'\n');
	if (bot.masterRules.size()) {
		std::wstring mr;
		for (auto rule : bot.masterRules) {
			if (mr.size())
				mr.push_back(L',');
			mr.append(epro::to_wstring(rule));
		}
		params.append(epro::format(gDataManager->GetSysString(2057), mr)).push_back(L'\n');
	}
	deckProperties->setText(params.data());
}

void WindBotPanel::UpdateEngine() {
	// Keep an explicitly selected AI player while choosing its override deck.
	// WindBot and generic-engine auto-selection behavior remains unchanged.
	if(CurrentEngineSelection().kind == LocalAiEngineKind::AI_PLAYER) {
		UpdateDescription();
		return;
	}
	int index = CurrentIndex();
	if(index >= (int)bots.size()) {
		if(genericEngineIdx != -1)
			cbBotEngine->setSelected(genericEngineIdx);
		else
			cbBotEngine->setSelected(0);
	} else {
		cbBotEngine->setSelected(cbBotDeck->getSelected());
	}
	UpdateDescription();
}

bool WindBotPanel::LaunchSelected(int port, epro::wstringview pass, uint32_t now_ms) {
	const auto selection = CurrentEngineSelection();
	if(!selection.valid)
		return false;

	int index = CurrentIndex();
	if(selection.kind == LocalAiEngineKind::AI_PLAYER) {
		if(!aiPlayerEngines || selection.index >= aiPlayerEngines->size()) {
			deckProperties->setText(kAiPlayerHostVisibleFailureMessage.data());
			return false;
		}

		const wchar_t* overridedeck = nullptr;
		std::wstring tmpdeck{};
		const auto maxsize = static_cast<int>(bots.size() - (genericEngine != nullptr));
		if(index >= maxsize) {
			if(index >= 0) {
				tmpdeck = Utils::ToUnicodeIfNeeded(DeckManager::GetDeckPath(Utils::ToPathString(cbBotDeck->getItem(cbBotDeck->getSelected()))));
				overridedeck = tmpdeck.data();
			}
		} else if(index >= 0) {
			overridedeck = bots[index].deckfile.data();
		}

		// Serialize launch and participant notification so a very fast join cannot
		// arrive before its process record exists.
		std::lock_guard<std::mutex> process_lock(aiPlayerProcessesMutex);
		// Allocate tracking storage before process ownership can transfer to us.
		aiPlayerProcesses.reserve(aiPlayerProcesses.size() + 1);
		// 1 = scissors, 2 = rock, 3 = paper
		const auto result = (*aiPlayerEngines)[selection.index].Launch(
			port, pass, !chkMute->isChecked(), chkThrowRock->isChecked() * 2, overridedeck);
		if(!result) {
			deckProperties->setText(kAiPlayerHostVisibleFailureMessage.data());
			return false;
		}
		auto expected_name = (*aiPlayerEngines)[selection.index].launch_args.display_name;
		// Lobby names are transmitted in a fixed 20-code-unit field (19 + NUL).
		if(expected_name.size() > 19)
			expected_name.resize(19);
		aiPlayerProcesses.push_back({ selection.index, result.process_handle, std::move(expected_name), now_ms });
		return true;
	}

	int engine = CurrentEngine();
	if (index < 0 || engine < 0) return false;
	const wchar_t* overridedeck = nullptr;
	std::wstring tmpdeck{};
	const auto maxsize = (int)(bots.size() - (genericEngine != nullptr));
	if(engine != index || index >= maxsize) {
		if(index >= maxsize) {
			tmpdeck = Utils::ToUnicodeIfNeeded(DeckManager::GetDeckPath(Utils::ToPathString(cbBotDeck->getItem(cbBotDeck->getSelected()))));
			overridedeck = tmpdeck.data();
		} else {
			overridedeck = bots[index].deckfile.data();
		}
	}
	// 1 = scissors, 2 = rock, 3 = paper
	auto res = bots[engine].Launch(port, pass, !chkMute->isChecked(), chkThrowRock->isChecked() * 2, overridedeck);
#if EDOPRO_LINUX || EDOPRO_MACOS
	if(res > 0)
		windbotsPids.push_back(res);
#endif
	return res;
}


bool WindBotPanel::UpdatePendingAiPlayers(uint32_t now_ms) {
	std::lock_guard<std::mutex> process_lock(aiPlayerProcessesMutex);
	bool failed = false;
	for(auto& process : aiPlayerProcesses) {
		if(process.failure_reported)
			continue;
		if(process.participant_joined) {
			// Joined AI players are no longer pending, but retaining ownership lets
			// us reap normal exits and terminate them when the room closes.
			process.process_handle.IsRunning();
			continue;
		}
		if(process.observed_failure != AiPlayerPendingDecision::WAITING) {
			ErrorLog("AI-player launch failure: {}", AiPlayerPendingFailureReason(process.observed_failure));
			if(process.observed_failure == AiPlayerPendingDecision::TIMED_OUT)
				process.process_handle.Terminate();
			else
				process.process_handle.Release();
			process.failure_reported = true;
			failed = true;
			continue;
		}
		const auto decision = EvaluateAiPlayerPending(
			process.process_handle.IsRunning(), false, now_ms - process.launched_at_ms);
		if(decision != AiPlayerPendingDecision::WAITING) {
			// Confirm on the next frame so a lobby update already in flight can
			// correlate this launch before it is reported as failed.
			process.observed_failure = decision;
		}
	}
	if(failed)
		deckProperties->setText(kAiPlayerHostVisibleFailureMessage.data());
	return failed;
}

void WindBotPanel::NotifyParticipantJoined(epro::wstringview name) {
	std::lock_guard<std::mutex> process_lock(aiPlayerProcessesMutex);
	const auto index = FindPendingAiPlayerLaunch(aiPlayerProcesses, name);
	if(index == AI_PLAYER_PROCESS_NOT_FOUND)
		return;
	aiPlayerProcesses[index].participant_joined = true;
	aiPlayerProcesses[index].observed_failure = AiPlayerPendingDecision::WAITING;
}

void WindBotPanel::ClearAiPlayerProcesses() {
	std::lock_guard<std::mutex> process_lock(aiPlayerProcessesMutex);
	for(auto& process : aiPlayerProcesses)
		process.process_handle.Terminate();
	aiPlayerProcesses.clear();
}

std::wstring WindBotPanel::GetParameters(int port, epro::wstringview pass) {
	int index = CurrentIndex();
	int engine = CurrentEngine();
	if(index < 0 || engine < 0) return {};
	const wchar_t* overridedeck = nullptr;
	std::wstring tmpdeck{};
	const auto maxsize = (int)(bots.size() - (genericEngine != nullptr));
	if(engine != index || index >= maxsize) {
		if(index >= maxsize) {
			tmpdeck = Utils::ToUnicodeIfNeeded(DeckManager::GetDeckPath(Utils::ToPathString(cbBotDeck->getItem(cbBotDeck->getSelected()))));
			overridedeck = tmpdeck.data();
		} else {
			overridedeck = bots[index].deckfile.data();
		}
	}
	// 1 = scissors, 2 = rock, 3 = paper
	return bots[engine].GetLaunchParameters(port, pass, !chkMute->isChecked(), chkThrowRock->isChecked() * 2, overridedeck);
}

}
