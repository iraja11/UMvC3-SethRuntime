/* Read-only half of the steal: finding Seth, finding who he grabbed, finding
   the move to borrow and building a trimmed copy of it.

   Nothing in this file writes to the running match.  Every read goes through
   the ProbeMemory callback, which is why the same code runs inside the game
   (as part of the ASI) and on Linux against /proc/<pid>/mem for validation. */
#include "steal_core.h"

static U32 u32(const U8 *p) {
    return (U32)p[0] | ((U32)p[1]<<8) | ((U32)p[2]<<16) | ((U32)p[3]<<24);
}
static U64 u64(const U8 *p) { return u32(p) | ((U64)u32(p+4)<<32); }

static int rd(const ProbeMemory *m, U64 p, void *out, U64 n) {
    return p >= 0x10000 && p+n >= p && m->read(m->context, p, out, n);
}
static int ptr(const ProbeMemory *m, U64 p, U64 *out) {
    U8 d[8];
    if (!rd(m, p, d, 8)) return 0;
    *out = u64(d);
    return 1;
}
static int name_is(const U8 *p, const char *name) {
    U32 i;
    for (i = 0; i < 64; i++) {
        if (p[i] != (U8)name[i]) return 0;
        if (!name[i]) return 1;
    }
    return 0;
}
/* A resource object carries its path at +12, its type hash at +0x64 and a
   pointer to the file data at +0x70.  This checks the first two and hands
   back the third. */
static int resource(const ProbeMemory *m, U64 p, U32 hash, const char *name, U64 *data) {
    U8 d[0x78];
    if (!rd(m, p, d, sizeof(d)) || u32(d+0x64) != hash) return 0;
    if (name && !name_is(d+12, name)) return 0;
    *data = u64(d+0x70);
    return *data != 0;
}

/* The address range an anmchr occupies in memory: from the start of its data
   to the largest record offset in the table.  The last record has no declared
   size, so the table itself is all we have to go on - which is why callers
   compare against this limit with `<=`. */
static int anmchr_extent(const ProbeMemory *m, U64 data, U32 *count, U64 *last) {
    U8 h[16], e[8];
    U32 n, i, off, top = 0;
    if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) return 0;
    n = u32(h+8);
    if (!n || n > STEAL_MAX_ACTIONS) return 0;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return 0;
        off = u32(e+4);
        if (off < 16+n*8) return 0;
        if (off > top) top = off;
    }
    *count = n;
    *last = data + top;
    return 1;
}

static int each_actor(const ProbeMemory *m, U32 side, U64 *list, U32 *n) {
    U64 node, next, actor, seen[STEAL_MAX_ROSTER], root;
    U32 i, k = 0;
    if (!ptr(m, m->exe + STEAL_ROSTER_RVA, &root) || !root) return 0;
    if (!ptr(m, root + (side ? STEAL_TEAM1_HEAD : STEAL_TEAM0_HEAD), &node)) return 0;
    while (node && k < STEAL_MAX_ROSTER) {
        for (i = 0; i < k; i++) if (seen[i] == node) return 0;  /* the list loops back on itself */
        seen[k] = node;
        if (!ptr(m, node + STEAL_NODE_ACTOR, &actor) ||
            !ptr(m, node + STEAL_NODE_NEXT, &next)) return 0;
        if (actor) list[k++] = actor;
        node = next;
    }
    if (node) return 0;  /* longer than any real team can be; don't trust it */
    *n = k;
    return 1;
}

int steal_actor_present(const ProbeMemory *m, U64 actor, U32 *side) {
    U64 list[STEAL_MAX_ROSTER];
    U32 n, i, s;
    if (!actor) return STEAL_NO_OWNER;
    for (s = 0; s < 2; s++) {
        if (!each_actor(m, s, list, &n)) return STEAL_NO_ROSTER;
        for (i = 0; i < n; i++) if (list[i] == actor) { *side = s; return STEAL_OK; }
    }
    return STEAL_NO_OWNER;
}

