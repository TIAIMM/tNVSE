#include "menu_search_owner_state.h"

#include <iostream>
#include <string_view>

namespace
{
	using namespace fonthook::multibyte_input::menu_search_owner_state;

	int s_failures = 0;

	void Expect(bool condition, std::string_view message)
	{
		if (condition)
			return;
		std::cerr << "FAIL " << message << '\n';
		++s_failures;
	}

	Observation BaseInputField()
	{
		Observation observation;
		observation.menuVisible = true;
		observation.tileAvailable = true;
		observation.ownerSurfaceEnabled = true;
		observation.usesInputField = true;
		return observation;
	}

	Observation BasePlainSearchBar()
	{
		Observation observation;
		observation.menuVisible = true;
		observation.tileAvailable = true;
		observation.ownerSurfaceEnabled = true;
		observation.visibleTraitPresent = true;
		observation.visibleTraitValue = 1.0f;
		observation.stringTraitPresent = true;
		return observation;
	}
}

int main()
{
	Observation inputField = BaseInputField();
	Expect(Resolve(inputField) == State::Inactive,
		"a newly loaded InputField without _IsActive must be inactive");
	inputField.activeTraitPresent = true;
	inputField.activeTraitValue = 0.0f;
	Expect(Resolve(inputField) == State::Inactive,
		"_IsActive=0 must be inactive");
	inputField.activeTraitValue = 1.0f;
	Expect(Resolve(inputField) == State::Active,
		"_IsActive=1 must be active");

	Observation initialPlain = BasePlainSearchBar();
	Expect(Resolve(initialPlain) == State::Inactive,
		"initial visible plain SearchBar without alpha must be inactive");

	Observation activePlain = BasePlainSearchBar();
	activePlain.alphaTraitPresent = true;
	activePlain.alphaTraitValue = 255.0f;
	activePlain.stringNonEmpty = true;
	Expect(Resolve(activePlain) == State::Active,
		"visible alpha=255 underscore SearchBar must be active");
	Expect(Resolve(activePlain) == State::Active,
		"duplicate absolute observations must be idempotent");

	Observation queriedPlain = activePlain;
	queriedPlain.stringNonEmpty = true;
	Expect(Resolve(queriedPlain) == State::Active,
		"a visible high-alpha non-empty query must remain active");

	Observation dimmedPlain = activePlain;
	dimmedPlain.alphaTraitValue = 128.0f;
	Expect(Resolve(dimmedPlain) == State::Inactive,
		"Stewie's alpha=128 close path must be inactive");
	Observation hiddenPlain = activePlain;
	hiddenPlain.visibleTraitValue = 0.0f;
	Expect(Resolve(hiddenPlain) == State::Inactive,
		"Stewie's hidden close path must be inactive");

	Observation missingTile = BasePlainSearchBar();
	missingTile.tileAvailable = false;
	Expect(Resolve(missingTile) == State::Unknown,
		"a missing Tile must fail closed as unknown");

	Observation disabledStartMenu = activePlain;
	disabledStartMenu.ownerSurfaceEnabled = false;
	Expect(Resolve(disabledStartMenu) == State::Inactive,
		"a disabled StartMenu saves surface must be inactive");

	State cached = State::Inactive;
	cached = Resolve(activePlain);
	Expect(cached == State::Active,
		"an active owner must override a stale inactive cache");
	cached = State::Active;
	cached = Resolve(initialPlain);
	Expect(cached == State::Inactive,
		"an inactive owner must override a stale active cache");

	const ReconcileDecision opened = DecideReconcile(
		State::Inactive, State::Active, true);
	Expect(opened.clearCachedTarget && opened.activateSession
			&& !opened.endSession,
		"an observed activation must start the local input session");
	const ReconcileDecision duplicate = DecideReconcile(
		State::Active, State::Active, true);
	Expect(!duplicate.clearCachedTarget && !duplicate.activateSession
			&& !duplicate.endSession,
		"duplicate owner observations must not toggle or refresh state");
	const ReconcileDecision lateInstall = DecideReconcile(
		State::Active, State::Active, true, true);
	Expect(lateInstall.clearCachedTarget && lateInstall.activateSession
			&& !lateInstall.endSession,
		"late adapter installation must adopt an already-active owner");
	const ReconcileDecision replacement = DecideReconcile(
		State::Active, State::Active, true, true);
	Expect(replacement.activateSession,
		"an active Tile replacement must refresh the target identity");
	Expect(IsTrackedTileReplacement(true, false, false),
		"replacement history must survive clearing stale non-owning pointers");
	Expect(!IsTrackedTileReplacement(false, false, false),
		"the first Tile attachment must not be treated as a replacement");
	const ReconcileDecision closed = DecideReconcile(
		State::Active, State::Inactive, true);
	Expect(closed.clearCachedTarget && !closed.activateSession
			&& closed.endSession,
		"an absolute inactive observation must close the local session");

	Observation rejectedLevelUp = initialPlain;
	Expect(Resolve(rejectedLevelUp) == State::Inactive,
		"a LevelUp Ctrl+F rejection must not be guessed active");

	if (s_failures)
	{
		std::cerr << s_failures
			<< " menu search owner-state test(s) failed\n";
		return 1;
	}

	std::cout << "menu search owner-state tests passed\n";
	return 0;
}
