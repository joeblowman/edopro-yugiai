#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "core_utils.h"
#include "ocgapi.h"
#include "utils.h"

namespace fs = std::filesystem;

extern "C" void* OCG_DuelGetMessage(OCG_Duel, uint32_t* length) {
	if(length != nullptr)
		*length = 0;
	return nullptr;
}

namespace ygo {

namespace {

epro::path_string JoinDir(epro::path_stringview base, epro::path_stringview child) {
	epro::path_string out(base.data(), base.size());
	out.append(child.data(), child.size());
	out.push_back(EPRO_TEXT('/'));
	return out;
}

}

void Utils::FindFiles(epro::path_stringview path, const std::function<void(epro::path_stringview, bool)>& cb) {
	const auto normalized = NormalizePath(path);
	const fs::path root(normalized);
	if(!fs::exists(root) || !fs::is_directory(root))
		return;
	for(const auto& entry : fs::directory_iterator(root)) {
		auto name = entry.path().filename().string();
		cb(name, entry.is_directory());
	}
}

std::vector<epro::path_string> Utils::FindFiles(epro::path_stringview path, const std::vector<epro::path_stringview>& extensions, int subdirectorylayers) {
	std::vector<epro::path_string> results;
	FindFiles(path, [&results, extensions, path, subdirectorylayers](epro::path_stringview name, bool isdir) {
		if(isdir) {
			if(subdirectorylayers > 0 && name != EPRO_TEXT(".") && name != EPRO_TEXT("..")) {
				auto nested = FindFiles(JoinDir(path, name), extensions, subdirectorylayers - 1);
				for(auto& file : nested)
					file = JoinDir(EPRO_TEXT(""), name) + file;
				results.insert(results.end(), std::make_move_iterator(nested.begin()), std::make_move_iterator(nested.end()));
			}
			return;
		}
		if(extensions.empty() || std::find(extensions.begin(), extensions.end(), Utils::GetFileExtension(name)) != extensions.end())
			results.emplace_back(name.data(), name.size());
	});
	std::sort(results.begin(), results.end(), CompareIgnoreCase<epro::path_string>);
	return results;
}

std::vector<epro::path_string> Utils::FindSubfolders(epro::path_stringview path, int subdirectorylayers, bool addparentpath) {
	std::vector<epro::path_string> results;
	FindFiles(path, [&results, path, subdirectorylayers, addparentpath](epro::path_stringview name, bool isdir) {
		if(!isdir || name == EPRO_TEXT(".") || name == EPRO_TEXT(".."))
			return;
		auto fullpath = JoinDir(path, name);
		epro::path_stringview current = addparentpath ? epro::path_stringview(fullpath) : name;
		results.emplace_back(current.data(), current.size());
		if(subdirectorylayers > 1) {
			auto nested = FindSubfolders(fullpath, subdirectorylayers - 1, false);
			for(auto& folder : nested)
				folder = JoinDir(fullpath, folder);
			results.insert(results.end(), std::make_move_iterator(nested.begin()), std::make_move_iterator(nested.end()));
		}
	});
	std::sort(results.begin(), results.end(), CompareIgnoreCase<epro::path_string>);
	return results;
}

}

namespace {

struct ScopedTempDir {
	fs::path path;

	ScopedTempDir() {
		char temp_template[] = "/tmp/coreutils-discovery-XXXXXX";
		char* dir = mkdtemp(temp_template);
		assert(dir != nullptr);
		path = dir;
	}

