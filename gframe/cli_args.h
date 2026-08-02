#ifndef CLI_ARGS_H
#define CLI_ARGS_H

#include <array>
#include <vector>
#include "text_types.h"

enum LAUNCH_PARAM {
	WORK_DIR,
	MUTE,
	CHANGELOG,
	DISCORD,
	HOST_HEADLESS,
	OVERRIDE_UPDATE_URL,
	WANTS_TO_RUN_AS_ADMIN,
	REPOS_READ_ONLY,
	ONLY_CLONE_REPOS,
	USER_STORAGE_DIRECTORY,
	COUNT,
};


struct Option {
	bool enabled{ false };
	epro::path_stringview argument;
	std::vector<epro::path_string> arguments;
};

using args_t = std::array<Option, LAUNCH_PARAM::COUNT>;

extern args_t cli_args;

#endif //CLI_ARGS_H
