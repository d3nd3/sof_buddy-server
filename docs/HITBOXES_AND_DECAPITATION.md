# Decapitation damage and the Widowmaker hair hitbox

Research notes for two related questions:

1. does decapitation itself add damage;
2. why the female `Widowmaker` player model feels harder to headshot or
   decapitate than other female skins.

## Short answer

- **No, decapitation does not add damage.** The head/neck zones already have a
  **10x damage scale**, and decapitation is a gore result applied *after* the
  killing damage has been calculated. A decapitating hit is necessarily a
  lethal head hit, so it correlates with high damage, but the sever/head
  explosion itself is not a damage bonus.
- **Widowmaker is a special case, not a general female-skin property.** Her
  `.gpm` hides the normal female hair meshes and attaches a separate
  `gina_hair` bolt-on. The normal female hair meshes are head-zone owners
  (`GZ_HEAD_BACK`); `gina_hair` is not a zone owner. It is still a real
  53-triangle collision mesh, so a shot that hits only the hair resolves no
  zone and does **zero** damage. A shot that passes through the hair into the
  head still resolves normally.

## Damage by gore zone

`SetGoreZoneDamageScale_human` (`gamex86.dll` RVA `0x30A70`) writes the 26
human damage scales in zone order:

| Zones | Scale |
|---|---:|
| `GZ_HEAD_FRONT`, `GZ_HEAD_BACK`, `GZ_NECK` | **10.0** |
| shoulders, chest, gut, groin, extra gut | 1.0 |
| upper/lower arms, upper/lower legs, feet | 0.5 |

The zone table is at RVA `0x132F78`; the selected front-zone index is the
global at RVA `0x130650`.

## When a head is severed

`bodyhuman_damage` (RVA `0x2B8E0`) resolves the entry zone and applies the
damage. The gore/sever routine (`sub_50028070`, head-explosion call at
`0x500289DC`) only enters the sever path when all of these are true:

- the hit is lethal (`health <= damage`);
- `lock_sever` is off;
- the zone is a severable body cap;
- and either the damage flags contain `SPREAD` (`0x20000`) or `EXPLOSIVE`
  (`0x8000`), or the zone's accumulated damage is **greater than 500**
  (`cmp [ebp+24h], 1F4h` at `0x500288EB`).

The head is then replaced by the neck cap/head-explosion effects. The damage
that killed the target was already applied; the sever branch does not add
anything.

For a normal player with 100 health, repeated hits on a living zone accumulate
at most the remaining health per hit, so ordinary bullets cannot reach the
>500 threshold on a living player. Decapitation in normal play therefore comes
from weapons/flags that bypass the threshold (shotgun spread, explosives) or
from shooting a corpse until the zone threshold is crossed. `ai_goretest 1`
also forces every killing blow to sever.

## Widowmaker

The stock model is `ghoul/pmodels/Widowmaker.gpm` (not "Windowmaker"). It uses:

- base model: `enemy/female`, player model `fem_play`;
- visible parts: `_LEAR`, `_REAR`;
- hidden parts: `_CREWHAIR`, `_MEDMHAIRMHEAD`, `_LBANG`, `_RBANG`,
  `_DRESSHIPS`;
- custom bolt-on: `boltons/gina_hair.gbm` on `abolt_head_t`, scale
  `1.142857`;
- hip accessories: `gina_guns` on both hip bolts.

The important difference from other female player models:

| Model | Head-back hair | Head-zone owner? |
|---|---|---|
| Princess, Icepick, Stiletto, Taylor, Whisper | `_CREWHAIR` or `_MEDMHAIRMHEAD` | yes, maps to `GZ_HEAD_BACK` |
| Widowmaker | `gina_hair` bolt-on | no; separate child instance |

`gina_hair` is a real mesh: `ACC_GINA_HAIR.ghb` has 1 material, 1 mesh,
30 vertices, 53 triangles. The Ghoul ray test recurses into child instances,
so the hair is part of the trace. A hit record on the hair has the hair
instance, not the body instance. `GetProjectileFrontHole` (`0x2A830`) only
maps body-instance parts to zones; if no body zone appears before the exit
record, `bodyhuman_damage` returns 0. The one special case for the base
`_mohawk` part (`return 0` when only that part was hit) does not help a custom
bolt-on.

Approximate model-space bounds at frame 0 (translation-only attachment
approximation, no parent-bone rotation):

| Part | X | Y | Z |
|---|---|---|---|
| base head owners `_HEADCREW` + `_COMFACE` | -3.0 .. 2.7 | -7.6 .. -2.1 | 18.6 .. 25.0 |
| `gina_hair` at 1.142857 scale | -8.3 .. 2.1 | -8.2 .. -0.2 | 20.3 .. 29.4 |

So the hair is roughly 2-3x the head's bounding volume in each axis and
extends several units above and behind it. A rough parallel-ray sweep over
that region found the hair as the first surface far more often than the actual
head-zone triangles, especially from the back and top. Treat those exact
ratios as approximate; the bounding-volume result and the zone-owner
difference are the reliable parts.

## Telemetry caveat

The native `player_weapon_accuracy_stats` hit-zone counters only accept a zone
when `bodyhuman_damage` returned a real hit and the damage came through the
traced/projectile resolver. Hair/ear/accessory-only hits are ignored, and
spread/explosive/torso-blast damage is skipped because it does not refresh the
zone global. This avoids attributing a miss on hair to the previous shot's
zone. Exact zone rows therefore cover bullets/projectiles; shotgun spread and
explosions are not represented in the zone counters yet.

## Evidence

- `SetGoreZoneDamageScale_human` RVA `0x30A70` (IDA).
- `bodyhuman_damage` RVA `0x2B8E0`; `GetProjectileFrontHole` RVA `0x2A830`.
- Head-explosion/sever branch `sub_50028070`, threshold at `0x500288EB`,
  `FX_HeadExplosion` call at `0x500289DC`.
- `Widowmaker.gpm`, `gina_hair.gbm`, `ACC_GINA_HAIR.ghb` in the 1.07 pak set.
- OpenMerc `SPEC-720` (damage scales), `SPEC-721` (sever/head explosion),
  `SPEC-722` (female body setup), and the `g3_human.cpp`/`g3_gore.cpp`
  reconstruction for the hit-list/zone rules.
