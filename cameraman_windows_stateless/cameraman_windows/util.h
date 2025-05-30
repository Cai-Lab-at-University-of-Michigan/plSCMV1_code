#pragma once

#include <string>
inline std::wstring convert_narrow_to_wide_string(std::string as)
{
	std::wstring out;
	out.assign(as.begin(), as.end());
	return out;
}