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

	Expect(MergeObservation(State::Active, State::Inactive,
			ObservationAuthority::Presentation) == State::Active,
		"an inactive presentation mirror must not close a confirmed session");
	Observation inactiveInputField = inputField;
	inactiveInputField.activeTraitValue = 0.0f;
	Expect(MergeObservation(State::Active, Resolve(inactiveInputField),
			ObservationAuthority::Presentation) == State::Active,
		"a transient InputField _IsActive mirror must not detach IME ownership");
	Expect(MergeObservation(State::Active, Resolve(dimmedPlain),
			ObservationAuthority::Presentation) == State::Active,
		"a transient plain SearchBar alpha mirror must not detach IME ownership");
	Expect(MergeObservation(State::Active, State::Inactive,
			ObservationAuthority::Authoritative) == State::Inactive,
		"an authoritative inactive transition must close a confirmed session");
	Expect(MergeObservation(State::Inactive, State::Active,
			ObservationAuthority::Presentation) == State::Active,
		"a positive presentation mirror may adopt an already-open search field");
	Expect(MergeObservation(State::Active, State::Unknown,
			ObservationAuthority::Authoritative) == State::Active,
		"an unknown authoritative read must preserve a confirmed active state");
	Expect(MergeObservation(State::Inactive, State::Unknown,
			ObservationAuthority::Presentation) == State::Inactive,
		"an unknown presentation read must preserve a confirmed inactive state");
	Expect(ResolveReplacementObservation(State::Active) == State::Active,
		"a positively active replacement may inherit ownership");
	Expect(ResolveReplacementObservation(State::Inactive) == State::Inactive,
		"an inactive replacement must end ownership");
	Expect(ResolveReplacementObservation(State::Unknown) == State::Inactive,
		"an unreadable replacement must fail closed at the lifecycle boundary");
	Expect(ResolveHandledControlObservation(State::Inactive, State::Active,
			ControlAction::Toggle) == State::Active,
		"post-handler Ctrl+F active must repair a stale inactive cache");
	Expect(ResolveHandledControlObservation(State::Active, State::Inactive,
			ControlAction::Toggle) == State::Inactive,
		"post-handler Ctrl+F inactive must repair a stale active cache");
	Expect(ResolveHandledControlObservation(State::Inactive, State::Inactive,
			ControlAction::Toggle) == State::Inactive,
		"a LevelUp Ctrl+F rejection must remain inactive instead of blind toggling");
	Expect(ResolveHandledControlObservation(State::Active, State::Active,
			ControlAction::Reset) == State::Inactive,
		"Ctrl+R must be inactive even if a presentation mirror remains active");
	Expect(ResolveHandledControlObservation(State::Active, State::Unknown,
			ControlAction::Toggle) == State::Active,
		"an unreadable post-handler Ctrl+F observation must preserve confirmed state");

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
	const ReconcileDecision unresolved = DecideReconcile(
		State::Active, State::Unknown, true);
	Expect(!unresolved.clearCachedTarget && !unresolved.activateSession
			&& !unresolved.endSession,
		"an unknown observation must never end an active session");

	State stableActive = State::Active;
	for (int frame = 0; frame < 1000; ++frame)
	{
		stableActive = MergeObservation(stableActive, State::Inactive,
			ObservationAuthority::Presentation);
	}
	Expect(stableActive == State::Active,
		"repeated transient presentation false values must not detach IME ownership");
	stableActive = MergeObservation(stableActive, State::Inactive,
		ObservationAuthority::Authoritative);
	Expect(stableActive == State::Inactive,
		"an explicit lifecycle close must still terminate after transient mirrors");

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