int steal_find_owner(const ProbeMemory *m, U64 *owner, U32 *side) {
    U64 list[STEAL_MAX_ROSTER], data;
    U32 n, i, s, found = 0;
    for (s = 0; s < 2; s++) {
        if (!each_actor(m, s, list, &n)) return STEAL_NO_ROSTER;
        for (i = 0; i < n; i++) {
            U64 res;
            if (!ptr(m, list[i] + STEAL_ACTOR_MOTION_L1, &res) || !res) continue;
            if (resource(m, res, STEAL_HASH_MOTION, STEAL_OWNER_MOTION, &data)) {
                *owner = list[i];
                *side = s;
                found++;
            }
        }
    }
    if (!found) return STEAL_NO_OWNER;
    if (found > 1) return STEAL_AMBIGUOUS_OWNER;
    return STEAL_OK;
}

int steal_find_victim(const ProbeMemory *m, U64 owner, U32 side, U64 *victim) {
    U64 list[STEAL_MAX_ROSTER], anmtdown, data, last, cur, held;
    U32 n, i, count, found = 0;

    /* Seth's OWN anmtdown is the yardstick: whoever is currently executing
       inside its data is the one being grabbed. */
    if (!ptr(m, owner + STEAL_ACTOR_ANMTDOWN, &anmtdown) || !anmtdown) return STEAL_NO_VICTIM;
    if (!resource(m, anmtdown, STEAL_HASH_ANMCHR, STEAL_OWNER_ANMTDOWN, &data)) return STEAL_NO_VICTIM;
    if (!anmchr_extent(m, data, &count, &last)) return STEAL_BAD_DATA;

    if (!each_actor(m, side ? 0u : 1u, list, &n)) return STEAL_NO_ROSTER;
    for (i = 0; i < n; i++) {
        if (list[i] == owner) continue;
        /* Holding a pointer to Seth's anmtdown is not enough on its own: that
           pointer stays behind on everyone Seth has ever grabbed this match.
           What only holds DURING the capture is the current action living
           inside that data, so we require both. */
        if (!ptr(m, list[i] + STEAL_ACTOR_ANMTDOWN2, &held) || held != anmtdown) continue;
        if (!ptr(m, list[i] + STEAL_ACTOR_RECORD, &cur) || !cur) continue;
        if (cur < data || cur > last) continue;
        *victim = list[i];
        found++;
    }
    if (found != 1) return STEAL_NO_VICTIM;
    return STEAL_OK;
}

int steal_action_record(const ProbeMemory *m, U64 actor, U32 action,
                        U64 *anmchr, U64 *record, U32 *frames, U32 *subblocks) {
    U64 res, data, rec;
    U8 h[16], e[8], r[8];
    U32 n, i, off;
    /* Read the bank the engine actually indexes (+0x13E8 + 1*8), not the
       other copy of the pointer. */
    if (!ptr(m, actor + STEAL_BANK_ANMCHR, &res) || !res)
        if (!ptr(m, actor + STEAL_ACTOR_ANMCHR, &res) || !res) return STEAL_BAD_DATA;
    if (!resource(m, res, STEAL_HASH_ANMCHR, 0, &data)) return STEAL_BAD_DATA;
    if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) return STEAL_BAD_DATA;
    n = u32(h+8);
    if (!n || n > STEAL_MAX_ACTIONS) return STEAL_BAD_DATA;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        if (u32(e) != action) continue;
        off = u32(e+4);
        if (off < 16+n*8) return STEAL_BAD_DATA;
        rec = data + off;
        if (!rd(m, rec, r, 8)) return STEAL_BAD_DATA;
        /* +0 is the sub-block count and +4 the duration.  A usable assist has
           at least the prologue plus the move, so frame 6 has to fit inside. */
        if (!u32(r) || u32(r) > 1024) return STEAL_BAD_DATA;
        if (u32(r+4) <= STEAL_FIRST_FRAME || u32(r+4) > 100000) return STEAL_BAD_DATA;
        *anmchr = res;
        *record = rec;
        *subblocks = u32(r);
        *frames = u32(r+4);
        return STEAL_OK;
    }
    return STEAL_NO_ACTION;
}

