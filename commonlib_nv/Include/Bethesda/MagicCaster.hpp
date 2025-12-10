#pragma once

class TESForm;
class MagicItem;
class SpellItem;
class MagicTarget;
class MagicLight;
class ActiveEffect;
class EffectItem;
class TESBoundObject;
class NiNode;

class MagicCaster {
public:
	MagicCaster();
	~MagicCaster();

	virtual void			CastAbility(SpellItem* apAbility, bool abLoadCast);
	virtual void			CastAddiction(SpellItem* apAddiction, bool abLoadCast);
	virtual void			Unk_02();
	virtual void			CastSpellImmediate(MagicItem* apMagicItem, bool, MagicTarget* apTarget, float afEffectivenessMult, bool abAdjustOnlyHostileEffectiveness);
	virtual void			TransferDisease(SpellItem* apDisease, MagicTarget* apTarget, bool a4);
	virtual void			CastWornEnchantment(MagicItem* apSpell, TESBoundObject* apSource, bool abLoadCast);
	virtual void			StartAim();
	virtual void			Unk_07();
	virtual void			StartCastImpl();
	virtual void			SpellCast(MagicItem* apSpell, bool);
	virtual bool			CheckCast(MagicItem* apSpell, float*, UInt32* apReason, bool);
	virtual Actor*			GetCasterAsActor() const;
	virtual NiNode*			GetMagicNode() const;
	virtual MagicItem*		GetCurrentSpell() const;
	virtual bool			TargetSelf(ActiveEffect* apActiveEffect);
	virtual float			GetMagicEffectivenessModifier(bool, float);
	virtual void			SetCurrentSpell(MagicItem* apMagicItem);
	virtual void			Unk_11();
	virtual void			SetDesiredTarget(MagicTarget*);
	virtual ActiveEffect*	CreateActiveEffect(MagicItem* apMagicItem, EffectItem* apEffectItem, TESBoundObject* apObject);

	MagicLight* pMagicLight;
	DWORD		uiState;
};

ASSERT_SIZE(MagicCaster, 0x0C);