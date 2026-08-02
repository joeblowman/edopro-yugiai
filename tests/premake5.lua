project "coreutils_discovery_test"
	kind "ConsoleApp"
	language "C++"
	cppdialect "C++17"
	targetdir "../bin/tests/%{cfg.buildcfg}"
	objdir "../obj/tests/%{cfg.buildcfg}"
	files {
		"coreutils_discovery_test.cpp",
		"../gframe/core_utils.cpp"
	}
	includedirs {
		"../gframe",
		"../ocgcore"
	}

	filter { "action:not vs*" }
		enablewarnings "pedantic"
		buildoptions { "-include ../tests/coreutils_format_stub.h" }
	filter {}