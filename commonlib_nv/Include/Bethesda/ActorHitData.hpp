#pragma once

#include "NiPoint3.hpp"

class Actor;
class Projectile;
class Explosion;
class TESObjectWEAP;

struct ActorHitData {
	ActorHitData() = default;
	ActorHitData(Actor* source, Actor* target, TESObjectWEAP* weapon) : source(source), target(target), weapon(weapon) {}

	enum HitFlags : uint32_t {
		kFlag_TargetIsBlocking       = 0x1,
		kFlag_TargetWeaponOut        = 0x2,
		kFlag_IsCritical             = 0x4,
		kFlag_OnDeathCritEffect      = 0x8,
		kFlag_IsFatal                = 0x10,
		kFlag_ExplodeLimb            = 0x40,
		kFlag_CrippleLimb            = 0x80,
		kFlag_BreakWeaponNonEmbedded = 0x100,
		kFlag_BreakWeaponEmbedded    = 0x200,
		kFlag_BreakWeapon            = 0x300,
		kFlag_IsSneakAttack          = 0x400,
		kFlag_Unk800                 = 0x800,
		kFlag_Unk1000                = 0x1000,
		kFlag_IsExplosionHit         = 0x2000,
		kFlag_ArmorPenetrated        = 0x80000000,
	};

	Actor* source{};		// 00
	Actor* target{};		// 04
	union								// 08
	{
		Projectile* projectile{};
		Explosion* explosion;
	};
	UInt32				unk0C{};			// 0C
	SInt32				hitLocation{};	    // 10
	float				healthDmg{};		// 14
	float				wpnBaseDmg{};		// 18	Skill and weapon condition modifiers included
	float				fatigueDmg{};		// 1C
	float				limbDmg{};		    // 20
	float				blockDTMod{};		// 24
	float				armorDmg{};		    // 28
	float				flt2C{};			// 2C
	TESObjectWEAP*		weapon{};		    // 30
	float				healthPerc{};		// 34
	NiPoint3			impactPos{};		// 38
	NiPoint3			impactAngle{};	    // 44
	UInt32				critEffect{};			// 50
	void*				vatsTargetInfo{};	// 54
	UInt32				flags{};			// 58
	float				dmgMult{1};		    // 5C
	UInt32				unk60{};			// 60	Unused
};