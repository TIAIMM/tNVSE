#include "multibyte_input_ime_internal.h"

namespace fonthook
{
	namespace multibyte_input
	{
		namespace implementation::multibyte_input_broker {}
		using namespace implementation::multibyte_input_broker;

		namespace implementation::multibyte_input_broker
		{
			bool SameTokenIdentity(
				const TextInputTargetToken& lhs,
				const TextInputTargetToken& rhs)
			{
				return lhs.kind == rhs.kind
					&& lhs.identity == rhs.identity
					&& lhs.secondaryIdentity == rhs.secondaryIdentity
					&& lhs.writable == rhs.writable;
			}

			bool HasInputClass(
				GameInputFilterClass value,
				GameInputFilterClass flag)
			{
				return (static_cast<UInt8>(value)
					& static_cast<UInt8>(flag)) != 0;
			}

			TextInputTarget MakeTextEditTarget(
				TextInputTargetKind kind,
				TextEditMenu* menu,
				bool writable,
				bool active)
			{
				TextInputTarget target;
				target.token.kind = kind;
				target.token.identity = menu;
				target.token.writable = writable;
				target.token.active = active;
				target.textEdit = menu;
				return target;
			}
		}

		const char* TextInputTargetKindName(TextInputTargetKind kind)
		{
			switch (kind)
			{
			case TextInputTargetKind::TextEdit:
				return "TextEdit";
			case TextInputTargetKind::JipTextInput:
				return "JipTextInput";
			case TextInputTargetKind::Stewie:
				return "Stewie";
			case TextInputTargetKind::DialogueHistory:
				return "DialogueHistory";
			case TextInputTargetKind::McmExtender:
				return "McmExtender";
			default:
				return "None";
			}
		}

		TextInputTarget ResolveCurrentTextInputTarget()
		{
			if (TextEditMenu* menu = GetActiveTextEditMenu())
				return MakeTextEditTarget(
					TextInputTargetKind::TextEdit, menu, true, true);

			TextEditMenu* jipMenu = GetActiveJipTextInputMenu();
			if (jipMenu)
				return MakeTextEditTarget(
					TextInputTargetKind::JipTextInput, jipMenu, true, true);

			jipMenu = GetCurrentJipTextInputMenu();
			if (jipMenu)
				return MakeTextEditTarget(
					TextInputTargetKind::JipTextInput, jipMenu, true, false);

			if (TextEditMenu* menu = GetCurrentTextEditMenuObject())
				return MakeTextEditTarget(
					TextInputTargetKind::TextEdit, menu, false, false);

			const StewieInputTarget stewie = GetOverlayStewieInputTarget();
			if (stewie.valid)
			{
				TextInputTarget target;
				target.token.kind = TextInputTargetKind::Stewie;
				target.token.identity = stewie.menu;
				target.token.secondaryIdentity = stewie.tile;
				target.token.writable = true;
				target.token.active = true;
				target.stewie = stewie;
				return target;
			}

			const DialogueHistoryInputTarget dialogueHistory =
				GetOverlayDialogueHistoryInputTarget();
			if (dialogueHistory.valid)
			{
				TextInputTarget target;
				target.token.kind = TextInputTargetKind::DialogueHistory;
				target.token.identity = dialogueHistory.menu;
				target.token.secondaryIdentity = dialogueHistory.search;
				target.token.writable = true;
				target.token.active = true;
				target.dialogueHistory = dialogueHistory;
				return target;
			}

			const McmExtenderInputTarget mcmExtender =
				GetOverlayMcmExtenderInputTarget();
			if (mcmExtender.valid)
			{
				TextInputTarget target;
				target.token.kind = TextInputTargetKind::McmExtender;
				target.token.identity = mcmExtender.menu;
				target.token.secondaryIdentity = mcmExtender.search;
				target.token.writable = true;
				target.token.active = true;
				target.mcmExtender = mcmExtender;
				return target;
			}

			return {};
		}

		void AdvanceTextInputSessionGeneration(const char* reason)
		{
			ImeState& state = State();
			if (++state.textInputSessionGeneration == 0)
				state.textInputSessionGeneration = 1;
			state.currentTextInputTarget.token.generation =
				state.textInputSessionGeneration;
			DebugLog(
				"tnvse_multibyte_input: text target generation=%u reason=%s",
				state.textInputSessionGeneration,
				reason ? reason : "unknown");
		}

