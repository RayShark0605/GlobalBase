#include "GB_Math.h"

std::string GB_RandomString(size_t length, const std::vector<char32_t>& characterPool)
{
	if (length == 0 || characterPool.empty())
	{
		return std::string();
	}

	auto AppendUtf8FromCodePoint = [](std::string& output, char32_t codePoint) {
		// 过滤无效码点：代理项范围 / 超出 Unicode 上限
		if ((codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF)
		{
			codePoint = U'?';
		}

		if (codePoint <= 0x7F)
		{
			output.push_back(static_cast<char>(codePoint));
		}
		else if (codePoint <= 0x7FF)
		{
			output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else if (codePoint <= 0xFFFF)
		{
			output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
		else
		{
			output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
	};

	static thread_local std::mt19937 generator(std::random_device{}());
	std::uniform_int_distribution<size_t> distribution(0, characterPool.size() - 1);

	std::string result;
	result.reserve(length);

	for (size_t i = 0; i < length; i++)
	{
		const char32_t character = characterPool[distribution(generator)];
		AppendUtf8FromCodePoint(result, character);
	}

	return result;
}

std::string GB_RandomString(size_t length)
{
	static const std::vector<char32_t> defaultCharacterPool = []() {
		const std::u32string poolString = U"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
		return std::vector<char32_t>(poolString.begin(), poolString.end());
	}();

	return GB_RandomString(length, defaultCharacterPool);
}


