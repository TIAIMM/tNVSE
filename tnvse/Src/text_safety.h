#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace fonthook::text_safety
{
	// Retail Font::PrepText stores replacement names in a 128-byte local and
	// stops collecting at 0x7f bytes before calling Interface replacement code.
	inline constexpr size_t kRetailReplacementNameMaxBytes = 0x7f;
	inline constexpr size_t kRetailReplacementOutputCapacity = 0x400;
	inline constexpr size_t kRetailUiTextCapacity = 0x104;

	enum class CopyStatus
	{
		Copied,
		InvalidArgument,
		InsufficientCapacity,
	};

	enum class CopyChoice
	{
		Preferred,
		Fallback,
		None,
	};

	struct GameVariableToken
	{
		size_t nameBegin = 0;
		size_t nameLength = 0;
		size_t nextIndex = 0;
		bool isPositive = true;
	};

	inline void ClearBuffer(char* destination, size_t capacity) noexcept
	{
		if (destination && capacity != 0)
			destination[0] = '\0';
	}

	inline CopyStatus CopyTextIfFits(
		char* destination,
		size_t capacity,
		std::string_view source) noexcept
	{
		if (!destination || capacity == 0)
			return CopyStatus::InvalidArgument;

		if (source.size() >= capacity
			|| source.find('\0') != std::string_view::npos)
		{
			destination[0] = '\0';
			return CopyStatus::InsufficientCapacity;
		}

		if (!source.empty())
			std::memmove(destination, source.data(), source.size());
		destination[source.size()] = '\0';
		return CopyStatus::Copied;
	}

	inline CopyStatus CopyCStringIfFits(
		char* destination,
		size_t capacity,
		const char* source) noexcept
	{
		if (!destination || capacity == 0 || !source)
		{
			ClearBuffer(destination, capacity);
			return CopyStatus::InvalidArgument;
		}

		size_t length = 0;
		while (length < capacity && source[length] != '\0')
			++length;
		if (length == capacity)
		{
			destination[0] = '\0';
			return CopyStatus::InsufficientCapacity;
		}

		return CopyTextIfFits(destination, capacity,
			std::string_view(source, length));
	}

	inline CopyChoice CopyPreferredTextWithFallback(
		char* destination,
		size_t capacity,
		std::string_view preferred,
		std::string_view fallback) noexcept
	{
		if (CopyTextIfFits(destination, capacity, preferred)
			== CopyStatus::Copied)
		{
			return CopyChoice::Preferred;
		}
		if (CopyTextIfFits(destination, capacity, fallback)
			== CopyStatus::Copied)
		{
			return CopyChoice::Fallback;
		}
		return CopyChoice::None;
	}

	// This accepts both the name-only form used by Font::PrepText and the
	// optional &[-]name; form used by FontManager rich-text collection. The
	// bounded scan is intentional: unsafe arguments are rejected before the
	// retail function can copy them into its 260-byte TestConstant local.
	inline bool IsRetailReplacementArgumentSafe(const char* argument) noexcept
	{
		if (!argument || !*argument)
			return false;

		size_t index = 0;
		if (argument[index] == '&')
			++index;
		if (argument[index] == '-')
			++index;

		size_t nameLength = 0;
		for (;;)
		{
			const char current = argument[index + nameLength];
			if (current == '\0')
				return nameLength != 0;
			if (current == ';')
			{
				return nameLength != 0
					&& argument[index + nameLength + 1] == '\0';
			}
			if (current == '\n' || current == '\r'
				|| nameLength == kRetailReplacementNameMaxBytes)
			{
				return false;
			}
			++nameLength;
		}
	}

	// Parse only the form that PreResolveGameVariables historically resolved:
	// the name must terminate at ';' or a line boundary inside the string.
	// Malformed and overlong sequences remain literal text.
	inline bool TryParseGameVariableToken(
		std::string_view text,
		size_t ampersandIndex,
		GameVariableToken& token) noexcept
	{
		token = {};
		if (ampersandIndex >= text.size()
			|| text[ampersandIndex] != '&')
		{
			return false;
		}

		size_t nameBegin = ampersandIndex + 1;
		bool isPositive = true;
		if (nameBegin < text.size() && text[nameBegin] == '-')
		{
			isPositive = false;
			++nameBegin;
		}

		size_t nameLength = 0;
		for (size_t index = nameBegin; index < text.size(); ++index)
		{
			const char current = text[index];
			if (current == '\0')
				return false;
			if (current == ';' || current == '\n' || current == '\r')
			{
				if (nameLength == 0)
					return false;
				token.nameBegin = nameBegin;
				token.nameLength = nameLength;
				token.nextIndex = index + (current == ';' ? 1u : 0u);
				token.isPositive = isPositive;
				return true;
			}
			if (nameLength == kRetailReplacementNameMaxBytes)
				return false;
			++nameLength;
		}

		return false;
	}
}