int steal_action_of(const ProbeMemory *m, U64 actor, U64 record,
                    U32 *action, U32 *bank) {
    static const U32 banks[3] = { STEAL_ACTOR_BANKS, STEAL_ACTOR_BANKS + 8u,
                                  STEAL_ACTOR_BANKS + 16u };
    U64 res, data;
    U8 h[16], e[8];
    U32 b, n, i;
    if (!record) return STEAL_BAD_DATA;
    for (b = 0; b < 3; b++) {
        if (!ptr(m, actor + banks[b], &res) || !res) continue;
        if (!resource(m, res, STEAL_HASH_ANMCHR, 0, &data)) continue;
        if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) continue;
        n = u32(h+8);
        if (!n || n > STEAL_MAX_ACTIONS) continue;
        if (record < data) continue;
        for (i = 0; i < n; i++) {
            if (!rd(m, data+16+(U64)i*8, e, 8)) break;
            if (data + u32(e+4) == record) {
                *action = u32(e);
                *bank = b;
                return STEAL_OK;
            }
        }
    }
    return STEAL_NO_ACTION;
}

int steal_prepare(const ProbeMemory *m, U64 donor, StealResult *out) {
    U64 list[STEAL_MAX_ROSTER], anmchr = 0, record = 0;
    U32 n, i, side = 0, frames = 0, subs = 0, owner_side = 0, present = 0;
    U64 owner = 0;
    int d;
    if (!donor) return STEAL_NO_VICTIM;
    if ((d = steal_find_owner(m, &owner, &owner_side)) != STEAL_OK) return d;
    if (donor == owner) return STEAL_NO_VICTIM;

    /* The donor has to still be on the other team.  If they were KO'd or
       tagged out, that address may already belong to a different actor, so a
       matching pointer proves nothing - they have to be in the roster. */
    side = owner_side ? 0u : 1u;
    if (!each_actor(m, side, list, &n)) return STEAL_NO_ROSTER;
    for (i = 0; i < n; i++) if (list[i] == donor) present = 1;
    if (!present) return STEAL_NO_VICTIM;

    if ((d = steal_action_record(m, donor, STEAL_ASSIST_ACTION,
                                 &anmchr, &record, &frames, &subs)) != STEAL_OK) return d;
    out->owner = owner;
    out->donor = donor;
    out->donor_anmchr = anmchr;
    out->donor_record = record;
    out->frames = frames;
    out->subblocks = subs;
    return STEAL_OK;
}

int steal_select(const ProbeMemory *m, StealResult *out) {
    U64 owner = 0, victim = 0, anmchr = 0, record = 0;
    U32 side = 0, frames = 0, subs = 0;
    int d;
    if ((d = steal_find_owner(m, &owner, &side)) != STEAL_OK) return d;
    if ((d = steal_find_victim(m, owner, side, &victim)) != STEAL_OK) return d;
    if ((d = steal_action_record(m, victim, STEAL_ASSIST_ACTION,
                                 &anmchr, &record, &frames, &subs)) != STEAL_OK) return d;
    out->owner = owner;
    out->donor = victim;
    out->donor_anmchr = anmchr;
    out->donor_record = record;
    out->frames = frames;
    out->subblocks = subs;
    return STEAL_OK;
}

/* ------------------------------------------- the record without its prologue */

static void put32(U8 *p, U32 v) {
    p[0]=(U8)v; p[1]=(U8)(v>>8); p[2]=(U8)(v>>16); p[3]=(U8)(v>>24);
}
static void put64(U8 *p, U64 v) { put32(p, (U32)v); put32(p+4, (U32)(v>>32)); }

/* Frame jumps inside the move carry an ABSOLUTE frame of the record: the third
   value of `0_02`/`0_04 GoTo Frame On Condition` and the first of `0_1C GoTo
   Frame` go straight to the context's GoToFrame (anmchrCmd0_interpreter,
   +0x100DE0).  Shifting the sub-blocks down without shifting these made every
   jump land `corte` frames late.  Wolverine's hit confirm at f22 jumps to 29,
   which in the copy is the frame of the `1_00` itself: the jump fired before
   the goto window could open, and Seth ran his OWN 0xC5 - frozen in the air.
   A target inside the dropped prologue goes to frame 0. */
