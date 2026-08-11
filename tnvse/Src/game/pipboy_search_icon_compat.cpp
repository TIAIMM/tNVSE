#include "pipboy_search_icon_compat.h"

#include "TileText.hpp"

#include <array>
#include <cstring>

namespace fonthook::implementation::pipboy_search_icon_compat
{
	namespace
	{
		struct SearchIconContract
		{
			const char* searchBarName;
			const char* searchButtonName;
		};

		inline constexpr std::array<SearchIconContract, 3> kSearchIconContracts = { {
			{ "IM_SearchBar", "IM_SearchButton" },
			{ "SM_SearchBar", "SM_SearchButton" },
			{ "MM_SearchBar", "MM_SearchButton" },
		} };

		thread_local UInt32 s_normalizationDepth = 0;

		const SearchIconContract* FindContract(const TileText* tile)
		{
			if (!tile)
				return nullptr;

			const char* const tileName = tile->strName.c_str();
			if (!tileName)
				return nullptr;

			for (const SearchIconContract& contract : kSearchIconContracts)
			{
				if (_stricmp(tileName, contract.searchBarName) == 0)
					return &contract;
			}

			return nullptr;
		}

		Tile* FindDirectChild(Tile* parent, const char* name)
		{
			if (!parent || !name)
				return nullptr;

			for (Tile* child : parent->kChildren)
			{
				const char* const childName = child ? child->strName.c_str() : nullptr;
				if (childName && _stricmp(childName, name) == 0)
					return child;
			}

			return nullptr;
		}

		bool StringValueEquals(Tile* tile, UInt32 trait, const char* expected)
		{
			const Tile::Value* const value = tile ? tile->GetValue(trait) : nullptr;
			return value && value->pcText && expected
				&& _stricmp(value->pcText, expected) == 0;
		}

		bool MatchesSearchIconActions(
			TileText* searchBar,
			Tile* searchButton,
			Tile::Value* widthValue)
		{
			if (!searchBar || !searchButton || !widthValue
				|| widthValue->pParent != searchBar || widthValue->pAction)
			{
				return false;
			}

			static const UInt32 statusTrait = Tile::TraitNameToID("_Status");
			static const UInt32 inactiveTextureTrait =
				Tile::TraitNameToID("_Tile_0");
			static const UInt32 activeTextureTrait =
				Tile::TraitNameToID("_Tile_1");
			if (!statusTrait || !inactiveTextureTrait || !activeTextureTrait
				|| !StringValueEquals(searchButton, inactiveTextureTrait,
					"lStewieAl\\search.dds")
				|| !StringValueEquals(searchButton, activeTextureTrait,
					"lStewieAl\\searchStop.dds"))
			{
				return false;
			}

			const Tile::Value* const statusValue =
				searchButton->GetValue(statusTrait);
			const Tile::Action* const copyWidth =
				statusValue ? statusValue->pAction : nullptr;
			if (!copyWidth || copyWidth->uiType != Tile::kAction_copy)
				return false;

			const auto* const widthReference =
				static_cast<const Tile::RefValueAction*>(copyWidth);
			const Tile::Action* const greaterThan = copyWidth->pNext;
			if (widthReference->pTileValue != widthValue
				|| !greaterThan || greaterThan->uiType != Tile::kAction_gt)
			{
				return false;
			}

			const auto* const threshold =
				static_cast<const Tile::FloatAction*>(greaterThan);
			const Tile::Action* const tail = greaterThan->pNext;
			return threshold->fValue == 10.0f
				&& (!tail || (tail->uiType == Tile::kAction_end && !tail->pNext));
		}
	}

	void NormalizeInactiveEmptySearchWidth(TileText* searchBar)
	{
		// Pip-Boy UI Tweaks selects its magnifier/stop texture from
		// SearchBar.width > 10. FreeType intentionally prepares empty text as one
		// measurable space, so correct only the published Tile width after MakeNode
		// has completed; the placeholder text and its geometry remain untouched.
		if (!searchBar || s_normalizationDepth)
			return;

		const SearchIconContract* const contract = FindContract(searchBar);
		if (!contract)
			return;

		const Tile::Value* const stringValue =
			searchBar->GetValue(Tile::kTileValue_string);
		if (!stringValue || (stringValue->pcText && stringValue->pcText[0]))
			return;

		static const UInt32 activeTrait = Tile::TraitNameToID("_IsActive");
		const Tile::Value* const activeValue = activeTrait
			? searchBar->GetValue(activeTrait) : nullptr;
		if (activeValue && activeValue->fNum > 0.5f)
			return;

		Tile::Value* const widthValue =
			searchBar->GetValue(Tile::kTileValue_width);
		if (!widthValue || widthValue->fNum <= 10.0f)
			return;

		Tile* const searchButton = FindDirectChild(
			searchBar->pParent, contract->searchButtonName);
		if (!MatchesSearchIconActions(searchBar, searchButton, widthValue))
			return;

		++s_normalizationDepth;
		searchBar->SetValue(Tile::kTileValue_width, 0.0f, true);
		--s_normalizationDepth;
	}
}
