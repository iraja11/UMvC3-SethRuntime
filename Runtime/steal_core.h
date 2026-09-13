/* Donor selection for the generic assist-alpha steal.  Read-only: nothing in
   here writes to the match.

   The donor is the character Seth actually HIT with the steal, not simply the
   opponent's point character.  We can tell who that is without writing
   anything: during the capture the victim runs from `chr\SethTE\anmtdown`, and
   nobody else does, so the opponent whose current-action pointer (+0x1390)
   falls inside that resource is our donor.

   The move we borrow is the donor's action 0x0AC (Assist Alpha).  It is the
   one address that holds a usable move for every character in the game,
   including community characters nobody has mapped. */
#ifndef SETH_STEAL_CORE_H
#define SETH_STEAL_CORE_H
#include "probe_core.h"

#define STEAL_ROSTER_RVA      0xd44a70u
#define STEAL_TEAM0_HEAD      0x58u
#define STEAL_TEAM1_HEAD      0x328u
#define STEAL_NODE_ACTOR      8u
#define STEAL_NODE_NEXT       0x10u

#define STEAL_ACTOR_MOTION_L1 0x11c0u
/* An actor keeps TWO copies of its script pointers, and only one of them is
   used to look actions up: the indexed array starting at +0x13E8.  You can see
   it in the getter at +0x140036F70:
       movslq %edx,%rax ; mov 0x13e8(%rcx,%rax,8),%rax ; ret
   Index 0 is anmcmn, 1 is anmchr, 2 is anmtdown.  The copy at +0x1360..+0x1378
   is real, but swapping only that one changes nothing - which is exactly how
   Seth once ended up running his own 0x0AC (Vergil's) instead of the donor's. */
#define STEAL_ACTOR_ANMCMN    0x1360u
#define STEAL_ACTOR_ANMCHR    0x1368u
#define STEAL_ACTOR_ANMTDOWN  0x1370u
#define STEAL_ACTOR_ANMTDOWN2 0x1378u
#define STEAL_ACTOR_BANKS     0x13e8u  /* the indexed array the lookup uses */
#define STEAL_BANK_ANMCMN     0x13e8u  /* = BANKS + 0*8 */
#define STEAL_BANK_ANMCHR     0x13f0u  /* = BANKS + 1*8 */
#define STEAL_BANK_ANMTDOWN   0x13f8u  /* = BANKS + 2*8 */
#define STEAL_ACTOR_RECORD    0x1390u
#define STEAL_ACTOR_ATKINFO   0x1898u
#define STEAL_ACTOR_ATKINFO2  0x34f8u  /* second copy of the same pointer */
#define STEAL_ACTOR_SHOTLIST  0x34f0u
#define STEAL_ACTOR_ATKCLI    0x3500u

#define STEAL_HASH_ANMCHR     0x5a7e5d8au
#define STEAL_HASH_MOTION     0x76820d81u
#define STEAL_MAGIC_ANMCHR    0x00434143u  /* 'CAC\0' */

#define STEAL_OWNER_MOTION    "chr\\SethTE\\motion\\SethTE_l1"
#define STEAL_OWNER_ANMTDOWN  "chr\\SethTE\\anmtdown"

#define STEAL_ASSIST_ACTION   0x0acu  /* Assist Alpha */
#define STEAL_FIRST_FRAME     6u      /* the assist entrance usually takes f0-f5 */
#define STEAL_MAX_ACTIONS     4096u
#define STEAL_MAX_ROSTER      16u

enum StealDecision {
    STEAL_OK = 0,
    STEAL_NO_ROSTER,        /* the roster can't be read, or it loops */
    STEAL_NO_OWNER,         /* SethTE isn't on the field */
    STEAL_AMBIGUOUS_OWNER,  /* more than one SethTE */
    STEAL_NO_VICTIM,        /* nobody took the steal */
    STEAL_NO_ACTION,        /* the donor has no 0x0AC */
    STEAL_BAD_DATA          /* something looked outside the sizes we accept */
};

typedef struct {
    U64 owner;         /* SethTE's actor */
    U64 donor;         /* the actor Seth hit with the steal */
    U64 donor_anmchr;  /* the donor's anmchr resource */
    U64 donor_record;  /* its 0x0AC record */
    U32 frames;        /* the action's declared duration */
    U32 subblocks;
} StealResult;

/* Everything below only reads. */

/* Finds SethTE's actor and which side he is on. */
int steal_find_owner(const ProbeMemory *m, U64 *owner, U32 *side);