static void shift_frame_jump(U8 *rec, U32 size, U32 co, U32 corte) {
    U32 code, np, slot, at;
    if (co + 16 > size || u32(rec+co) != 0) return;
    code = u32(rec+co+4);
    if (code == 0x02 || code == 0x04) slot = 2;
    else if (code == 0x1c) slot = 0;
    else return;
    np = u32(rec+co+8);
    if (np <= slot || np > 31 || co + 16 + np*8 > size) return;
    if (u32(rec + co + 16 + slot*4) == 6) return;      /* float tag: not a frame */
    at = co + 16 + np*4 + slot*4;
    put32(rec + at, u32(rec + at) >= corte ? u32(rec + at) - corte : 0);
}

/* Where does the move REALLY start for this donor?  The assist prologue plays
   its animations from bank lmt0, so the first sub-block that plays something
   from any other bank is where the move begins. */
/* Commands we can't afford to lose to the cut.

   "Cut at the first animation outside lmt0" is right for almost everyone, but
   some characters start the move a few frames before the animation changes.
   Measured across the cast, eight of them lost something important that way:
   Evil Ryu his `1_7B Animation Flip`, Xero and ServbotA their projectile,
   Krauser a `0_07 GoTo`, and four others a `1_A2`.

   So we still cut at the animation, but BACK UP to the first dropped sub-block
   that holds one of these.  Frames 0 and 1 are prologue for the whole cast, so
   they never count.  For the other 122 characters this changes nothing. */
static int cmd_indispensavel(U32 grupo, U32 code) {
    if (grupo == 0) return code == 0x07;                    /* GoTo */
    if (grupo == 1) return code == 0x7b || code == 0xa2 || code == 0x107;
    if (grupo == 3) return code == 0x30 || code == 0x31 ||  /* projectile spawns */
                           code == 0x32 || code == 0x34;
    return 0;
}

/* Weapon-prop animation commands, which we leave out of the copy.

   `1_F7 Play Weapon Prop Animation` and `1_F8` (its speed) ask for a prop
   animation using the DONOR's numbering.  Seth is a Vergil clone, so his prop
   slots hold Vergil's swords rather than the donor's weapon, and in game the
   request throws Seth into a T-pose (Dante's Jam Session shows it clearly).
   Twelve assists in the cast use `1_F7`.

   Props can't be borrowed anyway (a prop is an object bound to the donor's
   skeleton), so dropping the animation request loses nothing visible.  The
   spawn commands (`1_F3`..`1_F6`) stay, covered by the prop guard in
   steal_win.c. */
static int cmd_prop_anim(U32 grupo, U32 code) {
    return grupo == 1 && (code == 0xf7 || code == 0xf8);
}

/* Frame of the first sub-block that plays an animation from a bank other
   than 0. */
static U32 corte_pela_animacao(const U8 *rec, U32 size) {
    U32 subs = u32(rec), i, k;
    for (i = 0; i < subs; i++) {
        const U8 *par = rec + STEAL_REC_HEADER + i*8;
        U32 fr = u32(par), po = u32(par+4), ncmd;
        if (po + 16 > size) continue;
        ncmd = u32(rec + po + 4);
        if (!ncmd || ncmd > 256) continue;
        if (po + 16 + ncmd*8 > size) continue;
        for (k = 0; k < ncmd; k++) {
            U32 co = po + u32(rec + po + 16 + k*8), np, vo;
            if (co + 12 > size) continue;
            if (u32(rec+co) != 0 || u32(rec+co+4) != 0x21) continue;   /* 0_21 */
            np = u32(rec+co+8);
            if (!np || np > 31) continue;
            vo = co + 16 + np*4;
            if (vo + 4 > size) continue;
            if (u32(rec+vo) != 0) return fr;      /* any bank but lmt0 is the move itself */
        }
    }
    return STEAL_FIRST_FRAME;
}