	~ScopedTempDir() {
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void MakeDir(const fs::path& dir) {
	fs::create_directories(dir);
}

void MakeFile(const fs::path& file) {
	MakeDir(file.parent_path());
	std::ofstream out(file);
	assert(out.good());
	out << "ok\n";
}

std::vector<std::string> ToUtf8(const std::vector<epro::path_string>& values) {
	std::vector<std::string> result;
	result.reserve(values.size());
	for(const auto& value : values)
		result.emplace_back(value.begin(), value.end());
	return result;
}

void AssertEqual(const char* label, const std::vector<std::string>& actual, std::initializer_list<std::string> expected) {
	auto normalize_all = [](const std::vector<std::string>& input) {
		std::vector<std::string> out;
		out.reserve(input.size());
		for(const auto& value : input) {
			auto normalized = ygo::Utils::NormalizePath(value, false);
			out.emplace_back(normalized.begin(), normalized.end());
		}
		return out;
	};
	const std::vector<std::string> expected_vec(expected);
	const auto actual_norm = normalize_all(actual);
	const auto expected_norm = normalize_all(expected_vec);
	if(actual_norm != expected_norm) {
		std::fprintf(stderr, "assertion failed: %s\n", label);
		std::fprintf(stderr, "  actual (%zu):\n", actual.size());
		for(const auto& value : actual_norm)
			std::fprintf(stderr, "    %s\n", value.c_str());
		std::fprintf(stderr, "  expected (%zu):\n", expected_vec.size());
		for(const auto& value : expected_norm)
			std::fprintf(stderr, "    %s\n", value.c_str());
	}
	assert(actual_norm == expected_norm);
}

epro::path_string AsPathString(const fs::path& p) {
	return p.string();
}

void TestDatabaseRecursionDepthAndOrdering() {
	ScopedTempDir tmp;
	const auto root = tmp.path / "db-root";
	MakeFile(root / "Zeta.CDB");
	MakeFile(root / "alpha.cdb");
	MakeFile(root / "nested" / "Beta.cdb");
	MakeFile(root / "nested" / "deep" / "gamma.cdb");
	MakeFile(root / "nested" / "ignore.txt");

	CoreUtils::ScanPolicy depth0{};
	depth0.database_depth = 0;
	auto depth0_files = ToUtf8(CoreUtils::CollectDatabaseFiles({ AsPathString(root) }, depth0));
	AssertEqual("db depth=0", depth0_files, {
		(root / "alpha.cdb").generic_string(),
		(root / "Zeta.CDB").generic_string()
	});

	CoreUtils::ScanPolicy depth1{};
	depth1.database_depth = 1;
	auto depth1_files = ToUtf8(CoreUtils::CollectDatabaseFiles({ AsPathString(root) }, depth1));
	AssertEqual("db depth=1", depth1_files, {
		(root / "alpha.cdb").generic_string(),
		(root / "nested" / "Beta.cdb").generic_string(),
		(root / "Zeta.CDB").generic_string()
	});

	CoreUtils::ScanPolicy depth2{};
	depth2.database_depth = 2;
	auto depth2_files = ToUtf8(CoreUtils::CollectDatabaseFiles({ AsPathString(root) }, depth2));
	AssertEqual("db depth=2", depth2_files, {
		(root / "alpha.cdb").generic_string(),
		(root / "nested" / "Beta.cdb").generic_string(),
		(root / "nested" / "deep" / "gamma.cdb").generic_string(),
		(root / "Zeta.CDB").generic_string()
	});
}

void TestScriptDirectoryDepthOrderingAndDedup() {
	ScopedTempDir tmp;
	const auto base = tmp.path / "scripts";
	MakeDir(base / "beta");
	MakeDir(base / "alpha");
	MakeDir(base / "alpha" / "inner");

	CoreUtils::ScanPolicy depth0{};
	depth0.script_depth = 0;
	auto depth0_dirs = ToUtf8(CoreUtils::CollectScriptDirectories({ AsPathString(base) }, depth0));
	AssertEqual("script depth=0", depth0_dirs, {
		(base).generic_string() + "/"
	});

	CoreUtils::ScanPolicy depth2{};
	depth2.script_depth = 2;
	auto depth2_dirs = ToUtf8(CoreUtils::CollectScriptDirectories({ AsPathString(base) }, depth2));
	AssertEqual("script depth=2", depth2_dirs, {
		(base).generic_string() + "/",
		(base / "alpha").generic_string() + "/",
		(base / "alpha" / "inner").generic_string() + "/",
		(base / "beta").generic_string() + "/"
	});

	const auto normalized = (base / ".").generic_string();
	auto dedup_dirs = ToUtf8(CoreUtils::CollectScriptDirectories({ AsPathString(base), normalized }, depth2));
	assert(dedup_dirs == depth2_dirs);
}

void TestDatabaseDedupWithOverlappingRoots() {
	ScopedTempDir tmp;
	const auto root = tmp.path / "expansions";
	MakeFile(root / "cards.cdb");
	MakeFile(root / "sub" / "extra.cdb");

	CoreUtils::ScanPolicy policy{};
	policy.database_depth = 1;
	auto files = ToUtf8(CoreUtils::CollectDatabaseFiles({ AsPathString(root), (root / ".").generic_string() }, policy));
	AssertEqual("db dedup", files, {
		(root / "cards.cdb").generic_string(),
		(root / "sub" / "extra.cdb").generic_string()
	});
}

}

int main() {
	TestDatabaseRecursionDepthAndOrdering();
	TestScriptDirectoryDepthOrderingAndDedup();
	TestDatabaseDedupWithOverlappingRoots();
	return 0;
}