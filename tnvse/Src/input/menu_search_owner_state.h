#pragma once

#include <cstdint>

namespace fonthook::multibyte_input::menu_search_owner_state
{
	enum class State : std::uint8_t
	{
		Unknown,
		Inactive,
		Active,
	};

	struct Observation
	{
		bool menuVisible = false;
		bool tileAvailable = false;
		bool ownerSurfaceEnabled = true;
		bool usesInputField = false;

		bool activeTraitPresent = false;
		float activeTraitValue = 0.0f;
		bool visibleTraitPresent = false;
		float visibleTraitValue = 0.0f;
		bool alphaTraitPresent = false;
		float alphaTraitValue = 0.0f;
		bool stringTraitPresent = false;
		bool stringNonEmpty = false;
	};

	[[nodiscard]] constexpr State Resolve(const Observation& observation)
	{
		if (!observation.menuVisible || !observation.ownerSurfaceEnabled)
			return State::Inactive;
		if (!observation.tileAvailable)
			return State::Unknown;

		if (observation.usesInputField)
		{
			// Stewie's InputField::SetActive writes _IsActive synchronously.
			// A newly loaded prefab has no such trait and is therefore inactive.
			return observation.activeTraitPresent
				&& observation.activeTraitValue > 0.5f
				? State::Active : State::Inactive;
		}

		// Plain SearchBar prefabs start visible with an empty string and no alpha
		// value. SetVisible(true) publishes alpha=255 plus '_' (or the query),
		// while SetVisible(false) either hides the tile or publishes alpha=128.
		if (!observation.visibleTraitPresent)
			return State::Unknown;
		if (observation.visibleTraitValue <= 0.5f)
			return State::Inactive;
		if (!observation.alphaTraitPresent)
			return State::Inactive;
		if (observation.alphaTraitValue <= 192.0f)
			return State::Inactive;
		if (!observation.stringTraitPresent)
			return State::Unknown;
		return observation.stringNonEmpty ? State::Active : State::Inactive;
	}

	[[nodiscard]] constexpr bool IsActive(State state)
	{
		return state == State::Active;
	}

	[[nodiscard]] constexpr bool IsTrackedTileReplacement(
		bool previouslyTracked,
		bool sameRoot,
		bool sameTile)
	{
		return previouslyTracked && (!sameRoot || !sameTile);
	}

	struct ReconcileDecision
	{
		bool clearCachedTarget = false;
		bool activateSession = false;
		bool endSession = false;
	};

	[[nodiscard]] constexpr ReconcileDecision DecideReconcile(
		State previousState,
		State observedState,
		bool adapterInstalled,
		bool forceSessionRefresh = false)
	{
		const bool wasActive = IsActive(previousState);
		const bool isActive = IsActive(observedState);
		return {
			wasActive != isActive || (forceSessionRefresh && isActive),
			isActive && adapterInstalled
				&& (!wasActive || forceSessionRefresh),
			wasActive && !isActive,
		};
	}

	[[nodiscard]] constexpr const char* Name(State state)
	{
		switch (state)
		{
		case State::Inactive:
			return "inactive";
		case State::Active:
			return "active";
		default:
			return "unknown";
		}
	}
}