static U32 corte_do_golpe(const U8 *rec, U32 size) {
    U32 subs = u32(rec), i, k, anim;
    if (!subs || subs > 1024) return STEAL_FIRST_FRAME;
    if (STEAL_REC_HEADER + subs*8u > size) return STEAL_FIRST_FRAME;
    anim = corte_pela_animacao(rec, size);
    for (i = 0; i < subs; i++) {
        const U8 *par = rec + STEAL_REC_HEADER + i*8;
        U32 fr = u32(par), po = u32(par+4), ncmd;
        if (fr >= anim) break;
        if (fr < 2) continue;              /* f0 and f1 are prologue for everyone */
        if (po + 16 > size) continue;
        ncmd = u32(rec + po + 4);
        if (!ncmd || ncmd > 256) continue;
        if (po + 16 + ncmd*8 > size) continue;
        for (k = 0; k < ncmd; k++) {
            U32 co = po + u32(rec + po + 16 + k*8);
            if (co + 12 > size) continue;
            if (cmd_indispensavel(u32(rec+co), u32(rec+co+4))) return fr;
        }
    }
    return anim;
}

/* State bits the dropped prologue clears on its way in.

   Every factory assist has `1_3D State/Invincibility (Disable) [8192]` at f1.
   `1_3D` clears bits of +0x14FC, and bit 0x2000 there is what the stage wall
   clamp (FUN_1400502c0, +0x502C0) checks before doing anything: the assist
   flies in from off screen, so the wall is off until the prologue turns it
   back on.  Cutting the prologue left the wall off for the whole borrowed move
   - measured live, +0x14FC = 0x2000 from the first frame of every borrow and
   0 on Seth's own actions - and Morrigan's move, which travels far, carried
   Seth straight through the corner and snapped him back when it ended. */
static U32 prologue_clears(const U8 *rec, U32 size, U32 po) {
    U32 ncmd, k, out = 0;
    if (po + 16 > size) return 0;
    ncmd = u32(rec + po + 4);
    if (ncmd > 256 || po + 16 + ncmd*8 > size) return 0;
    for (k = 0; k < ncmd; k++) {
        U32 co = po + u32(rec + po + 16 + k*8), np;
        if (co + 24 > size || u32(rec+co) != 1) continue;
        np = u32(rec+co+8);
        if (!np || np > 31 || co + 16 + np*8 > size) continue;
        if (u32(rec + co + 16) == 6) continue;             /* float tag */
        if (u32(rec+co+4) == 0x3d) out |= u32(rec + co + 16 + np*4);
        if (u32(rec+co+4) == 0x3c) out &= ~u32(rec + co + 16 + np*4);
    }
    return out;
}

