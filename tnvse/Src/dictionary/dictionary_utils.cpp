#include "dictionary_internal.h"
#include "encoding.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

namespace fonthook
{
	namespace implementation::dictionary_utils {}
	using namespace implementation::dictionary_utils;

	namespace implementation::dictionary_utils
	{
		bool TryDecodeUtf8Exact(std::string_view value, std::wstring& decoded)
		{
			decoded.clear();
			if (value.empty()
				|| value.size() > static_cast<size_t>(std::numeric_limits<int>::max())
				|| value.find('\0') != std::string_view::npos)
			{
				return false;
			}

			const int required = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), nullptr, 0);
			if (required <= 0)
				return false;

			decoded.assign(static_cast<size_t>(required), L'\0');
			const int written = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
				static_cast<int>(value.size()), decoded.data(), required);
			if (written != required)
			{
				decoded.clear();
				return false;
			}
			return true;
		}

		std::string WideToMultiByte(const std::wstring& value, UINT codePage)
		{
			if (value.empty())
				return {};
			const int encodedByteCountWithTerminator = WideCharToMultiByte(
				codePage, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
			if (encodedByteCountWithTerminator <= 1)
				return {};
			std::string result(
				static_cast<size_t>(encodedByteCountWithTerminator - 1), '\0');
			WideCharToMultiByte(codePage, 0, value.c_str(), -1, &result[0],
				encodedByteCountWithTerminator, nullptr, nullptr);
			return result;
		}
	}

	// ---- OS / path / file utilities ----

	std::string GetGameDirectory()
	{
		char path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		char* slash = strrchr(path, '\\');
		if (slash)
			*slash = 0;
		return path;
	}

	bool IsAbsolutePath(const std::string& path)
	{
		if (path.size() >= 2 && path[1] == ':')
			return true;
		return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
	}

	std::string ResolvePath(std::string path)
	{
		std::replace(path.begin(), path.end(), '/', '\\');
		if (path.empty() || IsAbsolutePath(path))
			return path;
		return GetGameDirectory() + "\\" + path;
	}

	bool FileExists(const std::string& path)
	{
		const DWORD attr = GetFileAttributesA(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool DirectoryExists(const std::string& path)
	{
		const DWORD attr = GetFileAttributesA(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool ReadWholeFile(const std::string& path, std::string& out)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return false;
		out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
		return true;
	}

	std::vector<std::string> FindFiles(const std::string& directory, const char* pattern)
	{
		std::vector<std::string> files;
		const std::string findPath = directory + "\\" + pattern;
		WIN32_FIND_DATAA data = {};
		HANDLE find = FindFirstFileA(findPath.c_str(), &data);
		if (find == INVALID_HANDLE_VALUE)
			return files;

		do
		{
			if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
				files.emplace_back(data.cFileName);
		} while (FindNextFileA(find, &data));

		FindClose(find);
		return files;
	}

	// ---- encoding conversion ----

	std::string WideToUtf8(const std::wstring& value)
	{
		return WideToMultiByte(value, CP_UTF8);
	}

	std::wstring Utf8ToWide(const std::string& str)
	{
		if (str.empty())
			return {};
		int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
		if (size <= 1)
			return {};
		std::wstring result(size - 1, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
		return result;
	}

	bool TryEncodeWindows1252Exact(const std::wstring& value, std::string& encoded)
	{
		encoded.clear();
		if (value.empty()
			|| value.size() > static_cast<size_t>(std::numeric_limits<int>::max())
			|| value.find(L'\0') != std::wstring::npos)
		{
			return false;
		}
		if (std::none_of(value.begin(), value.end(), [](wchar_t ch)
			{
				return ch > 0x7F;
			}))
		{
			return false;
		}

		BOOL usedDefault = FALSE;
		const int required = WideCharToMultiByte(
			static_cast<UINT>(kWindows1252CodePage), WC_NO_BEST_FIT_CHARS,
			value.data(), static_cast<int>(value.size()), nullptr, 0,
			nullptr, &usedDefault);
		if (required <= 0 || usedDefault)
			return false;

		encoded.assign(static_cast<size_t>(required), '\0');
		usedDefault = FALSE;
		const int written = WideCharToMultiByte(
			static_cast<UINT>(kWindows1252CodePage), WC_NO_BEST_FIT_CHARS,
			value.data(), static_cast<int>(value.size()), encoded.data(), required,
			nullptr, &usedDefault);
		if (written != required || usedDefault)
		{
			encoded.clear();
			return false;
		}
		return true;
	}

	bool TryResolveWindows1252ExactSource(std::string_view value, std::string& encoded)
	{
		encoded.clear();
		if (value.size() >= 3
			&& static_cast<UInt8>(value[0]) == 0xEF
			&& static_cast<UInt8>(value[1]) == 0xBB
			&& static_cast<UInt8>(value[2]) == 0xBF)
		{
			value.remove_prefix(3);
		}
		if (value.empty()
			|| value.find('\0') != std::string_view::npos
			|| std::none_of(value.begin(), value.end(), [](char ch)
				{
					return static_cast<UInt8>(ch) >= 0x80;
				}))
		{
			return false;
		}

		std::wstring decoded;
		if (implementation::dictionary_utils::TryDecodeUtf8Exact(value, decoded))
			return TryEncodeWindows1252Exact(decoded, encoded);

		// A non-UTF-8 TXT source is treated as already carrying the original
		// Windows-1252 bytes. Reject the five undefined byte values so arbitrary
		// binary data cannot enter the exact index.
		for (char ch : value)
		{
			switch (static_cast<UInt8>(ch))
			{
			case 0x81:
			case 0x8D:
			case 0x8F:
			case 0x90:
			case 0x9D:
				return false;
			default:
				break;
			}
		}

		encoded.assign(value.data(), value.size());
		return true;
	}

	// ---- basic string utilities ----

	void ReplaceAll(std::string& text, std::string_view from, std::string_view to)
	{
		if (from.empty())
			return;

		size_t pos = 0;
		while ((pos = text.find(from.data(), pos, from.size())) != std::string::npos)
		{
			text.replace(pos, from.size(), to.data(), to.size());
			pos += to.size();
		}
	}

	size_t CountToken(std::string_view text, std::string_view token)
	{
		if (token.empty())
			return 0;

		size_t count = 0;
		size_t pos = 0;
		while ((pos = text.find(token, pos)) != std::string_view::npos)
		{
			++count;
			pos += token.size();
		}
		return count;
	}

	void StripUtf8Bom(std::string& text)
	{
		if (text.size() >= 3 &&
			(UInt8)text[0] == 0xEF &&
			(UInt8)text[1] == 0xBB &&
			(UInt8)text[2] == 0xBF)
		{
			text.erase(0, 3);
		}
	}

	void Trim(std::string& text)
	{
		const auto isTrim = [](unsigned char c) { return c == ' ' || c == '\t'; };
		size_t first = 0;
		while (first < text.size() && isTrim((unsigned char)text[first]))
			++first;
		size_t last = text.size();
		while (last > first && isTrim((unsigned char)text[last - 1]))
			--last;
		text = text.substr(first, last - first);
	}

	void CollapseSpaces(std::string& text)
	{
		std::string result;
		result.reserve(text.size());
		bool previousSpace = false;
		for (char ch : text)
		{
			const bool isSpace = ch == ' ' || ch == '\t';
			if (isSpace)
			{
				if (!previousSpace)
					result.push_back(' ');
				previousSpace = true;
			}
			else
			{
				result.push_back(ch);
				previousSpace = false;
			}
		}
		text.swap(result);
	}

	void ToLowerAscii(std::string& text)
	{
		for (char& ch : text)
		{
			if (ch >= 'A' && ch <= 'Z')
				ch = static_cast<char>(ch + ('a' - 'A'));
		}
	}

	bool HasAlphabet(std::string_view text)
	{
		for (char ch : text)
		{
			if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))
				return true;
		}
		return false;
	}

	bool ContainsDbcs(std::string_view text)
	{
		for (size_t i = 0; i + 1 < text.size(); ++i)
		{
			UInt32 code = 0;
			if (TryDecodeDoubleByte(text.data() + i, code))
				return true;
		}
		return false;
	}

	void RemoveControlChars(std::string& text)
	{
		text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch)
			{
				return ch != '\t' && ch != '\n' && ch != '\r' && std::iscntrl(ch);
			}), text.end());
	}

	void Correct1252ToAscii(std::string& text)
	{
		static constexpr char table[129] =
			"                 ''\"\"                                           AAA A ACEEEEIIII NOOO O  UUUU  saaa a aceeeeiiii nooo o  uuuu   ";
		for (char& ch : text)
		{
			const UInt8 c = (UInt8)ch;
			if (c >= 128)
				ch = table[c - 128];
		}
	}

	void Replace1252ForXml(std::wstring& text)
	{
		struct Pair { const wchar_t* from; const wchar_t* to; };
		static constexpr Pair pairs[] = {
			{ L"\u2018", L"'" }, { L"\u2019", L"'" },
			{ L"\u201C", L"\"" }, { L"\u201D", L"\"" },
			{ L"\u2026", L" " }, { L"\u2020", L" " },
			{ L"\u2021", L" " }, { L"\u2030", L" " },
			{ L"\u20AC", L" " }, { L"\u2122", L" " }
		};

		for (const auto& pair : pairs)
		{
			size_t pos = 0;
			const std::wstring from(pair.from);
			const std::wstring to(pair.to);
			while ((pos = text.find(from, pos)) != std::wstring::npos)
			{
				text.replace(pos, from.size(), to);
				pos += to.size();
			}
		}

		static constexpr wchar_t table[] =
			L"                                AAA A ACEEEEIIII NOOO O  UUUU  saaa a aceeeeiiii nooo o  uuuu   ";
		for (wchar_t& ch : text)
		{
			if (ch >= 0xA0 && ch <= 0xFF)
				ch = table[ch - 0xA0];
		}
	}

	void RemoveAlignmentTag(std::string& text)
	{
		if (text.empty() || text[0] != '<')
			return;

		const size_t end = text.find('>');
		if (end == std::string::npos)
			return;

		const std::string tag = text.substr(1, end - 1);
		if (tag == "center" || tag == "right")
			text.erase(0, end + 1);
	}

	std::vector<std::string> SplitByToken(std::string_view text, std::string_view token)
	{
		std::vector<std::string> result;
		size_t cursor = 0;
		while (cursor <= text.size())
		{
			const size_t pos = text.find(token, cursor);
			if (pos == std::string_view::npos)
			{
				result.emplace_back(std::string(text.substr(cursor)));
				break;
			}
			result.emplace_back(std::string(text.substr(cursor, pos - cursor)));
			cursor = pos + token.size();
		}
		return result;
	}

	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		std::stringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			lines.push_back(line);
		}
		if (!text.empty() && text.back() == '\n')
			return lines;
		if (lines.empty())
			lines.emplace_back();
		return lines;
	}

	std::pair<std::string, std::string> SplitIdLine(const std::string& line)
	{
		const size_t pos = line.find('\t');
		if (pos == std::string::npos)
			return {};
		return { line.substr(0, pos), line.substr(pos + 1) };
	}

} // namespace fonthook
