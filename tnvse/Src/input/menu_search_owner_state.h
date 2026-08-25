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

	// Tile traits are presentation mirrors. They are useful for adopting an
	// already-open search field, but a transient inactive mirror must never tear
	// down an input session whose ownership was confirmed at Stewie's chained
	// keyboard-handler boundary. Authoritative observations are reserved for
	// that boundary and for explicit menu/Tile lifecycle transitions.
	enum class ObservationAuthority : std::uint8_t
	{
		Presentation,
		Authoritative,
	};

	enum class ControlAction : std::uint8_t
	{
		Toggle,
		Reset,
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

	[[nodiscard]] constexpr State MergeObservation(
		State currentState,
		State observedState,
		ObservationAuthority authority)
	{
		// An unreadable/missing presentation value carries no transition. Keeping
		// the last confirmed state also makes SEH-protected Tile replacement races
		// fail without spuriously closing the IME session.
		if (observedState == State::Unknown)
			return currentState;

		if (authority == ObservationAuthority::Presentation
			&& currentState == State::Active
			&& observedState == State::Inactive)
		{
			return State::Active;
		}

		return observedState;
	}

	[[nodiscard]] constexpr State ResolveReplacementObservation(
		State observedState)
	{
		// A different Tile/root is a real ownership boundary. Only a replacement
		// that positively reports active may inherit the old input session.
		return observedState == State::Active
			? State::Active : State::Inactive;
	}

	[[nodiscard]] constexpr State ResolveHandledControlObservation(
		State currentState,
		State observedState,
		ControlAction action)
	{
		// Ctrl+R is an idempotent reset in every supported Stewie handler.
		if (action == ControlAction::Reset)
			return State::Inactive;

		// Ctrl+F may be rejected by an owner-specific gate (for example the
		// LevelUp perk page), so use the post-handler absolute observation rather
		// than blindly negating tNVSE's cached state.
		return MergeObservation(currentState, observedState,
			ObservationAuthority::Authoritative);
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
		if (observedState == State::Unknown)
			return {};

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