int steal_trim(const ProbeMemory *m, U64 donor, U8 *buf, U32 cap, U64 base,
               U64 *out_resource, U32 *kept, U32 *frames, U32 *props_dropped,
               U32 *prologue_clear) {
    U64 res, data, rec;
    U8 h[16], e[8];
    U32 n, i, off, my_off = 0, next_off = 0, size, subs, dur, k, w, corte, dropped = 0, cleared = 0;
    U8 *data_out, *rec_out;

    if (cap < STEAL_DATA_AT + STEAL_RECORD_AT + 64) return STEAL_BAD_DATA;

    if (!ptr(m, donor + STEAL_BANK_ANMCHR, &res) || !res)
        if (!ptr(m, donor + STEAL_ACTOR_ANMCHR, &res) || !res) return STEAL_BAD_DATA;
    if (!resource(m, res, STEAL_HASH_ANMCHR, 0, &data)) return STEAL_BAD_DATA;
    if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) return STEAL_BAD_DATA;
    n = u32(h+8);
    if (!n || n > STEAL_MAX_ACTIONS) return STEAL_BAD_DATA;

    /* Find our action's offset and the next record's.  Records are laid out
       one after another, so the gap between them is how much to copy. */
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        if (u32(e) == STEAL_ASSIST_ACTION) my_off = u32(e+4);
    }
    if (!my_off) return STEAL_NO_ACTION;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        off = u32(e+4);
        if (off > my_off && (!next_off || off < next_off)) next_off = off;
    }
    size = next_off ? next_off - my_off : cap - STEAL_DATA_AT - STEAL_RECORD_AT;
    if (size < STEAL_REC_HEADER) return STEAL_BAD_DATA;
    if (size > cap - STEAL_DATA_AT - STEAL_RECORD_AT) size = cap - STEAL_DATA_AT - STEAL_RECORD_AT;

    rec = data + my_off;
    data_out = buf + STEAL_DATA_AT;
    rec_out  = data_out + STEAL_RECORD_AT;

    /* The resource object is a straight copy of the donor's, so its vtable and
       every other field stay valid; only the data pointer becomes ours. */
    if (!rd(m, res, buf, STEAL_OBJ_BYTES)) return STEAL_BAD_DATA;
    put64(buf + 0x70, base + STEAL_DATA_AT);

    /* Synthetic anmchr header: same magic and version, a single action. */
    for (i = 0; i < 16; i++) data_out[i] = h[i];
    put32(data_out + 8, 1);
    put32(data_out + 16, STEAL_BORROW_ACTION);
    put32(data_out + 20, STEAL_RECORD_AT);

    /* The record is copied whole; its internal offsets are relative to itself. */
    if (!rd(m, rec, rec_out, size)) return STEAL_BAD_DATA;
    subs = u32(rec_out);
    dur  = u32(rec_out + 4);
    corte = corte_do_golpe(rec_out, size);
    if (!subs || subs > 1024) return STEAL_BAD_DATA;
    if (dur <= corte || dur > 100000) return STEAL_BAD_DATA;
    if (STEAL_REC_HEADER + subs*8u > size) return STEAL_BAD_DATA;

    /* Compact the (frame, offset) table: keep the sub-blocks from the cut
       onwards, with their frame shifted down.  Whatever falls out becomes dead
       space the engine never reads, because the count at +0 shrinks. */
    for (i = 0, w = 0; i < subs; i++) {
        U8 *pair = rec_out + STEAL_REC_HEADER + i*8;
        U32 fr = u32(pair), po = u32(pair+4), ncmd, c;
        if (fr < corte) {
            U32 v = prologue_clears(rec_out, size, po);
            cleared |= v;
            continue;
        }
        if (po + 16 > size) return STEAL_BAD_DATA;
        /* The frame is stored TWICE - in the pair and at +0 of the sub-block
           itself - and in every factory arc the two agree.  Shifting only the
           table would leave them disagreeing. */
        if (u32(rec_out + po) != fr) return STEAL_BAD_DATA;

        /* Same trick one level down: a sub-block lists its commands through a
           table of 8-byte offsets at +16 (upper half always zero, +8 and +12
           always zero, checked on all 128 characters), and each command's
           bytes stay where they are.  Leaving an entry out of the table is
           enough to make the engine skip that command. */
        ncmd = u32(rec_out + po + 4);
        if (ncmd > 256 || po + 16 + ncmd*8 > size) return STEAL_BAD_DATA;
        for (k = 0, c = 0; k < ncmd; k++) {
            U8 *ent = rec_out + po + 16 + k*8;
            U32 co = po + u32(ent);
            if (co + 8 <= size && cmd_prop_anim(u32(rec_out+co), u32(rec_out+co+4))) {
                dropped++;
                continue;
            }
            if (c != k) {
                U8 *dst = rec_out + po + 16 + c*8, j;
                for (j = 0; j < 8; j++) dst[j] = ent[j];
            }
            shift_frame_jump(rec_out, size, co, corte);
            c++;
        }
        /* A sub-block that only had prop animation in it goes away entirely,
           rather than leaving the engine an empty frame to chew on. */
        if (ncmd && !c) continue;
        put32(rec_out + po + 4, c);

        put32(rec_out + po, fr - corte);
        put32(rec_out + STEAL_REC_HEADER + w*8, fr - corte);
        put32(rec_out + STEAL_REC_HEADER + w*8 + 4, po);
        w++;
    }
    if (!w) return STEAL_NO_ACTION;          /* nothing left after the cut */
    put32(rec_out, w);
    put32(rec_out + 4, dur - corte);

    k = w;
    *out_resource = base;
    if (kept) *kept = k;
    if (frames) *frames = dur - corte;
    if (props_dropped) *props_dropped = dropped;
    if (prologue_clear) *prologue_clear = cleared;
    return STEAL_OK;
}

