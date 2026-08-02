#ifndef COREUTILS_FORMAT_STUB_H
#define COREUTILS_FORMAT_STUB_H

#include "text_types.h"

namespace epro {

template<typename T1, typename T2>
inline path_string format(path_stringview pattern, const T1& first, const T2& second) {
	const path_stringview concat_pattern = EPRO_TEXT("{}{}");
	const path_stringview slash_pattern = EPRO_TEXT("{}/{}");
	path_string a(first.data(), first.size());
	path_string b(second.data(), second.size());
	if(pattern == concat_pattern)
		return a + b;
	if(pattern == slash_pattern) {
		if(!a.empty() && a.back() != EPRO_TEXT('/'))
			a.push_back(EPRO_TEXT('/'));
		return a + b;
	}
	return path_string{};
}

}

#endif