		void SynchronizeTextInputTarget(const char* reason)
		{
			ImeState& state = State();
			TextInputTarget resolved = ResolveCurrentTextInputTarget();
			if (!SameTokenIdentity(
					state.currentTextInputTarget.token,
					resolved.token))
			{
				const TextInputTargetKind previousKind =
					state.currentTextInputTarget.token.kind;
				AdvanceTextInputSessionGeneration(
					reason ? reason : "target_changed");
				AdvanceTsfCandidateSession();
				ResetImeCommitKeyState("text_input_target_changed");
				s_imeComposing = false;
				ClearImePreviewState();
				HideCandidateOverlay();
				resolved.token.generation = state.textInputSessionGeneration;
				state.currentTextInputTarget = resolved;
				DebugLog(
					"tnvse_multibyte_input: text target changed previous=%s current=%s generation=%u",
					TextInputTargetKindName(previousKind),
					TextInputTargetKindName(resolved.token.kind),
					state.textInputSessionGeneration);
				return;
			}

			resolved.token.generation = state.textInputSessionGeneration;
			state.currentTextInputTarget = resolved;
		}

		void ResetTextInputBroker()
		{
			ImeState& state = State();
			state.currentTextInputTarget = {};
			if (++state.textInputSessionGeneration == 0)
				state.textInputSessionGeneration = 1;
			state.currentTextInputTarget.token.generation =
				state.textInputSessionGeneration;
		}

		TextInputTarget GetCachedTextInputTarget()
		{
			return State().currentTextInputTarget;
		}

		TextInputTargetToken CaptureTextInputTargetToken()
		{
			TextInputTargetToken token =
				State().currentTextInputTarget.token;
			token.generation = State().textInputSessionGeneration;
			return token;
		}

		bool IsCurrentTextInputTargetToken(
			const TextInputTargetToken& token)
		{
			const ImeState& state = State();
			return token.generation == state.textInputSessionGeneration
				&& SameTokenIdentity(
					token,
					state.currentTextInputTarget.token);
		}

		bool HasCurrentTextInputTarget()
		{
			return State().currentTextInputTarget.token.kind
				!= TextInputTargetKind::None;
		}

		ImeCommitInputChannel TextInputTargetCommitChannel(
			const TextInputTarget& target)
		{
			switch (target.token.kind)
			{
			case TextInputTargetKind::TextEdit:
				return ImeCommitInputChannel::TextEdit;
			case TextInputTargetKind::JipTextInput:
				return ImeCommitInputChannel::JipTextInput;
			case TextInputTargetKind::Stewie:
				return ImeCommitInputChannel::Stewie;
			case TextInputTargetKind::DialogueHistory:
				return ImeCommitInputChannel::DialogueHistory;
			case TextInputTargetKind::McmExtender:
				return ImeCommitInputChannel::McmExtender;
			default:
				return ImeCommitInputChannel::WndProcChar;
			}
		}

		bool InsertWideTextIntoTarget(
			const TextInputTarget& target,
			std::wstring_view text)
		{
			switch (target.token.kind)
			{
			case TextInputTargetKind::TextEdit:
				return target.token.writable
					&& target.textEdit
					&& InsertWideText(target.textEdit, text);
			case TextInputTargetKind::JipTextInput:
				return target.token.writable
					&& target.textEdit
					&& InsertWideTextJip(target.textEdit, text);
			case TextInputTargetKind::Stewie:
				return target.stewie.valid
					&& InsertWideTextStewie(target.stewie, text);
			case TextInputTargetKind::DialogueHistory:
				return target.dialogueHistory.valid
					&& InsertWideTextDialogueHistory(
						target.dialogueHistory, text);
			case TextInputTargetKind::McmExtender:
				return target.mcmExtender.valid
					&& InsertWideTextMcmExtender(
						target.mcmExtender, text);
			default:
				return false;
			}
		}

		GameInputFilterResult FilterGameInput(
			UInt32 input,
			ImeCommitInputChannel channel,
			GameInputFilterClass inputClass)
		{
			if (ShouldSuppressImeCommitInput(input, channel))
				return GameInputFilterResult::SuppressImeCommit;

			if (HasInputClass(
					inputClass,
					GameInputFilterClass::AllDuringComposition)
				&& IsImeCompositionActive())
			{
				ObserveImeCommitInput(input);
				return GameInputFilterResult::SuppressCompositionControl;
			}

			if (HasInputClass(
					inputClass,
					GameInputFilterClass::PrintableAscii)
				&& IsImeConsumingAscii())
			{
				ObserveImeCommitInput(input);
				return GameInputFilterResult::SuppressCompositionAscii;
			}

			if (HasInputClass(
					inputClass,
					GameInputFilterClass::CompositionControl)
				&& IsImeCompositionActive())
			{
				ObserveImeCommitInput(input);
				return GameInputFilterResult::SuppressCompositionControl;
			}

			return GameInputFilterResult::Pass;
		}
	}
}