/* ------------------------------------------------ jumps to the donor's actions */

/* First action id of each class, as the anmchr addresses them (`1_00 [class,
   index]` means base[class] + index). */
static const U32 CLASS_BASE[12] = {
    0x000, 0x01e, 0x032, 0x03c, 0x064, 0x082, 0x096, 0x0aa, 0x0be, 0x0dc, 0x0f0, 0x104
};

U32 steal_gotos(const U8 *rec, U32 size, StealGoto *out, U32 max) {
    U32 subs, i, k, n = 0;
    if (size < STEAL_REC_HEADER) return 0;
    subs = u32(rec);
    if (!subs || subs > 1024 || STEAL_REC_HEADER + subs*8u > size) return 0;
    for (i = 0; i < subs && n < max; i++) {
        U32 fr = u32(rec + STEAL_REC_HEADER + i*8), po = u32(rec + STEAL_REC_HEADER + i*8 + 4), ncmd;
        if (po + 16 > size) continue;
        ncmd = u32(rec + po + 4);
        if (ncmd > 256 || po + 16 + ncmd*8 > size) continue;
        for (k = 0; k < ncmd && n < max; k++) {
            U32 co = po + u32(rec + po + 16 + k*8), cls, idx;
            /* 1_00 with two parameters: types at +16, values at +24 */
            if (co + 32 > size) continue;
            if (u32(rec+co) != 1 || u32(rec+co+4) != 0x00 || u32(rec+co+8) != 2) continue;
            cls = u32(rec + co + 24);
            idx = u32(rec + co + 28);
            if (cls >= 12 || idx > 0xff) continue;
            out[n].frame = fr;
            out[n].action = CLASS_BASE[cls] + idx;
            n++;
        }
    }
    return n;
}

int steal_donor_record(const ProbeMemory *m, U64 anmchr, U64 record,
                       U32 *action, U32 *frames, U32 *size) {
    U64 data;
    U8 h[16], e[8], r[8];
    U32 n, i, off, mine = 0, next = 0, act = 0, found = 0;
    if (!record || !resource(m, anmchr, STEAL_HASH_ANMCHR, 0, &data)) return STEAL_BAD_DATA;
    if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) return STEAL_BAD_DATA;
    n = u32(h+8);
    if (!n || n > STEAL_MAX_ACTIONS) return STEAL_BAD_DATA;
    if (record < data) return STEAL_NO_ACTION;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        off = u32(e+4);
        if (data + off == record) { mine = off; act = u32(e); found = 1; }
    }
    if (!found) return STEAL_NO_ACTION;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        off = u32(e+4);
        if (off > mine && (!next || off < next)) next = off;
    }
    if (!rd(m, record, r, 8)) return STEAL_BAD_DATA;
    *action = act;
    *frames = u32(r+4);
    *size = next ? next - mine : 0x4000u;
    return STEAL_OK;
}

int steal_has_action(const ProbeMemory *m, U64 anmchr, U32 action) {
    U64 data;
    U8 h[16], e[8];
    U32 n, i;
    if (!resource(m, anmchr, STEAL_HASH_ANMCHR, 0, &data)) return STEAL_BAD_DATA;
    if (!rd(m, data, h, 16) || u32(h) != STEAL_MAGIC_ANMCHR) return STEAL_BAD_DATA;
    n = u32(h+8);
    if (!n || n > STEAL_MAX_ACTIONS) return STEAL_BAD_DATA;
    for (i = 0; i < n; i++) {
        if (!rd(m, data+16+(U64)i*8, e, 8)) return STEAL_BAD_DATA;
        if (u32(e) == action) return STEAL_OK;
    }
    return STEAL_NO_ACTION;
}
