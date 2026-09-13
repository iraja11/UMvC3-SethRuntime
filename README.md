# SethRuntimeProbe v4

An ASI for UMvC3 that lets Seth borrow a
move from the opponent he hit with his steal, at runtime, for the entire cast,
including community characters.

There is no character name anywhere in this code. That is a design constraint,
not an accident: a hardcoded cast list would need a rebuild every time the
community adds a character. Per-donor exceptions live in an `.ini` next to the
ASI.

## How it works

**1. Latch the donor.** During the steal's capture the victim, and only the
victim, runs from `chr\SethTE\anmtdown`. The ASI walks the roster and latches
whoever has their current-action pointer (`actor+0x1390`) inside that resource's
data. Read-only; nothing is written.

**2. Trim the prologue.** The borrowed move is the donor's action `0x0AC`
(Assist Alpha) — the one address that exists, with a usable move, across the
whole cast. Its first frames are the assist entrance (teleport, chase physics,
freeze-until-landing), which must not run. The donor's record is copied verbatim
into our own buffer and only the `(frame, offset)` table in its header is
compacted, so the cut becomes frame 0. Sub-block offsets are relative to the
record and stay valid; dropped entries become dead space because the count in
`+0` shrinks. Nothing belonging to the donor is modified.

The cut is the frame of the first sub-block playing an animation from a bank
other than `lmt0`, but it *backs up* if a dropped sub-block holds a command that
must not be lost (`1_7B` animation flip, `1_A2`, `1_107`, `0_07`, or a
`3_30`/`3_31`/`3_32`/`3_34` projectile spawn). Measured across 130 characters,
that changes the cut for exactly 8 of them and leaves the other 122 identical.

**3. Swap the active resources.** anmchr, anmtdown, atkinfo, atkcollision, shot
list, and the class callback table (`+0x6738`/`+0x6740`) are pointed at the
donor's. `mUsrF` (`+0x1510`) is snapshotted and restored, so what the donor's
`1_5A` sets holds during the move but does not survive it. Every write is paired
with a restore of the exact value that was there.

**4. Animation is redirected per call**, not by swapping the slot. Swapping the
bank leaves Seth with the wrong LMT the instant he *leaves* the move — a cancel
enters one of his own actions, asks for `lmt1:N` and gets the donor's motion N,
which is a T-pose. `motion_for_donor()` checks on every call that Seth is still
running the borrowed record, so the redirect stops in the same call he leaves.

**5. Projectiles.** The 4th argument of `sShot__spawnShot` is
`uShot__setOwnerCharacter`, and it decides both the visual (the drawing class
asks for `chr\<owner>\effect\...`) and where the shot is born. The two kinds of
shot want opposite owners, measured in game both ways:

| shot kind | owner = donor | owner = Seth |
|---|---|---|
| drawn by its class (AnimPath empty) | visual works | no visual |
| script-driven (AnimPath set) | no visual, no hitbox | hitbox and position work |

So the choice is made per shot, reading the SHT at spawn time. For class-drawn
shots the donor is the owner and the donor is moved to Seth's position for the
exact duration of the factory call (the call is synchronous and the game is
single-threaded, so nothing sees him out of place). For script-driven shots the
owner stays Seth.

An anmchr script spawns projectiles from **three** callsites, not one:
`3_30` through `sShot__spawnShot`, `3_31` and `3_32` through
`sShot__spawnShotEx`. Only `3_30` used to be hooked, which left 29 characters
spawning shots with nothing to draw them from.

**6. Prop guard.** The indexed path of the weapon-prop commands (`1_F3`..`1_F8`)
does no bounds check: `p = *(cChr **)(*(longlong *)(chr + 0x3F40) + idx * 8)`.
A character with no props has that pointer NULL and the read lands at a low
address. During the borrow Seth's prop array is copied into a 512-entry page
with the tail zeroed and the pointer points at the copy, so an out-of-range
index reads zero instead of crashing. Nothing is taken away from Seth.

**7. Prop animation is left out.** `1_F7 Play Weapon Prop Animation` and `1_F8`
ask for a prop animation by the donor's numbering, but Seth's prop slots hold
Vergil's swords, and in game that throws him into a T-pose. They are dropped
from the trimmed copy the same way the prologue is: each sub-block lists its
commands through a table of offsets, and leaving an entry out is enough.

**8. Cancels get Seth's own actions.** Seth keeps his own anmchr in the lookup
slot for the whole move (only the synthetic one-action bank is installed, and
only until the engine consumes the request), so a hyper cancel finds Seth's
hyper. When the engine flags a pending action change (`+0x14F0` bit 2 turning
on) the borrow ends right away, so the new action never starts with the donor's
atkinfo, shot list or callback table.

**9. Undo everything** at the end and return to idle. One use per steal.

## Files

    steal_core.c/.h   pure read-only logic: donor selection, record trimming.
                      Builds as a native .so and is tested against the running
                      game through /proc/<pid>/mem.
    steal_win.c       hooks, writes, state machine, log
    probe_core.c/.h   motion resolver shared with earlier versions
    build_v4.py       builds; does not install
    validate_steal.py validates the selector against a live game, read-only
    legacy/           superseded versions, kept for reference

Build with `python3 build_v4.py` (clang targeting `x86_64-pc-windows-msvc`,
plus `lld-link`). The core is compiled first under ASan/UBSan as a native
object, so a logic bug shows up on Linux instead of in the game.

Install by copying `build/SethRuntimeProbe.v4.asi` and its `.ini` next to the
game executable. The ASI looks for an `.ini` named after itself.

## Exceptions file

    IronMan = skip     steal nothing from this character
    Amaterasu = 0xCE   Seth runs his OWN action 0xCE instead

Anyone not listed falls through to the generic borrow. The name is the one in
the resource paths (`chr\<Name>\...`), not the display name.

## Known issues

- Iron Man's UniBeam spawns with a hitbox but draws nothing. The class builds
  the beam from `chr\IronMan\effect\mod\%04d` and every lookup succeeds, so it
  is not a missing resource.
- Bishop T-poses as a donor. Not missing animation data and not a mismatched
  motion-slot pairing. (Dante's T-pose was the prop animation below.)
- Assists that jump to another action of their own (`0_01`/`1_00` to a
  follow-up: Wolverine, Leilei, KTho, Neroe, Juri, WL3, Lilithan, Ultro) are not
  supported. The borrow ends at the jump, and since Seth keeps his own anmchr
  during the move, the jump lands on Seth's action with the same id.
- Donor props do not appear on Seth. A prop is an object bound to the donor's
  skeleton, not a resource resolved by path, so the projectile trick does not
  apply. Ghost Rider's chain does appear but comes out short, because its length
  comes from a bone chain Seth's skeleton does not have.

## Method

Everything here was measured, not guessed. Three traps cost the most, all three
the same mistake in different clothes:

- **Confirm object fields in the assembly of the getter/setter, never in the
  decompiler output.** It cost a wrong vtable base (the DTI vtable is not the
  object vtable — take the base from the constructor), and a `field1_0x70` that
  the decompiler shows as a pointer and is an embedded struct.
- **Count a hooked function's arguments in the caller's assembly.** The path
  composer takes five and `sShot__spawnShotEx` takes twelve; declaring fewer
  leaves stack slots holding the hook's own garbage and crashes the game.
- **The actor holds two copies of almost every resource pointer, and the engine
  reads only one.** The one that matters is whichever the indexed getter uses.