/* Finds the opponent currently running Seth's anmtdown - the steal's victim. */
int steal_find_victim(const ProbeMemory *m, U64 owner, U32 side, U64 *victim);

/* Looks an action up in an actor's active anmchr. */
int steal_action_record(const ProbeMemory *m, U64 actor, U32 action,
                        U64 *anmchr, U64 *record, U32 *frames, U32 *subblocks);

/* The full path: owner -> victim -> the donor's 0x0AC.  It only works WHILE
   the capture is happening, so use it to latch the donor, not to use him
   later. */
int steal_select(const ProbeMemory *m, StealResult *out);

/* Checks that an actor is still in the roster, and on which side.  This goes
   by presence, never by resource: during the borrow Seth is holding the
   donor's resources, so identifying him by resource fails exactly when we
   need it most. */
int steal_actor_present(const ProbeMemory *m, U64 actor, U32 *side);

/* Tells which action a record is, by finding its offset in the actor's three
   script banks.  It exists so the log can name actions instead of printing
   raw pointers. */
int steal_action_of(const ProbeMemory *m, U64 actor, U64 record,
                    U32 *action, U32 *bank);

/* Validates a donor that was latched earlier and resolves his 0x0AC.  By the
   time the move comes out the victim has long stopped running Seth's
   anmtdown, so there is no victim search here. */
int steal_prepare(const ProbeMemory *m, U64 donor, StealResult *out);


/* ------------------------------------------------------- the trimmed record

   You can't skip the assist entrance after the fact: entering an action resets
   the frame, and frame 0 has already fired the teleport, the chase physics and
   the freeze-until-landing.  The only way out is for the engine to never see
   those sub-blocks at all.

   So the donor's record is copied VERBATIM into a buffer of ours, and only the
   (frame, offset) table in its header gets compacted: the sub-blocks before the
   cut are dropped and the rest are shifted down.  Sub-block offsets are
   relative to the record, so they stay valid untouched, and the dropped
   entries turn into dead space the engine never reads because the count at +0
   gets smaller.  The weapon-prop animation commands are dropped the same way,
   one level down, from each sub-block's command table.

   What comes out is a synthetic anmchr with a single action, plus a copy of
   the donor's resource object whose data pointer (+0x70) points at it.
   Nothing that belongs to the donor is modified.

   Buffer layout:
       +0x000  copy of the resource object (STEAL_OBJ_BYTES)
       +0x200  synthetic anmchr: 16 header | 8 entry | copied record
*/
#define STEAL_OBJ_BYTES   0x200u
#define STEAL_DATA_AT     0x200u
#define STEAL_RECORD_AT   24u     /* 16 header + one 8-byte entry */
#define STEAL_REC_HEADER  24u     /* +0 count, +4 duration, pairs from here */

/* `base` is the address the buffer will live at inside the game process.
   *out_resource receives the resource object to install, and *props_dropped
   how many prop animation commands were left out. */
int steal_trim(const ProbeMemory *m, U64 donor, U8 *buf, U32 cap, U64 base,
               U64 *out_resource, U32 *kept, U32 *frames, U32 *props_dropped);


/* --------------------------------------------- jumps to the donor's actions

   Some assists don't finish inside 0x0AC: they end with `1_00 GoTo Anmchr
   Script` into a follow-up of their own (Wolverine goes to 0xC5, Ultron to
   0x16, Leilei to 0xB3...).  That jump is looked up in the same slot a cancel
   uses, so it can only land on the donor's action if the donor's anmchr is in
   the slot at that instant - and Seth's has to be there the rest of the time,
   or cancels find the donor's moves.  These helpers let the ASI know where
   the jumps are and recognise the follow-up once the engine enters it. */
#define STEAL_MAX_GOTOS 8u
typedef struct { U32 frame, action; } StealGoto;

/* Lists the `1_00` jumps in a record held in our own memory, in frame order,
   with the class:index already turned into an action id. */
U32 steal_gotos(const U8 *rec, U32 size, StealGoto *out, U32 max);

/* If `record` is one of the actions of this anmchr, gives its id, declared
   duration and byte size (up to the next record). */
int steal_donor_record(const ProbeMemory *m, U64 anmchr, U64 record,
                       U32 *action, U32 *frames, U32 *size);

/* Does this anmchr have the action at all? */
int steal_has_action(const ProbeMemory *m, U64 anmchr, U32 action);

#endif
