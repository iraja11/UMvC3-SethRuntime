/* SethRuntimeProbe v4 - generic assist-alpha steal.

   Seth borrows the donor's action 0x0AC (Assist Alpha), where the donor is
   whoever he hit with his steal (see steal_core.c for how we tell).  The
   assist entrance never runs: we copy the donor's record into a buffer of our
   own, trim the entrance out of its sub-block table, and let the engine enter
   that copy.  Nothing that belongs to the donor is ever modified.

   You won't find a character name anywhere in this code, and that's on
   purpose.  A hardcoded cast list would need a rebuild every time the
   community adds a character, so per-donor exceptions live in an .ini next to
   the ASI instead.

   One use per steal.  With no donor latched, the ASI does nothing at all.

   What we write into a live match: the resource-pointer slots listed in SLOTS,
   the class callback table, mUsrF, the weapon-slot array pointer, the deferred
   action request (+0x1318) and the frame (+0x1328).  Every single one of them
   is put back with the exact value that was there.  No file data is touched. */
#include "win_api.h"
#include "steal_core.h"

#define STEAL_TRIGGER_MOTION  240u  /* what action 0x67 already asks for at f0 */
/* The M+H entry action.  In SethTE's anmchr its frame 0 holds four `66_11`
   branches into the old hardcoded copy-moves, and the `0_21` that fires our
   hook is the LAST command of that frame.  Requesting the action from here
   overrides whichever branch matched, and also covers the case where none did,
   so the steal works for the whole cast without touching the param. */
#define STEAL_ENTRY_ACTION    0x0beu
/* The three actions of the OLD hardcoded copy-moves.  The game still routes
   Seth into them, so entering any of them with a donor latched counts as the
   entry too. */
#define STEAL_ENTRY_OLD1      0x064u  /* used to be Berserker Barrage */
#define STEAL_ENTRY_OLD2      0x0ceu  /* used to be Hadouken */
#define STEAL_ENTRY_OLD3      0x0cfu  /* used to be Charging Star */
#define STEAL_IDLE_ACTION     0u
#define STEAL_ACTOR_REQUEST   0x1318u  /* u32: requested action (deferred request) */
#define STEAL_ACTOR_FRAME     0x1328u  /* f32: current frame */
#define STEAL_ACTOR_PENDING   0x14f0u  /* u32: bit 2 = an action change is pending */
#define STEAL_ACTOR_STATEF    0x14fcu  /* u32: the flags 1_3C sets and 1_3D clears */
#define STEAL_LATCH_PERIOD    32u      /* one roster sweep every N lookups */
#define STEAL_GUARD_PERIOD    16u      /* one donor check every N lookups */
#define STEAL_ENTER_TICKS     600u     /* give up if the action never gets entered */

/* The active resource slots we swap while the borrowed move runs.  The order
   doesn't matter; restoring exactly the same offsets does. */
/* Motion lists are deliberately NOT in this table.  Swapping the animation bank
   leaves Seth holding the wrong LMT the moment he LEAVES the move: a cancel or
   a hit enters one of his own actions, asks for `lmt1:N`, and gets the donor's
   motion N - T-pose.  Animation is redirected per call instead, in
   motion_for_donor(), and only while Seth is still running the borrowed
   record. */
static const U32 SLOTS[] = {
    STEAL_BANK_ANMCHR,   /* the copy the action lookup indexes */
    STEAL_ACTOR_ANMCHR,  /* the other copy, kept consistent */
    /* anmtdown holds the CAPTURE hits, and a grabbed victim runs the capture
       script out of the ATTACKER's anmtdown.  If we didn't swap it, a borrowed
       grab would pin the victim to Seth's anmtdown, which is Vergil's. */
    STEAL_BANK_ANMTDOWN, STEAL_ACTOR_ANMTDOWN,
    STEAL_ACTOR_ATKINFO,   /* the copy the engine reads */
    STEAL_ACTOR_ATKINFO2,  /* the second copy, kept in sync */
    STEAL_ACTOR_SHOTLIST, STEAL_ACTOR_ATKCLI,
};
/* Motion slots are found by scanning instead of hardcoding, so a character
   with an extra bank is still covered.  A motion resource is recognised by its
   type hash at +0x64. */
#define MOTION_SCAN_BASE 0x1190u
#define MOTION_SCAN_SPAN 0x80u
#define MOTION_MAX 16u
#define SLOT_COUNT (sizeof(SLOTS)/sizeof(SLOTS[0]))

enum { ST_IDLE = 0, ST_ENTERING, ST_BORROWED };

typedef struct {
    U32 magic, version;
    volatile U32 enabled, state;
    volatile U32 latched_count, borrows, restores, aborts, logged;
    U64 owner, donor, donor_record;
    U32 frames, last_decision;
    volatile U32 efl_borrows, kept_blocks;
} StealStats;

__declspec(dllexport) StealStats SethStealStats = {
    0x4c414554 /* 'TEAL' */, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* The MSVC ABI wants this symbol in any unit that uses floating point, and the
   DLL links without a standard library to provide it. */
int _fltused = 1;

static ProbeMemory memory;
static HANDLE log_file;

static U64 latched_donor;      /* latched when the steal connects */
static U64 borrow_owner;       /* Seth's actor during the borrow */
static U64 borrow_donor;
static U64 borrow_record;      /* the donor's 0x0AC record */
static U32 borrow_frames;
static U64 saved_slot[SLOT_COUNT];
static U32 enter_ticks;
static U32 latch_tick;
static U32 guard_tick;
static U64 seth_motion[MOTION_MAX], donor_motion[MOTION_MAX];
static U32 motion_n;
/* Anmchr pointers for the two lookup slots.  Seth's own stay in them for the
   whole move, so a cancel finds Seth's actions.  The donor's go in only for
   the instant the borrowed script jumps to one of the donor's own actions
   (see goto_window), and come straight back out once the jump has landed. */
static U64 seth_bank, seth_alt, seth_cmn, donor_bank, donor_alt;
/* The action id the running borrowed record answers to: STEAL_BORROW_ACTION
   for the trimmed copy, the donor's own id once the move follows a jump into
   one of the donor's actions.  See lookup_hook(). */
static U32 borrow_key;
static U32 bank_fake;     /* our one-action bank is still installed */
static U32 window_on;     /* the donor's bank is in the slots right now */
static StealGoto gotos[STEAL_MAX_GOTOS];
static U32 goto_n;
static U32 anim_null_logged, follow_logged;
#define GOTO_LEAD  2.0f   /* frames ahead of a jump to put the donor's bank in */
#define GOTO_GRACE 3.0f   /* frames after it before giving up on the jump */
/* Set once the pending-change bit has been seen CLEAR during the borrow, so
   only a fresh request (a cancel) can end it early.  See tick_state(). */
static U32 pending_armed;
/* +0x14FC bits the dropped prologue would have cleared (see prologue_clears
   in steal_core.c).  Cleared by us the moment the move is entered. */
static U32 prologue_clear;
/* Action trail: while a donor is latched or a move is running, log every
   action change on Seth.  Of all the log lines, this one has solved the most
   bugs in this file. */
static U64 trail_record;
static U32 trail_left = 60;
static U32 spawn_logged;   /* one log entry per move */

static void **name_slot;   /* Seth's vtable +0x68 while hooked */
static int name_install(U64 owner);
static U32 name_logged;
static U8 *trim_buf;            /* buffer holding the trimmed record */
#define TRIM_CAP 0x10000u

/* ------------------------------------------------------------------ reads */

static int read_memory(void *context, U64 address, void *out, U64 n) {
    U64 got = 0;
    (void)context;
    return ReadProcessMemory(GetCurrentProcess(), (void *)address, out, n, &got) && got == n;
}
static int rd64(U64 p, U64 *v) {
    U8 d[8];
    if (!read_memory(0, p, d, 8)) return 0;
    *v = (U64)d[0] | ((U64)d[1]<<8) | ((U64)d[2]<<16) | ((U64)d[3]<<24) |
         ((U64)d[4]<<32) | ((U64)d[5]<<40) | ((U64)d[6]<<48) | ((U64)d[7]<<56);
    return 1;
}
static int rd32(U64 p, U32 *v) {
    U8 d[4];
    if (!read_memory(0, p, d, 4)) return 0;
    *v = (U32)d[0] | ((U32)d[1]<<8) | ((U32)d[2]<<16) | ((U32)d[3]<<24);
    return 1;
}
static float frame_at(U64 actor) {
    U32 bits;
    union { U32 u; float f; } c;
    if (!rd32(actor + STEAL_ACTOR_FRAME, &bits)) return -1.0f;
    c.u = bits;
    return c.f;
}

/* -------------------------------------------------------------------- log */

static U32 length(const char *s) { U32 n = 0; while (s[n]) n++; return n; }

void log_text(const char *s) {
    DWORD done;
    if (log_file && log_file != (HANDLE)(U64)-1) WriteFile(log_file, s, length(s), &done, 0);
}
void log_value(const char *name, U64 value) {
    char line[128];
    U32 i = 0, k;
    while (name[i] && i < 95) { line[i] = name[i]; i++; }
    line[i++] = '='; line[i++] = '0'; line[i++] = 'x';
    for (k = 0; k < 16; k++) line[i++] = "0123456789abcdef"[(value >> ((15-k)*4)) & 15];
    line[i++] = '\r'; line[i++] = '\n'; line[i] = 0;
    log_text(line);
}
/* One line per kind of event, so the log doesn't flood every frame. */
static void log_once(U32 bit, const char *text) {
    if (SethStealStats.logged & (1u << bit)) return;
    SethStealStats.logged |= (1u << bit);
    log_text(text);
}

/* ----------------------------------------------------- swap and restore */

static void write64(U64 p, U64 v) { *(volatile U64 *)p = v; }
static void efl_restore(U64 owner);
static void write32(U64 p, U32 v) { *(volatile U32 *)p = v; }

/* The donor has to stay on the field and keep owning the resources we
   borrowed.  If they left (KO, tag-out), undo everything before any of it can
   be freed. */
static int donor_still_valid(void) {
    U32 lado_seth = 0, lado_doador = 0;
    /* Identifying by RESOURCE would be circular here: the swap has already
       replaced Seth's resources with the donor's, so looking him up by
       SethTE_l1 fails exactly during the borrow.  Being in the roster is the
       one thing the swap doesn't touch. */
    if (steal_actor_present(&memory, borrow_owner, &lado_seth) != STEAL_OK) return 0;
    if (steal_actor_present(&memory, borrow_donor, &lado_doador) != STEAL_OK) return 0;
    return lado_seth != lado_doador;
}

/* Besides the resource pointers, a few other fields matter.

   +0x6738 / +0x6740   the character's class callback table.  Command
       `1_106 Character Class Commands` does:
           count = *(u32*)(chr+0x6738);  tab = *(u64*)(chr+0x6740);
           if (idx < count) ret = ((code*)tab[idx])(chr, arg, f, caller);
       Seth is a Vergil clone, so his table is VERGIL's: Ghost Rider asks for
       callback 0 and Vergil's callback 0 answers.  Borrowing both fields gets
       the right one called.

   +0x1510   mUsrF.  Commands `1_5A`/`1_5B` are nothing more than
       `mUsrF |= v` and `mUsrF &= ~v`; they don't dispatch anything.  The
       trouble is that those bits OUTLIVE the move, and Vergil's own code reads
       them as if they were his (that's what locked Seth's input until a
       reset).  Taking a snapshot and restoring it keeps the bits live during
       the move without losing anything the donor's script wanted. */
#define STEAL_ACTOR_CBCOUNT  0x6738u
#define STEAL_ACTOR_CBTABLE  0x6740u
#define STEAL_ACTOR_USRF     0x1510u

/* Weapon props.  `mWeaponNum` lives at +0x3F38 and the array at +0x3F40.  The
   indexed path of `1_F3`..`1_F8` does no bounds check whatsoever:

       p = *(cChr **)(*(longlong *)(chr + 0x3F40) + idx * 8);
       if (p) ...

   A character with no props has that pointer NULL, so the read lands at
   `idx*8` - a low address, and an instant crash.  That's what a donor with a
   detached prop (Chris, Vergil) used to do to the game.

   The guard takes nothing away from Seth: during the borrow his array is
   COPIED into a 512-entry page of ours with the tail zeroed, and the pointer
   points at the copy.  An index in range still finds the usual prop; one out
   of range reads zero, and the `if (p)` quietly fails instead of chasing
   garbage.  The count is left alone.

   Making the DONOR's prop appear is a different problem, since a prop is an
   object bound to their skeleton rather than a resource looked up by path.
   The prop animation commands are simply left out of the trimmed record (see
   cmd_prop_anim in steal_core.c). */
#define STEAL_ACTOR_WEAPONS  0x3f40u
#define STEAL_ACTOR_WEAPONN  0x3f38u

static U32 saved_cbcount;
static U64 saved_cbtable;
static U32 saved_usrf;
static U32 saved_usrf_ok;
static U64 saved_weapons;
static U32 saved_weaponn;
static U32 saved_weapons_ok;
#define PROP_SLOTS 512u
static void *prop_zeros;    /* copy of Seth's prop array, tail zeroed */

/* Class callbacks that can't run with Seth as `this`.

   A donor callback runs on Seth's object, which has Vergil's layout.  That's
   harmless while the callback sticks to cChr fields, but everything from
   +0x68F8 on belongs to the class: on Seth it holds Vergil's data.  Zero's
   callback 1 (uZero__onActionChange_3730, +0xF3730) reads a pointer table at
   +0x6930..+0x6948 and hands what it finds to uModel__setJointTexture - on Seth
   that's garbage, and the game crashed the moment the borrowed move started.

   Listed here are the callbacks that touch +0x68F8..+0x7FFF themselves or in a
   function they call directly, found by walking every uXxx__setup table in
   Ghidra (86 of 170).  They're engine addresses, not characters: a community
   character reuses its base class, so it's covered too.  A listed callback
   is answered by cb_skip(); everything else in the donor's table still runs,
   which is what brings Ghost Rider's chain and Nova's move along. */
static const U32 CB_UNSAFE[] = {
    0x05fe20u, 0x05fe80u, 0x05fed0u, 0x060040u, 0x060390u, 0x0604b0u,
    0x060520u, 0x060d30u, 0x060d90u, 0x061a80u, 0x06fd60u, 0x070880u,
    0x072710u, 0x075980u, 0x07a930u, 0x07aba0u, 0x081ac0u, 0x083270u,
    0x0838b0u, 0x083b40u, 0x083b60u, 0x0877e0u, 0x087850u, 0x0878a0u,
    0x087950u, 0x0879b0u, 0x090b60u, 0x090b70u, 0x090ba0u, 0x090bb0u,
    0x092e70u, 0x093c10u, 0x093db0u, 0x095c70u, 0x097d40u, 0x09a590u,
    0x0a1260u, 0x0a19a0u, 0x0a1a80u, 0x0a1fb0u, 0x0a1fc0u, 0x0a9b10u,
    0x0a9b70u, 0x0aab20u, 0x0b0f70u, 0x0b13d0u, 0x0b16f0u, 0x0b1b30u,
    0x0b1d50u, 0x0b5100u, 0x0b53e0u, 0x0b5410u, 0x0b5430u, 0x0b5450u,
    0x0b5460u, 0x0b9680u, 0x0b9e90u, 0x0b9ee0u, 0x0bb980u, 0x0bc7e0u,
    0x0bec90u, 0x0bed70u, 0x0c1030u, 0x0c1470u, 0x0c49c0u, 0x0c4fa0u,
    0x0d2920u, 0x0d4660u, 0x0d7be0u, 0x0d8660u, 0x0da970u, 0x0db220u,
    0x0db9f0u, 0x0dbf50u, 0x0ddc30u, 0x0ddc40u, 0x0e2670u, 0x0e2770u,
    0x0e2c70u, 0x0e3020u, 0x0e5f90u, 0x0e8c50u, 0x0e9cc0u, 0x0f0f60u,
    0x0f3730u, 0x0f66f0u,
};
#define CB_MAX 64u
static U64 cb_table[CB_MAX];   /* the donor's table as Seth sees it */

/* Same signature the 1_106 handler calls with; the 0 goes to the command
   value, which is what a callback with nothing to report returns. */
static U64 cb_skip(void *chr, U32 arg, float value, void *caller) {
    (void)chr; (void)arg; (void)value; (void)caller;
    return 0;
}

static int cb_unsafe(U64 fn) {
    U32 i;
    if (fn < memory.exe || fn >= memory.exe + 0x1000000) return 0;
    for (i = 0; i < sizeof(CB_UNSAFE)/sizeof(CB_UNSAFE[0]); i++)
        if (fn - memory.exe == CB_UNSAFE[i]) return 1;
    return 0;
}

static void apply_swap(U64 owner, U64 donor) {
    U32 i;
    U32 n = 0;
    U64 tab = 0;
    saved_cbcount = 0; saved_cbtable = 0; saved_usrf_ok = 0;
    if (rd32(owner + STEAL_ACTOR_CBCOUNT, &saved_cbcount) &&
        rd64(owner + STEAL_ACTOR_CBTABLE, &saved_cbtable) &&
        rd32(donor + STEAL_ACTOR_CBCOUNT, &n) &&
        rd64(donor + STEAL_ACTOR_CBTABLE, &tab) && tab) {
        U32 skipped = 0;
        if (n > CB_MAX) n = CB_MAX;
        for (i = 0; i < n; i++) {
            U64 fn = 0;
            if (!rd64(tab + (U64)i * 8, &fn) || !fn || cb_unsafe(fn)) {
                if (fn) skipped++;
                fn = (U64)&cb_skip;
            }
            cb_table[i] = fn;
        }
        write32(owner + STEAL_ACTOR_CBCOUNT, n);
        write64(owner + STEAL_ACTOR_CBTABLE, (U64)cb_table);
        if (skipped && !(SethStealStats.logged & 0x800u)) {
            SethStealStats.logged |= 0x800u;
            log_text("CLASSE: callbacks do doador que leem a cauda do objeto foram pulados\r\n");
            log_value("  pulados", (U64)skipped);
        }
    } else {
        saved_cbtable = 0;              /* not swapped, so nothing to restore */
    }
    if (rd32(owner + STEAL_ACTOR_USRF, &saved_usrf)) saved_usrf_ok = 1;
    saved_weapons_ok = 0;
    if (prop_zeros && rd64(owner + STEAL_ACTOR_WEAPONS, &saved_weapons) &&
        rd32(owner + STEAL_ACTOR_WEAPONN, &saved_weaponn)) {
        U32 k, quantos = saved_weaponn > PROP_SLOTS ? PROP_SLOTS : saved_weaponn;
        for (k = 0; k < PROP_SLOTS; k++) ((U64 *)prop_zeros)[k] = 0;
        for (k = 0; k < quantos; k++) {
            U64 v = 0;
            if (saved_weapons && rd64(saved_weapons + k * 8, &v)) ((U64 *)prop_zeros)[k] = v;
        }
        saved_weapons_ok = 1;
        write64(owner + STEAL_ACTOR_WEAPONS, (U64)prop_zeros);
    }
    for (i = 0; i < SLOT_COUNT; i++) {
        U64 mine = 0, theirs = 0;
        if (!rd64(owner + SLOTS[i], &mine) || !rd64(donor + SLOTS[i], &theirs)) {
            saved_slot[i] = 0;
            continue;
        }
        saved_slot[i] = mine;
        if (theirs) write64(owner + SLOTS[i], theirs);

    }
}

static void undo_swap(U64 owner) {
    U32 i;
    efl_restore(owner);
    if (saved_cbtable) {
        write32(owner + STEAL_ACTOR_CBCOUNT, saved_cbcount);
        write64(owner + STEAL_ACTOR_CBTABLE, saved_cbtable);
        saved_cbtable = 0;
    }
    /* mUsrF goes back to what Seth had: whatever 1_5A set holds during the
       move and doesn't survive it. */
    if (saved_usrf_ok) { write32(owner + STEAL_ACTOR_USRF, saved_usrf); saved_usrf_ok = 0; }
    if (saved_weapons_ok) {
        write64(owner + STEAL_ACTOR_WEAPONS, saved_weapons);
        saved_weapons_ok = 0;
    }
    if (seth_bank) write64(owner + STEAL_BANK_ANMCHR, seth_bank);
    if (seth_alt)  write64(owner + STEAL_ACTOR_ANMCHR, seth_alt);
    bank_fake = window_on = 0;
    goto_n = 0;
    for (i = 0; i < SLOT_COUNT; i++) {
        if (saved_slot[i]) write64(owner + SLOTS[i], saved_slot[i]);
        saved_slot[i] = 0;
    }
}

static void end_borrow(const char *why, int back_to_idle) {
    U64 cur = 0;
    if (!borrow_owner) return;
    undo_swap(borrow_owner);
    if (back_to_idle) {
        /* Only force idle if the borrowed script is still the one running.  If
           the engine already moved on by itself, leave it be. */
        if (rd64(borrow_owner + STEAL_ACTOR_RECORD, &cur) && cur == borrow_record)
            write32(borrow_owner + STEAL_ACTOR_REQUEST, STEAL_IDLE_ACTION);
    }
    SethStealStats.restores++;
    SethStealStats.state = ST_IDLE;
    log_once(3, "RESTORE: recursos do Seth devolvidos\r\n");
    log_text(why);
    borrow_owner = borrow_donor = borrow_record = 0;
    borrow_frames = 0;
    enter_ticks = 0;
}


/* ------------------------------------------------------ per-donor exceptions

   Not every character fits Seth's skeleton.  For those, instead of borrowing
   the donor's 0x0AC, Seth runs one of his own actions.

   The list is NOT in the code - that would mean a rebuild for every community
   character.  It comes from an .ini next to the ASI, one line per character:

       Amaterasu = 0xCE      -> Seth runs his OWN action 0xCE
       Sentinel = skip       -> don't steal anything from this character

   Anyone not listed falls through to the generic borrow.  The name is the one
   used in resource paths (`chr\<Name>\...`), not the display name. */

#define RULE_MAX      64u
#define RULE_NAME_MAX 24u
#define RULE_SKIP     0xffffffffu

typedef struct { char name[RULE_NAME_MAX]; U32 action; } DonorRule;
static DonorRule rules[RULE_MAX];
static U32 rule_count;

static int same_name(const char *a, const char *b) {
    U32 i;
    for (i = 0; i < RULE_NAME_MAX; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* The character name, taken from `chr\<Name>\motion\<Name>_l1`. */
static int donor_name(U64 actor, char *out) {
    U8 d[0x78];
    U64 res;
    U32 i = 0, k = 0;
    if (!rd64(actor + STEAL_ACTOR_MOTION_L1, &res) || !res) return 0;
    if (!read_memory(0, res, d, sizeof(d))) return 0;
    while (i < 60 && d[12+i] && d[12+i] != '\\') i++;      /* skip "chr" */
    if (d[12+i] != '\\') return 0;
    i++;
    while (i < 60 && d[12+i] && d[12+i] != '\\' && k < RULE_NAME_MAX-1)
        out[k++] = (char)d[12+i++];
    out[k] = 0;
    return k != 0;
}

static U32 parse_hex_or_dec(const char *p) {
    U32 v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            U32 d;
            if (*p >= '0' && *p <= '9') d = (U32)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') d = (U32)(*p - 'a' + 10);
            else if (*p >= 'A' && *p <= 'F') d = (U32)(*p - 'A' + 10);
            else break;
            v = v*16 + d; p++;
        }
        return v;
    }
    while (*p >= '0' && *p <= '9') { v = v*10 + (U32)(*p - '0'); p++; }
    return v;
}

static void rules_load(const char *ini_path) {
    static char text[8192];
    HANDLE f = CreateFileA(ini_path, 0x80000000, 1, 0, 3, 0x80, 0);
    DWORD got = 0;
    U32 i = 0;
    if (!f || f == (HANDLE)(U64)-1) return;
    if (!ReadFile(f, text, sizeof(text)-1, &got, 0)) { CloseHandle(f); return; }
    CloseHandle(f);
    text[got] = 0;
    while (i < got && rule_count < RULE_MAX) {
        char name[RULE_NAME_MAX], val[24];
        U32 n = 0, v = 0;
        while (i < got && (text[i]==' '||text[i]=='\t'||text[i]=='\r'||text[i]=='\n')) i++;
        if (i < got && (text[i]=='#' || text[i]==';')) {          /* comment line */
            while (i < got && text[i] != '\n') i++;
            continue;
        }
        while (i < got && text[i]!='=' && text[i]!='\n' && n < RULE_NAME_MAX-1) {
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') name[n++] = text[i];
            i++;
        }
        name[n] = 0;
        if (i >= got || text[i] != '=') { while (i < got && text[i] != '\n') i++; continue; }
        i++;
        while (i < got && (text[i]==' '||text[i]=='\t')) i++;
        while (i < got && text[i]!='\n' && text[i]!='\r' && v < sizeof(val)-1) val[v++] = text[i++];
        val[v] = 0;
        if (!n) continue;
        for (U32 j = 0; j < RULE_NAME_MAX; j++) rules[rule_count].name[j] = j <= n ? name[j] : 0;
        rules[rule_count].action = (val[0]=='s'||val[0]=='S') ? RULE_SKIP : parse_hex_or_dec(val);
        rule_count++;
    }
    log_text("REGRAS: excecoes por doador carregadas\r\n");
    log_value("quantidade", (U64)rule_count);
}

/* Returns 1 if there's an exception for this actor; *action then holds Seth's
   own action, or RULE_SKIP to steal nothing from them. */
static int rule_for(U64 actor, U32 *action) {
    char nm[RULE_NAME_MAX];
    U32 i;
    if (!rule_count || !donor_name(actor, nm)) return 0;
    for (i = 0; i < rule_count; i++)
        if (same_name(rules[i].name, nm)) { *action = rules[i].action; return 1; }
    return 0;
}


/* --------------------------------------------------------- donor EFLs by id

   An actor's `eftpathlist` and its resource array (+0x2FF0) run in parallel,
   by position, and every character skips different ids - so the same POSITION
   means a different id from one character to the next.  Swapping the list
   itself only made things worse: the list then described the donor's positions
   while the array still held Seth's resources.

   So the list stays Seth's and only the ARRAY CONTENT gets swapped, matched by
   id: wherever Seth has efl 0010, the donor's 0010 goes in.  Ids the donor
   doesn't have are left alone. */

#define EFL_ARRAY_INI 0x2ff0u
#define EFL_ARRAY_MAX 96u
#define EFL_HASH      0x6d5ae854u

static U64 saved_efl[EFL_ARRAY_MAX];
static U32 saved_efl_slot[EFL_ARRAY_MAX];
static U32 saved_efl_n;

/* Returns the NNNN id of a `chr\<x>\effect\efl\NNNN` resource, or -1. */
static int efl_id_of(U64 res) {
    U8 d[0x78];
    U32 i = 0, fim = 0, v = 0, k;
    if (!res || !read_memory(0, res, d, sizeof(d))) return -1;
    if (((U32)d[0x64] | ((U32)d[0x65]<<8) | ((U32)d[0x66]<<16) | ((U32)d[0x67]<<24)) != EFL_HASH)
        return -1;
    while (i < 63 && d[12+i]) i++;
    fim = i;
    if (fim < 4) return -1;
    for (k = fim - 4; k < fim; k++) {
        U8 c = d[12+k];
        if (c < '0' || c > '9') return -1;
        v = v*10 + (U32)(c - '0');
    }
    return (int)v;
}

static void efl_swap_by_id(U64 owner, U64 donor) {
    U32 i, j;
    saved_efl_n = 0;
    for (i = 0; i < EFL_ARRAY_MAX; i++) {
        U64 meu = 0;
        int id;
        if (!rd64(owner + EFL_ARRAY_INI + i*8, &meu)) continue;
        id = efl_id_of(meu);
        if (id < 0) continue;
        for (j = 0; j < EFL_ARRAY_MAX; j++) {
            U64 dele = 0;
            if (!rd64(donor + EFL_ARRAY_INI + j*8, &dele)) continue;
            if (efl_id_of(dele) != id) continue;
            if (saved_efl_n >= EFL_ARRAY_MAX) break;
            saved_efl_slot[saved_efl_n] = i;
            saved_efl[saved_efl_n] = meu;
            saved_efl_n++;
            write64(owner + EFL_ARRAY_INI + i*8, dele);
            break;
        }
    }
    if (saved_efl_n && !(SethStealStats.logged & 0x200u)) {
        SethStealStats.logged |= 0x200u;
        log_text("EFL: recursos do array trocados por id (projetil incluso)\r\n");
        log_value("trocados", (U64)saved_efl_n);
    }
}

static void efl_restore(U64 owner) {
    U32 i;
    for (i = 0; i < saved_efl_n; i++)
        write64(owner + EFL_ARRAY_INI + saved_efl_slot[i]*8, saved_efl[i]);
    saved_efl_n = 0;
}

/* --------------------------------------------------------- state machine */

static void try_latch(void) {
    U64 owner = 0, victim = 0;
    U32 side = 0;
    if (steal_find_owner(&memory, &owner, &side) != STEAL_OK) return;
    if (steal_find_victim(&memory, owner, side, &victim) != STEAL_OK) return;
    if (victim == latched_donor) return;
    latched_donor = victim;
    SethStealStats.latched_count++;
    if (!(SethStealStats.logged & 1u)) {
        SethStealStats.logged |= 1u;
        log_text("LATCH: doador travado pelo steal\r\n");
        log_value("donor", victim);
    }
}

/* Is this resource a motion list?  The type hash lives at +0x64. */
static int eh_motion(U64 res) {
    U8 d[0x68];
    if (!res || !read_memory(0, res, d, sizeof(d))) return 0;
    return (((U32)d[0x64]) | ((U32)d[0x65]<<8) | ((U32)d[0x66]<<16) | ((U32)d[0x67]<<24))
           == STEAL_HASH_MOTION;
}

/* ---------------------------------------------------------- diagnostics */

static void log_resource_name(const char *label, U64 res) {
    U8 d[0x78];
    char s[65];
    U32 i;
    log_text(label);
    if (res && read_memory(0, res, d, sizeof(d))) {
        for (i = 0; i < 64 && d[12+i]; i++) s[i] = (char)d[12+i];
        s[i] = 0;
        log_text(s);
    } else {
        log_text("(ilegivel)");
    }
    log_text("\r\n");
}

static int is_owner_path(U64 res) {
    static const char pre[] = "chr\\SethTE\\";
    U8 d[0x20];
    U32 i;
    if (!res || !read_memory(0, res, d, sizeof(d))) return 0;
    for (i = 0; i + 1 < sizeof(pre); i++) if (d[12+i] != (U8)pre[i]) return 0;
    return 1;
}

/* From in here, a T-pose looks like an animation request that comes back
   empty while a move is borrowed.  Logging the list it was asked from, the
   list it was actually read from and the index is enough to tell a missed
   redirect apart from a motion the donor genuinely doesn't have. */
static void log_empty_anim(void *asked, void *got, U32 motion) {
    U32 i, ours = 0;
    if (SethStealStats.state == ST_IDLE || anim_null_logged >= 8) return;
    for (i = 0; i < motion_n; i++)
        if ((U64)asked == seth_motion[i] || (U64)asked == donor_motion[i]) ours = 1;
    if (!ours && !is_owner_path((U64)asked)) return;
    anim_null_logged++;
    log_text("ANIM VAZIA: pedido de animacao voltou sem motion (T-pose)\r\n");
    log_resource_name("  pedido na lista: ", (U64)asked);
    log_resource_name("  lido da lista:   ", (U64)got);
    log_value("  motion", (U64)motion);
}

/* ------------------------------------------- jumps into the donor's actions */

static void bank_seth(void) {
    if (seth_bank) write64(borrow_owner + STEAL_BANK_ANMCHR, seth_bank);
    if (seth_alt)  write64(borrow_owner + STEAL_ACTOR_ANMCHR, seth_alt);
    window_on = 0;
}

static void bank_donor(void) {
    if (!donor_bank) return;
    write64(borrow_owner + STEAL_BANK_ANMCHR, donor_bank);
    write64(borrow_owner + STEAL_ACTOR_ANMCHR, donor_alt ? donor_alt : donor_bank);
    window_on = 1;
}

/* Keeps only the jumps worth opening the window for.  A jump back to idle (0)
   should land on SETH's idle, which is what his own bank gives, and a target
   the donor doesn't have would fail no matter which bank is in. */
static void load_gotos(const U8 *rec, U32 size) {
    StealGoto all[STEAL_MAX_GOTOS];
    U32 n = steal_gotos(rec, size, all, STEAL_MAX_GOTOS), i;
    goto_n = 0;
    for (i = 0; i < n; i++) {
        if (!all[i].action || !donor_bank) continue;
        if (steal_has_action(&memory, donor_bank, all[i].action) != STEAL_OK) continue;
        gotos[goto_n++] = all[i];
    }
}

static U8 follow_buf[0x4000];

/* Same, for a follow-up record that lives in the donor's own memory. */
static void load_gotos_at(U64 record, U32 size) {
    if (size > sizeof(follow_buf)) size = sizeof(follow_buf);
    goto_n = 0;
    if (size < STEAL_REC_HEADER || !read_memory(0, record, follow_buf, size)) return;
    load_gotos(follow_buf, size);
}

/* Opens the window a couple of frames before a jump and closes it once the
   jump is well past.  Outside the window Seth's bank is in, so a cancel still
   finds his hyper; inside it, the `1_00` finds the donor's follow-up. */
static void goto_window(float f) {
    U32 i, hit = goto_n;
    for (i = 0; i < goto_n; i++)
        if (f + GOTO_LEAD >= (float)gotos[i].frame && f <= (float)gotos[i].frame + GOTO_GRACE) {
            hit = i;
            break;
        }
    if (hit < goto_n && !window_on) {
        bank_donor();
        if (follow_logged < 12) {
            follow_logged++;
            log_text("SALTO: banco do doador na janela do 1_00 do golpe\r\n");
            log_value("  para a acao", (U64)gotos[hit].action);
        }
    } else if (hit == goto_n && window_on) {
        bank_seth();
    }
}

/* The engine left the borrowed record.  If it went into another of the
   donor's actions while the window was open, that's the move continuing, not
   ending: the borrow follows it, with Seth's bank back in for cancels. */
static int follow_up(U64 cur) {
    U32 act = 0, dur = 0, size = 0;
    if (!window_on || !donor_bank || !cur) return 0;
    if (steal_donor_record(&memory, donor_bank, cur, &act, &dur, &size) != STEAL_OK) return 0;
    borrow_record = cur;
    borrow_frames = dur;
    borrow_key = act;
    bank_seth();
    load_gotos_at(cur, size);
    pending_armed = 0;
    SethStealStats.state = ST_BORROWED;
    SethStealStats.donor_record = cur;
    if (follow_logged < 12) {
        follow_logged++;
        log_text("CONTINUACAO: o golpe seguiu para outra acao do doador\r\n");
        log_value("  acao", (U64)act);
        log_value("  frames", (U64)dur);
    }
    return 1;
}

static void begin_borrow(void) {
    StealResult r;
    U32 flags = 0;
    /* This uses the donor LATCHED at steal time.  There's no victim search
       here: by the time the move comes out, the capture is long over. */
    int d = steal_prepare(&memory, latched_donor, &r);
    SethStealStats.last_decision = (U32)d;
    if (d != STEAL_OK) {
        /* The latched donor can't be used any more (left the field, or has no 0x0AC). */
        latched_donor = 0;
        SethStealStats.aborts++;
        if (!(SethStealStats.logged & 2u)) {
            SethStealStats.logged |= 2u;
            log_text("SEM EMPRESTIMO: doador travado nao pode mais ser usado\r\n");
            log_value("decision", (U64)d);
        }
        return;
    }
    {   /* There's an exception set up for this donor: nothing gets borrowed,
           and Seth runs one of his own actions instead. */
        U32 own = 0;
        if (rule_for(r.donor, &own)) {
            latched_donor = 0;                      /* the steal is used up either way */
            if (own != RULE_SKIP) {
                U32 fl = 0;
                if (rd32(r.owner + STEAL_ACTOR_PENDING, &fl))
                    write32(r.owner + STEAL_ACTOR_PENDING, fl | 2u);
                write32(r.owner + STEAL_ACTOR_REQUEST, own);
            }
            if (!(SethStealStats.logged & 0x80u)) {
                SethStealStats.logged |= 0x80u;
                log_text("REGRA: doador na lista de excecoes; acao propria do Seth\r\n");
                log_value("acao", (U64)own);
            }
            return;
        }
    }
    borrow_owner = r.owner;
    borrow_donor = r.donor;
    borrow_record = r.donor_record;
    borrow_frames = r.frames;
    enter_ticks = 0;

    /* Build the donor's 0x0AC without the assist entrance, in a buffer of our
       own, and install a copy of the resource object that points at it.  The
       engine then walks straight into the move: its first frame becomes frame
       0, and the entrance commands are never emitted. */
    {
        U64 fake = 0;
        U32 kept = 0, trimmed = 0, props = 0;
        int t;
        prologue_clear = 0;
        t = trim_buf ? steal_trim(&memory, borrow_donor, trim_buf, TRIM_CAP,
                                  (U64)trim_buf, &fake, &kept, &trimmed, &props,
                                  &prologue_clear)
                     : STEAL_BAD_DATA;
        if (t != STEAL_OK || !fake) {
            latched_donor = 0;
            SethStealStats.aborts++;
            borrow_owner = borrow_donor = borrow_record = 0;
            if (!(SethStealStats.logged & 0x40u)) {
                SethStealStats.logged |= 0x40u;
                log_text("SEM EMPRESTIMO: nao consegui aparar o prologo\r\n");
                log_value("decision", (U64)t);
            }
            return;
        }
        if (props && !(SethStealStats.logged & 0x400u)) {
            SethStealStats.logged |= 0x400u;
            log_text("PROP: animacao de prop do doador ignorada (evita T-pose)\r\n");
            log_value("  comandos 1_F7/1_F8 removidos", (U64)props);
        }
        motion_n = 0;
        for (U32 o = MOTION_SCAN_BASE;
             o < MOTION_SCAN_BASE + MOTION_SCAN_SPAN && motion_n < MOTION_MAX; o += 8) {
            U64 a = 0, b = 0;
            if (!rd64(borrow_owner + o, &a) || !eh_motion(a)) continue;
            if (!rd64(borrow_donor + o, &b) || !eh_motion(b)) continue;
            seth_motion[motion_n] = a;
            donor_motion[motion_n] = b;
            motion_n++;
        }
        /* Remember SETH's anmchr pointers BEFORE the swap.  This used to be
           read after apply_swap(), which had already put the DONOR's anmchr
           in both slots - so the "give Seth his bank back" step in
           tick_state() handed him the donor's instead, and it stayed there for
           the whole move.  Any cancel then looked Seth's own action id up in
           the donor's anmchr: a hyper cancel came out as the donor's special,
           the donor's hyper, or nothing at all. */
        if (!rd64(borrow_owner + STEAL_BANK_ANMCHR, &seth_bank)) seth_bank = 0;
        if (!rd64(borrow_owner + STEAL_ACTOR_ANMCHR, &seth_alt)) seth_alt = 0;
        if (!rd64(borrow_owner + STEAL_ACTOR_ANMCMN, &seth_cmn)) seth_cmn = 0;
        borrow_key = STEAL_BORROW_ACTION;
        /* ...and the donor's, for the moments the borrowed script jumps into
           one of the donor's own actions. */
        if (!rd64(borrow_donor + STEAL_BANK_ANMCHR, &donor_bank)) donor_bank = 0;
        if (!rd64(borrow_donor + STEAL_ACTOR_ANMCHR, &donor_alt)) donor_alt = 0;
        apply_swap(borrow_owner, borrow_donor);
        efl_swap_by_id(borrow_owner, borrow_donor);
        /* Both anmchr slots point at OUR copy rather than the donor's, since
           ours is the one holding the trimmed action. */
        write64(borrow_owner + STEAL_BANK_ANMCHR, fake);
        write64(borrow_owner + STEAL_ACTOR_ANMCHR, fake);
        bank_fake = 1;
        window_on = 0;
        borrow_record = (U64)trim_buf + STEAL_DATA_AT + STEAL_RECORD_AT;
        borrow_frames = trimmed;
        SethStealStats.kept_blocks = kept;
        load_gotos((const U8 *)borrow_record,
                   TRIM_CAP - STEAL_DATA_AT - STEAL_RECORD_AT);
    }
    /* Deferred request: the engine enters the action during its own update.
       This mirrors what the native GoToAction does, bit 2 at +0x14f0 included. */
    if (rd32(borrow_owner + STEAL_ACTOR_PENDING, &flags))
        write32(borrow_owner + STEAL_ACTOR_PENDING, flags | 2u);
    write32(borrow_owner + STEAL_ACTOR_REQUEST, STEAL_BORROW_ACTION);
    /* The frame is left alone: the trimmed record already starts at the move. */

    latched_donor = 0;              /* one use per steal */
    spawn_logged = 0;
    name_logged = 0;
    pending_armed = 0;
    /* Diagnostics start fresh for every steal, so each test gets its own
       trail instead of the first one using it all up. */
    trail_left = 30;
    anim_null_logged = 0;
    follow_logged = 0;
    SethStealStats.state = ST_ENTERING;
    SethStealStats.borrows++;
    SethStealStats.owner = borrow_owner;
    SethStealStats.donor = borrow_donor;
    SethStealStats.donor_record = borrow_record;
    SethStealStats.frames = borrow_frames;
    log_once(2, "BORROW: assist alpha do doador pedido no frame 6\r\n");
    log_value("owner", borrow_owner);
    log_value("donor", borrow_donor);
    log_value("record", borrow_record);
    log_value("frames", (U64)borrow_frames);
    {
        char nm[RULE_NAME_MAX];
        U32 i;
        if (donor_name(borrow_donor, nm)) { log_text("  doador: "); log_text(nm); log_text("\r\n"); }
        log_value("  saltos 1_00 para acoes do doador", (U64)goto_n);
        /* Which animation list got paired with which.  A wrong pairing is one
           of the two ways a borrowed move ends up in a T-pose. */
        if (SethStealStats.borrows <= 6) {
            for (i = 0; i < motion_n; i++) {
                log_resource_name("  lista do Seth:   ", seth_motion[i]);
                log_resource_name("  lista do doador: ", donor_motion[i]);
            }
        }
    }
}

static void tick_state(void) {
    U64 cur = 0;
    float f;
    if (SethStealStats.state == ST_IDLE || !borrow_owner) return;
    /* The donor check walks the whole roster, and the resolver gets called
       many times per frame, so it can't run on every lookup.  Once every
       STEAL_GUARD_PERIOD is plenty. */
    if (++guard_tick >= STEAL_GUARD_PERIOD) {
        guard_tick = 0;
        if (!donor_still_valid()) { end_borrow("FIM: doador saiu de campo\r\n", 1); return; }
    }
    if (!rd64(borrow_owner + STEAL_ACTOR_RECORD, &cur)) {
        end_borrow("FIM: estado do script ilegivel\r\n", 0);
        return;
    }
    if (SethStealStats.state == ST_ENTERING) {
        /* The synthetic bank holds a single action.  While it's installed, any
           other action lookup on Seth fails - that's what once froze him in
           the middle of a cancel.  As soon as the engine consumes our request
           (+0x1318 stops being ours), or we see it running the record, the
           record is cached and the slots can go back to a real bank.  Which
           one: Seth's, unless the move jumps to a donor action right at its
           first frames (KTho and Neroe do, on the very first frame) - then the
           donor's, so that jump lands. */
        U32 pend = 0;
        int consumed = rd32(borrow_owner + STEAL_ACTOR_REQUEST, &pend) &&
                       pend != STEAL_BORROW_ACTION;
        if (bank_fake && (consumed || cur == borrow_record)) {
            bank_fake = 0;
            if (goto_n && (float)gotos[0].frame <= GOTO_LEAD) bank_donor();
            else bank_seth();
            log_once(5, "BANCO: anmchr do Seth devolvido ao consumir o pedido\r\n");
        }
        if (cur == borrow_record) {
            U32 st = 0;
            SethStealStats.state = ST_BORROWED;
            /* What the prologue's `1_3D` would have done on its way in.  Without
               it the stage wall stays off for the whole move (bit 0x2000). */
            if (prologue_clear && rd32(borrow_owner + STEAL_ACTOR_STATEF, &st) && (st & prologue_clear)) {
                write32(borrow_owner + STEAL_ACTOR_STATEF, st & ~prologue_clear);
                if (!(SethStealStats.logged & 0x1000u)) {
                    SethStealStats.logged |= 0x1000u;
                    log_text("PAREDE: bits do prologo cortado desligados (+0x14FC)\r\n");
                    log_value("  bits", (U64)(st & prologue_clear));
                }
            }
            log_once(4, "ENTROU: script do doador, sem prologo; anmchr devolvido\r\n");
            return;
        }
        /* A jump on the first frame can take the engine straight past our
           record into the follow-up before we ever see the record itself. */
        if (!bank_fake && follow_up(cur)) return;
        if (++enter_ticks > STEAL_ENTER_TICKS)
            end_borrow("FIM: a acao pedida nunca entrou\r\n", 1);
        return;
    }
    /* ST_BORROWED */
    if (cur != borrow_record) {
        if (follow_up(cur)) return;
        end_borrow("FIM: o script saiu da acao emprestada\r\n", 0);
        return;
    }
    /* Leaving early for a cancel.  Waiting for the record to change means the
       new action (a hyper, say) can already run its first commands while Seth
       still holds the donor's atkinfo, shot list and callback table.  The
       engine flags an action change at +0x14F0 bit 2 before it enters the new
       action, so the moment we see that flag come on we give Seth everything
       back.  We only react to the bit going from clear to set: if it happened
       to be set when the move began, we wait until we've seen it clear, so a
       leftover flag can never cut a move short. */
    /* While the jump window is open, the pending change is almost certainly
       the script's own `1_00`, not a cancel, so it's left to follow_up(). */
    if (!window_on) {
        U32 pend = 0;
        if (rd32(borrow_owner + STEAL_ACTOR_PENDING, &pend)) {
            if (!(pend & 2u)) pending_armed = 1;
            else if (pending_armed) {
                end_borrow("FIM: troca de acao pedida (cancel); recursos devolvidos antes da entrada\r\n", 0);
                return;
            }
        }
    }
    f = frame_at(borrow_owner);
    goto_window(f);
    /* A jump often sits on the very last frame (Wolverine's is at frame 35 of
       35), so with the window open the move gets a few frames of grace to
       actually make the jump before we call it over. */
    if (f >= (float)borrow_frames + (window_on ? GOTO_GRACE : 0.0f))
        end_borrow("FIM: fim da acao emprestada\r\n", 1);
}


/* ----------------------------------------------------------------- EFL

   Effects don't resolve through a slot on the actor: the lookup builds
   `chr\<Name>\effect\efl\NNNN` from whichever ACTOR is passed in as the
   source.  Swapping a resource pointer does nothing here; passing the donor
   as the source is what changes the answer, while keeping the bank and index
   the script asked for. */

typedef void *(*NativeEFL)(void *, U32, U32, void *, U32);
#define EFL_NATIVE_RVA 0x1306e0u

typedef struct { U32 rva; U8 original[5]; } EflSite;
/* Each one is a `call rel32` to +0x1306E0, checked byte by byte before we
   write anything. */
static const EflSite efl_sites[] = {
    {0x13133e, {0xe8,0x9d,0xf3,0xff,0xff}},
    {0x13143e, {0xe8,0x9d,0xf2,0xff,0xff}},
    {0x13155e, {0xe8,0x7d,0xf1,0xff,0xff}},
    {0x13168e, {0xe8,0x4d,0xf0,0xff,0xff}},
    {0x13176a, {0xe8,0x71,0xef,0xff,0xff}},
    {0x131dce, {0xe8,0x0d,0xe9,0xff,0xff}},
};
#define EFL_SITE_COUNT (sizeof(efl_sites)/sizeof(efl_sites[0]))

static DWORD efl_protection[EFL_SITE_COUNT];
static U32 efl_installed;

static void *efl_hook(void *manager, U32 bank, U32 index, void *source, U32 flags) {
    NativeEFL native = (NativeEFL)(memory.exe + EFL_NATIVE_RVA);
    /* The window is narrow on purpose.  An earlier version kept redirecting
       for 900 ticks after the move, hoping to catch the projectile in flight:
       all it caught was dust, and it swapped the effect of Seth's own hyper
       for the donor's.  Now it only holds while the borrowed script is
       actually running, and only for requests coming from Seth himself.
       Bank 300 is the common jump/land dust and keeps its original source. */
    U64 cur = 0;
    int janela = SethStealStats.state == ST_BORROWED && borrow_owner && borrow_donor &&
                 borrow_record &&
                 rd64(borrow_owner + STEAL_ACTOR_RECORD, &cur) && cur == borrow_record;
    if (janela && (U64)source == borrow_owner && bank != 300u) {
        U64 doador = borrow_donor;
        void *r = native(manager, bank, index, (void *)doador, flags);
        SethStealStats.efl_borrows++;
        if (!(SethStealStats.logged & 0x20u)) {
            SethStealStats.logged |= 0x20u;
            log_text("EFL: efeitos do script emprestado vindo do doador\r\n");
        }
        if (r) return r;
        return native(manager, bank, index, source, flags);
    }
    return native(manager, bank, index, source, flags);
}

static void efl_rollback(void) {
    DWORD ignored;
    while (efl_installed) {
        U32 i = --efl_installed;
        U8 *site = (U8 *)(memory.exe + efl_sites[i].rva);
        DWORD old;
        if (!VirtualProtect(site, 5, 0x40, &old)) continue;
        for (U32 j = 0; j < 5; j++) site[j] = efl_sites[i].original[j];
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        VirtualProtect(site, 5, efl_protection[i], &ignored);
    }
}

static int efl_install(void) {
    U8 b[5], *relay = 0;
    DWORD old, ignored;
    U32 i, j;
    for (i = 0; i < EFL_SITE_COUNT; i++) {
        if (!read_memory(0, memory.exe + efl_sites[i].rva, b, 5)) return 0;
        for (j = 0; j < 5; j++) if (b[j] != efl_sites[i].original[j]) return 0;
    }
    for (U64 off = 0x2000000; off < 0x10000000; off += 0x10000) {
        relay = VirtualAlloc((void *)(memory.exe + off), 0x1000, 0x3000, 4);
        if (relay) break;
    }
    if (!relay) return 0;
    relay[0] = 0xff; relay[1] = 0x25;
    for (j = 2; j < 6; j++) relay[j] = 0;
    for (j = 0; j < 8; j++) relay[6+j] = (U8)((U64)&efl_hook >> (8*j));
    if (!VirtualProtect(relay, 0x1000, 0x20, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, 16)) return 0;
    for (i = 0; i < EFL_SITE_COUNT; i++) {
        U8 *site = (U8 *)(memory.exe + efl_sites[i].rva);
        long long delta = (long long)(U64)relay - (long long)(U64)(site + 5);
        if (delta < -2147483647LL || delta > 2147483647LL ||
            !VirtualProtect(site, 5, 0x40, &efl_protection[i])) { efl_rollback(); return 0; }
        efl_installed = i + 1;
        for (j = 0; j < 4; j++) site[j+1] = (U8)((U32)delta >> (8*j));
        if (!FlushInstructionCache(GetCurrentProcess(), site, 5) ||
            !VirtualProtect(site, 5, efl_protection[i], &ignored)) { efl_rollback(); return 0; }
    }
    return 1;
}

static void trail(void) {
    U64 cur = 0;
    U32 act = 0, bank = 0;
    if (!trail_left) return;
    if (!latched_donor && SethStealStats.state == ST_IDLE) return;
    if (!borrow_owner) {
        U64 owner = 0; U32 side = 0;
        if (steal_find_owner(&memory, &owner, &side) != STEAL_OK) return;
        if (!rd64(owner + STEAL_ACTOR_RECORD, &cur)) return;
        if (cur == trail_record) return;
        trail_record = cur;
        if (steal_action_of(&memory, owner, cur, &act, &bank) == STEAL_OK) {
            log_text("ACAO: o Seth entrou em\r\n");
            log_value("  id", (U64)act);
            log_value("  banco", (U64)bank);   /* 0=anmcmn 1=anmchr 2=anmtdown */
        } else {
            log_text("ACAO: registro fora dos bancos do Seth (emprestado?)\r\n");
            log_value("  record", cur);
        }
        trail_left--;
        return;
    }
    if (!rd64(borrow_owner + STEAL_ACTOR_RECORD, &cur) || cur == trail_record) return;
    trail_record = cur;
    if (steal_action_of(&memory, borrow_owner, cur, &act, &bank) == STEAL_OK) {
        log_text("ACAO: o Seth entrou em\r\n");
        log_value("  id", (U64)act);
        log_value("  banco", (U64)bank);
    } else {
        log_text("ACAO: registro fora dos bancos do Seth (emprestado?)\r\n");
        log_value("  record", cur);
    }
    trail_left--;
}


typedef void *(*NativeFind)(void *, void *, const char *, U32);
#define MANAGER_RVA 0xe175a8u
#define FIND_VSLOT  0x60u

static void **find_slot;      /* where the original pointer lives */
static NativeFind find_orig;

static void *find_hook(void *mgr, void *tipo, const char *nome, U32 flags) {
    char troca[192];
    char donor[RULE_NAME_MAX], dono[RULE_NAME_MAX];
    U32 i = 0, k = 0, n = 0;
    if (SethStealStats.state != ST_BORROWED || !borrow_owner || !borrow_donor || !nome)
        return find_orig(mgr, tipo, nome, flags);
    if (!(nome[0]=='c' && nome[1]=='h' && nome[2]=='r' && nome[3]=='\\'))
        return find_orig(mgr, tipo, nome, flags);
    if (!donor_name(borrow_owner, dono) || !donor_name(borrow_donor, donor))
        return find_orig(mgr, tipo, nome, flags);
    /* only paths that belong to Seth are touched */
    while (dono[i] && nome[4+i] == dono[i]) i++;
    if (dono[i] || nome[4+i] != '\\') return find_orig(mgr, tipo, nome, flags);

    troca[k++]='c'; troca[k++]='h'; troca[k++]='r'; troca[k++]='\\';
    while (donor[n] && k < sizeof(troca)-1) troca[k++] = donor[n++];
    n = 4 + i;                                  /* the rest of the path, from the backslash on */
    while (nome[n] && k < sizeof(troca)-1) troca[k++] = nome[n++];
    troca[k] = 0;
    {
        void *r = find_orig(mgr, tipo, troca, flags);
        if (r) return r;
    }
    return find_orig(mgr, tipo, nome, flags);   /* the donor doesn't have it: leave it as it was */
}

static int find_install(void) {
    U64 mgr = 0, vt = 0, alvo = 0;
    DWORD old, ignored;
    if (!rd64(memory.exe + MANAGER_RVA, &mgr) || !mgr) return 0;
    if (!rd64(mgr, &vt) || !vt) return 0;
    if (!rd64(vt + FIND_VSLOT, &alvo) || alvo < memory.exe || alvo > memory.exe + 0x1000000)
        return 0;
    find_orig = (NativeFind)alvo;
    find_slot = (void **)(vt + FIND_VSLOT);
    if (!VirtualProtect(find_slot, 8, 0x40, &old)) return 0;
    *find_slot = (void *)&find_hook;
    VirtualProtect(find_slot, 8, old, &ignored);
    return 1;
}

/* The resource manager is a singleton that doesn't exist yet when the ASI
   loads, so we keep retrying until it shows up. */
static U32 find_tick;

static void find_try(void) {
    /* The two hooks become possible at different moments: the resource
       manager exists at the menu, but Seth's vtable only once he's on the
       field. */
    if (find_slot && name_slot) return;
    if (++find_tick < 64u) return;
    find_tick = 0;
    if (!find_slot && find_install())
        log_text("NOME READY: busca de recurso por nome interceptada\r\n");
    if (!name_slot) {
        U64 dono = 0;
        U32 lado = 0;
        if (steal_find_owner(&memory, &dono, &lado) == STEAL_OK) name_install(dono);
    }
}


/* ------------------------------------------- answering with the donor's name

   The path composer (+0x130750) builds `chr\%s\effect\%s\%04d`, and the first
   %s comes from the actor's virtual +0x68 - its name.  So if Seth answers with
   the donor's name while the borrowed move runs, every composed path points at
   the donor, including ones resolved in places we never tracked down.  The
   name we return is the donor's own, fetched through the donor's virtual. */

typedef const char *(*NativeName)(void *);
#define NAME_VSLOT 0x68u

static NativeName name_orig;

static const char *name_hook(void *self) {
    if (SethStealStats.state == ST_BORROWED && borrow_owner && borrow_donor &&
        (U64)self == borrow_owner) {
        U64 vt = 0, fn = 0;
        if (rd64(borrow_donor, &vt) && vt && rd64(vt + NAME_VSLOT, &fn) && fn) {
            const char *nm = ((NativeName)fn)((void *)borrow_donor);
            if (nm && nm[0]) {
                if (!(name_logged & 1u)) {
                    name_logged |= 1u;
                    log_text("NOME DO DONO: o Seth responde como o doador\r\n");
                    log_text("  nome: "); log_text(nm); log_text("\r\n");
                }
                return nm;
            }
        }
    }
    return name_orig(self);
}

static U32 name_falhou;

static int name_install(U64 owner) {
    U64 vt = 0, alvo = 0;
    DWORD old, ignored;
    if (name_slot || !owner) return 0;
    if (!rd64(owner, &vt) || !vt) {
        if (!name_falhou) { name_falhou = 1; log_text("NOME DO DONO: vtable do ator ilegivel\r\n"); }
        return 0;
    }
    if (!rd64(vt + NAME_VSLOT, &alvo)) {
        if (!name_falhou) { name_falhou = 1; log_text("NOME DO DONO: slot +0x68 ilegivel\r\n"); log_value("vtable", vt); }
        return 0;
    }
    /* Do NOT require the target to live inside the executable.  Seth's vtable
       is a copy the Clone Engine keeps in its own memory; every slot points
       into the exe except +0x68, the GetName it replaced so the clone can
       answer "SethTE".  Requiring the exe range rejected exactly the case we
       wanted.  Being readable is enough. */
    {
        U64 sonda = 0;
        if (!alvo || !rd64(alvo, &sonda)) {
            if (!name_falhou) {
                name_falhou = 1;
                log_text("NOME DO DONO: o virtual +0x68 nao e legivel\r\n");
                log_value("ator", owner); log_value("vtable", vt); log_value("alvo", alvo);
            }
            return 0;
        }
    }
    name_orig = (NativeName)alvo;
    name_slot = (void **)(vt + NAME_VSLOT);
    if (!VirtualProtect(name_slot, 8, 0x40, &old)) {
        name_slot = 0;
        if (!name_falhou) { name_falhou = 1; log_text("NOME DO DONO: vtable somente leitura\r\n"); }
        return 0;
    }
    *name_slot = (void *)&name_hook;
    VirtualProtect(name_slot, 8, old, &ignored);
    log_text("NOME DO DONO READY: o Seth pode responder como o doador\r\n");
    return 1;
}


/* ------------------------------------------- rewriting the composed path

   Patching the actor's virtual +0x68 isn't always possible for Seth: his
   object may have no vtable at offset 0, because the Clone Engine allocates it
   differently.  So we also hook the path COMPOSER (+0x130750, two callsites):
   it writes `chr\<Name>\effect\<folder>\<id>` into a buffer, and we swap Seth's
   name for the donor's right after.  That doesn't depend on any vtable, and it
   catches every composition - effects, models and textures alike. */

/* FIVE arguments: the fifth (the folder table "efl\0mod\0") is passed on the
   stack at [rsp+0x20], and the composer reads it.  Declaring only four left
   that slot holding garbage from our own frame, and the game crashed while
   loading the match. */
typedef int (*NativeCompose)(char *, U32, U32, void *, void *);
#define COMPOSE_RVA 0x130750u
static const EflSite compose_sites[] = {
    {0x13070c, {0xe8,0x3f,0x00,0x00,0x00}},
    {0x1308cc, {0xe8,0x7f,0xfe,0xff,0xff}},
};
#define COMPOSE_SITE_COUNT (sizeof(compose_sites)/sizeof(compose_sites[0]))
static DWORD compose_protection[COMPOSE_SITE_COUNT];
static U32 compose_installed;

static int compose_hook(char *buf, U32 bank, U32 index, void *source, void *pastas) {
    NativeCompose native = (NativeCompose)(memory.exe + COMPOSE_RVA);
    int r = native(buf, bank, index, source, pastas);
    if (r >= 0 && buf && SethStealStats.state == ST_BORROWED &&
        borrow_owner && borrow_donor && (U64)source == borrow_owner) {
        char dono[RULE_NAME_MAX], doador[RULE_NAME_MAX], novo[260];
        U32 i = 0, k = 0, n = 0;
        if (!donor_name(borrow_owner, dono) || !donor_name(borrow_donor, doador)) return r;
        if (!(buf[0]=='c' && buf[1]=='h' && buf[2]=='r' && buf[3]=='\\')) return r;
        while (dono[i] && buf[4+i] == dono[i]) i++;
        if (dono[i] || buf[4+i] != '\\') return r;      /* not one of Seth's paths */
        novo[k++]='c'; novo[k++]='h'; novo[k++]='r'; novo[k++]='\\';
        while (doador[n] && k < sizeof(novo)-1) novo[k++] = doador[n++];
        n = 4 + i;
        while (buf[n] && k < sizeof(novo)-1) novo[k++] = buf[n++];
        novo[k] = 0;
        if (!(name_logged & 1u)) {
            name_logged |= 1u;
            log_text("CAMINHO: composto para o doador\r\n");
            log_text("  de: "); log_text(buf); log_text("\r\n");
            log_text("  para: "); log_text(novo); log_text("\r\n");
        }
        for (n = 0; n <= k; n++) buf[n] = novo[n];
    }
    return r;
}

static int compose_install(void) {
    U8 b[5], *relay = 0;
    DWORD old, ignored;
    U32 i, j;
    for (i = 0; i < COMPOSE_SITE_COUNT; i++) {
        if (!read_memory(0, memory.exe + compose_sites[i].rva, b, 5)) return 0;
        for (j = 0; j < 5; j++) if (b[j] != compose_sites[i].original[j]) return 0;
    }
    for (U64 off = 0x2000000; off < 0x10000000; off += 0x10000) {
        relay = VirtualAlloc((void *)(memory.exe + off), 0x1000, 0x3000, 4);
        if (relay) break;
    }
    if (!relay) return 0;
    relay[0] = 0xff; relay[1] = 0x25;
    for (j = 2; j < 6; j++) relay[j] = 0;
    for (j = 0; j < 8; j++) relay[6+j] = (U8)((U64)&compose_hook >> (8*j));
    if (!VirtualProtect(relay, 0x1000, 0x20, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, 16)) return 0;
    for (i = 0; i < COMPOSE_SITE_COUNT; i++) {
        U8 *site = (U8 *)(memory.exe + compose_sites[i].rva);
        long long delta = (long long)(U64)relay - (long long)(U64)(site + 5);
        if (delta < -2147483647LL || delta > 2147483647LL ||
            !VirtualProtect(site, 5, 0x40, &compose_protection[i])) return 0;
        compose_installed = i + 1;
        for (j = 0; j < 4; j++) site[j+1] = (U8)((U32)delta >> (8*j));
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        VirtualProtect(site, 5, compose_protection[i], &ignored);
    }
    return 1;
}


/* ------------------------------------------------------ projectile ownership

   The shot factories are called from the group-3 command handler.  The owner
   CHARACTER is what the drawing class asks about, so it decides the visual;
   the other owner fields decide who the shot belongs to. */

typedef void *(*NativeSpawn)(void *, void *, void *, void *, void *, void *, U32, void *);
#define SPAWN_RVA    0x150e00u   /* sShot__spawnShot   - command 3_30 */
#define SPAWN_EX_RVA 0x150f40u   /* sShot__spawnShotEx - commands 3_31 and 3_32 */

/* An anmchr script can create a projectile from THREE places, not one.  The
   group-3 handler (`anmchrParser__parse3Level`, +0x1038E0) has:

       case 0x30  ->  sShot__spawnShot   (+0x150E00)  callsite 0x106376
       case 0x31  ->  sShot__spawnShotEx (+0x150F40)  callsite 0x10657F
       case 0x32  ->  sShot__spawnShotEx (+0x150F40)  callsite 0x1067CD

   All three pass the owner as the 4th and 5th arguments and the request unit
   as the 6th.  We used to intercept only 3_30, which left a third of the cast
   (everyone whose assist uses `3_31`) spawning shots under Seth with nothing
   to draw them from. */
static const EflSite spawn_sites[] = {
    {0x106376, {0xe8,0x85,0xaa,0x04,0x00}},
};
static const EflSite spawn_ex_sites[] = {
    {0x10657f, {0xe8,0xbc,0xa9,0x04,0x00}},
    {0x1067cd, {0xe8,0x6e,0xa7,0x04,0x00}},
};
#define SPAWN_SITE_COUNT (sizeof(spawn_sites)/sizeof(spawn_sites[0]))
#define SPAWN_EX_SITE_COUNT (sizeof(spawn_ex_sites)/sizeof(spawn_ex_sites[0]))
static DWORD spawn_protection[SPAWN_SITE_COUNT];
static DWORD spawn_ex_protection[SPAWN_EX_SITE_COUNT];
static U32 spawn_installed;

/* sShot__spawnShotEx takes TWELVE arguments, not eight.  Counted from the
   stack slots the caller fills in:

       sShot__spawnShot    rcx rdx r8 r9 + [rsp+0x20..0x38]   ->  8
       sShot__spawnShotEx  rcx rdx r8 r9 + [rsp+0x20..0x58]   -> 12

   The four extra ones are trajectory parameters and have to be passed along
   untouched.  Declaring the wrapper with fewer leaves those slots holding
   garbage from our own frame, and the game crashes. */
typedef void *(*NativeSpawnEx)(void *, void *, void *, void *, void *, void *, U32, void *,
                               U32, void *, U32, void *);

static int spawn_deve_trocar(void *dono) {
    return SethStealStats.state == ST_BORROWED && borrow_owner && borrow_donor &&
           (U64)dono == borrow_owner;
}

/* The 4th argument ends up in uShot__setOwnerCharacter, and it decides BOTH
   the visual (the class asks for chr\<owner>\effect\...) and where the shot is
   born.  We measured it in game both ways: the donor gives the visual but the
   wrong position, Seth gives the right position but no visual.

   We get both by keeping the donor as the owner and standing the donor at
   Seth's position for exactly as long as the factory call takes.  The call is
   synchronous and the game is single-threaded, so nobody ever sees the donor
   out of place. */
#define STEAL_ACTOR_POS 0x40b0u   /* cChr::mPos, vec4 */

static U64 pos_salva[2];
static U32 pos_movido;

static void doador_na_posicao_do_seth(void) {
    U64 a = 0, b = 0;
    pos_movido = 0;
    if (!borrow_owner || !borrow_donor) return;
    if (!rd64(borrow_donor + STEAL_ACTOR_POS, &pos_salva[0]) ||
        !rd64(borrow_donor + STEAL_ACTOR_POS + 8, &pos_salva[1])) return;
    if (!rd64(borrow_owner + STEAL_ACTOR_POS, &a) ||
        !rd64(borrow_owner + STEAL_ACTOR_POS + 8, &b)) return;
    write64(borrow_donor + STEAL_ACTOR_POS, a);
    write64(borrow_donor + STEAL_ACTOR_POS + 8, b);
    pos_movido = 1;
}

static void doador_de_volta(void) {
    if (!pos_movido) return;
    write64(borrow_donor + STEAL_ACTOR_POS, pos_salva[0]);
    write64(borrow_donor + STEAL_ACTOR_POS + 8, pos_salva[1]);
    pos_movido = 0;
}

/* Is this shot driven by a script of its own (the SHT's
   ProjectileAnimationFilePath points at an anmchr)?  Fourteen assist shots in
   the whole cast are - Iron Man's UniBeam, Spencer's Wire and Sentinel's
   ForceBomb among them; everything else is drawn by its class.

   It matters because the two kinds want opposite owners, which we measured in
   game both ways:

       class-drawn   owner = donor  gives the visual (and, with the donor stood
                                    at Seth's position, the right position too)
       script-driven owner = donor  gives neither a visual nor a hitbox
                     owner = Seth   gives the right hitbox and position

   A script-driven shot stays anchored to its owner the whole time, so a donor
   owner drags the entire move over to wherever the donor happens to be.

   We read the field without calling anything.  In assembly, `rShot__getData`
   (+0x14FEB0) is just the two lines below - the decompiler shows a
   `field1_0x70` that looks like a pointer but is really an embedded struct:

       MOV EAX,dword ptr [RCX + 0xc4]
       ADD RAX,qword ptr [RCX + 0x78]

   and the AnimPath sits at `data + 0x1AC` (+0x214 in the file; the 0x68
   difference is where the data starts). */
static int tiro_com_script(void *a2) {
    U64 base = 0;
    U32 add = 0;
    U8 c = 0;
    if (!a2 || !rd64((U64)a2 + 0x78, &base) || !base) return 0;
    if (!rd32((U64)a2 + 0xc4, &add)) return 0;
    if (!read_memory(0, base + add + 0x1ac, &c, 1)) return 0;
    return c != 0;
}

static void spawn_depois(void *proj, void *a2, U64 fabrica) {
    if (!proj) return;
    if (spawn_logged < 8) {
        spawn_logged++;
        log_text("TIRO: nascido com o doador como dono; posse volta em seguida\r\n");
        log_value("  objeto", (U64)proj);
        log_value("  fabrica", fabrica);
        log_value("  script-driven (owner = Seth)", (U64)tiro_com_script(a2));
    }
}

static void *spawn_hook(void *a1, void *a2, void *a3, void *dono,
                        void *dono2, void *pediu, U32 a7, void *a8) {
    NativeSpawn native = (NativeSpawn)(memory.exe + SPAWN_RVA);
    void *proj;
    if (!spawn_deve_trocar(dono))
        return native(a1, a2, a3, dono, dono2, pediu, a7, a8);
    /* Only the 4th argument goes to the donor.  Where each argument lands,
       read from the assembly of the setters:
           a4 setOwnerCharacter  -> [shot+0x1308] and [shot+0x2030]
           a5 setOwner           -> [shot+0x2000]
           a6 setRequestUnit     -> [shot+0x2028]
           getOwnerCharacter     -> returns [shot+0x2030], i.e. a4
       Only a4 feeds the question the class asks, so handing the donor a5 and
       a6 as well never helped the visual - it just gave the projectile to the
       opponent, whose hit reaction then wiped it out. */
    if (tiro_com_script(a2)) {
        proj = native(a1, a2, a3, dono, dono2, pediu, a7, a8);
    } else {
        doador_na_posicao_do_seth();
        proj = native(a1, a2, a3, (void *)borrow_donor, dono2, pediu, a7, a8);
        doador_de_volta();
    }
    spawn_depois(proj, a2, SPAWN_RVA);
    return proj;
}

static void *spawn_ex_hook(void *a1, void *a2, void *a3, void *dono,
                           void *dono2, void *pediu, U32 a7, void *a8,
                           U32 a9, void *a10, U32 a11, void *a12) {
    NativeSpawnEx native = (NativeSpawnEx)(memory.exe + SPAWN_EX_RVA);
    void *proj;
    if (!spawn_deve_trocar(dono))
        return native(a1, a2, a3, dono, dono2, pediu, a7, a8, a9, a10, a11, a12);
    if (tiro_com_script(a2)) {
        proj = native(a1, a2, a3, dono, dono2, pediu, a7, a8, a9, a10, a11, a12);
    } else {
        doador_na_posicao_do_seth();
        proj = native(a1, a2, a3, (void *)borrow_donor, dono2, pediu,
                      a7, a8, a9, a10, a11, a12);
        doador_de_volta();
    }
    spawn_depois(proj, a2, SPAWN_EX_RVA);
    return proj;
}

static int spawn_patch(const EflSite *sites, U32 n, U8 *stub, DWORD *prot) {
    DWORD ignored;
    U32 i, j;
    for (i = 0; i < n; i++) {
        U8 *site = (U8 *)(memory.exe + sites[i].rva);
        long long delta = (long long)(U64)stub - (long long)(U64)(site + 5);
        if (delta < -2147483647LL || delta > 2147483647LL ||
            !VirtualProtect(site, 5, 0x40, &prot[i])) return 0;
        for (j = 0; j < 4; j++) site[j+1] = (U8)((U32)delta >> (8*j));
        FlushInstructionCache(GetCurrentProcess(), site, 5);
        VirtualProtect(site, 5, prot[i], &ignored);
    }
    return 1;
}

static void spawn_stub(U8 *p, void *destino) {
    U32 j;
    p[0] = 0xff; p[1] = 0x25;                 /* jmp [rip+0] */
    for (j = 2; j < 6; j++) p[j] = 0;
    for (j = 0; j < 8; j++) p[6+j] = (U8)((U64)destino >> (8*j));
}

static int spawn_install(void) {
    U8 b[5], *relay = 0;
    DWORD old;
    U32 i, j;
    for (i = 0; i < SPAWN_SITE_COUNT; i++) {
        if (!read_memory(0, memory.exe + spawn_sites[i].rva, b, 5)) return 0;
        for (j = 0; j < 5; j++) if (b[j] != spawn_sites[i].original[j]) return 0;
    }
    for (i = 0; i < SPAWN_EX_SITE_COUNT; i++) {
        if (!read_memory(0, memory.exe + spawn_ex_sites[i].rva, b, 5)) return 0;
        for (j = 0; j < 5; j++) if (b[j] != spawn_ex_sites[i].original[j]) return 0;
    }
    for (U64 off = 0x2000000; off < 0x10000000; off += 0x10000) {
        relay = VirtualAlloc((void *)(memory.exe + off), 0x1000, 0x3000, 4);
        if (relay) break;
    }
    if (!relay) return 0;
    spawn_stub(relay, (void *)&spawn_hook);
    spawn_stub(relay + 0x20, (void *)&spawn_ex_hook);
    if (!VirtualProtect(relay, 0x1000, 0x20, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, 0x40)) return 0;
    if (!spawn_patch(spawn_sites, SPAWN_SITE_COUNT, relay, spawn_protection)) return 0;
    spawn_installed = SPAWN_SITE_COUNT;
    if (!spawn_patch(spawn_ex_sites, SPAWN_EX_SITE_COUNT, relay + 0x20, spawn_ex_protection))
        return 0;
    spawn_installed += SPAWN_EX_SITE_COUNT;
    return 1;
}

/* ------------------------------------------------ action lookup by id

   The script cursor (cParameterTrack at actor +0x1348) doesn't just keep the
   record pointer: every GoTo Frame (`0_02`, `0_04`, `0_1C`) goes through
   cParameterTrack__resolveEntries (+0x13540), which looks the CURRENT action id
   up again in the actor's banks (+0x1368, then anmcmn at +0x1360) with
   sMvc3SceneUI__findInArrayKey0_offset (+0x2952E0) and replaces the record.

   By then Seth's own anmchr is back in the slots, and it has no action 0xD5.
   The lookup returned 0, the record pointer went to 0 and the game crashed -
   measured live on Wolverine, whose hit confirm is a `0_02` at f16.  With the
   old id (0xAC) the same lookup quietly found Seth's OWN 0xAC, which is why
   Wolverine used to finish the move as Vergil's assist instead of crashing.

   So both call sites in resolveEntries come here.  A lookup against one of
   Seth's banks for the id the borrowed record runs under answers with that
   record; everything else goes to the game untouched. */
typedef void *(*NativeLookup)(void *, U32);
#define LOOKUP_RVA 0x2952e0u
static const EflSite lookup_sites[] = {
    {0x01356c, {0xe8,0x6f,0x1d,0x28,0x00}},
    {0x013591, {0xe8,0x4a,0x1d,0x28,0x00}},
};
#define LOOKUP_SITE_COUNT (sizeof(lookup_sites)/sizeof(lookup_sites[0]))
static DWORD lookup_protection[LOOKUP_SITE_COUNT];
static U32 lookup_logged;

static void *lookup_hook(void *bank, U32 key) {
    NativeLookup native = (NativeLookup)(memory.exe + LOOKUP_RVA);
    void *r = native(bank, key);
    if (SethStealStats.state == ST_IDLE || !borrow_owner || !borrow_record || key != borrow_key)
        return r;
    if (!bank || ((U64)bank != seth_bank && (U64)bank != seth_alt && (U64)bank != seth_cmn))
        return r;
    if ((U64)r != borrow_record && lookup_logged < 4) {
        lookup_logged++;
        log_text("BUSCA: GoTo Frame refez a busca da acao no banco do Seth; devolvido o registro emprestado\r\n");
        log_value("  acao", (U64)key);
    }
    return (void *)borrow_record;
}

static int lookup_install(void) {
    U8 b[5], *relay = 0;
    DWORD old;
    U32 i, j;
    for (i = 0; i < LOOKUP_SITE_COUNT; i++) {
        if (!read_memory(0, memory.exe + lookup_sites[i].rva, b, 5)) return 0;
        for (j = 0; j < 5; j++) if (b[j] != lookup_sites[i].original[j]) return 0;
    }
    for (U64 off = 0x2000000; off < 0x10000000; off += 0x10000) {
        relay = VirtualAlloc((void *)(memory.exe + off), 0x1000, 0x3000, 4);
        if (relay) break;
    }
    if (!relay) return 0;
    spawn_stub(relay, (void *)&lookup_hook);
    if (!VirtualProtect(relay, 0x1000, 0x20, &old) ||
        !FlushInstructionCache(GetCurrentProcess(), relay, 0x20)) return 0;
    return spawn_patch(lookup_sites, LOOKUP_SITE_COUNT, relay, lookup_protection);
}

/* ---------------------------------------------------------------- resolver */

static int is_owner_motion_l1(U64 resource) {
    U8 d[0x78];
    U32 i;
    static const char want[] = STEAL_OWNER_MOTION;
    if (!read_memory(0, resource, d, sizeof(d))) return 0;
    if (((U32)d[0x64] | ((U32)d[0x65]<<8) | ((U32)d[0x66]<<16) | ((U32)d[0x67]<<24))
        != STEAL_HASH_MOTION) return 0;
    for (i = 0; i < sizeof(want); i++) if (d[12+i] != (U8)want[i]) return 0;
    return 1;
}

/* While Seth runs the borrowed record, an animation request made against one
   of HIS banks reads the donor's matching bank instead.  The condition is
   checked on every call, so the redirect stops the instant he leaves the
   record - cancel, hit or natural end - with no slot to restore. */
static void *motion_for_donor(void *resource) {
    U64 cur = 0;
    U32 i;
    if (SethStealStats.state != ST_BORROWED || !borrow_owner || !borrow_record) return resource;
    if (!rd64(borrow_owner + STEAL_ACTOR_RECORD, &cur) || cur != borrow_record) return resource;
    for (i = 0; i < motion_n; i++)
        if (seth_motion[i] && (U64)resource == seth_motion[i] && donor_motion[i])
            return (void *)donor_motion[i];
    return resource;
}

static void *resolver(void *resource, U32 motion) {
    void *original, *asked = resource;
    int on = (int)__atomic_load_n(&SethStealStats.enabled, __ATOMIC_ACQUIRE);
    /* The state is brought up to date BEFORE redirecting.  When the borrowed
       script has just jumped into a follow-up, the follow-up's first animation
       request is this very call, and it only gets the donor's list if the
       state already knows the move carried on. */
    if (on) tick_state();
    resource = motion_for_donor(resource);
    original = probe_original(resource, motion);
    if (!on) return original;
    if (!original) log_empty_anim(asked, resource, motion);

    trail();
    find_try();

    if (SethStealStats.state == ST_IDLE) {
        if (++latch_tick >= STEAL_LATCH_PERIOD) { latch_tick = 0; try_latch(); }
        if (latched_donor && is_owner_motion_l1((U64)resource)) {
            U64 owner = 0, cur = 0;
            U32 side = 0, act = 0, bank = 0;
            /* The generic trigger: Seth is running the entry action. */
            if (steal_find_owner(&memory, &owner, &side) == STEAL_OK &&
                rd64(owner + STEAL_ACTOR_RECORD, &cur) &&
                steal_action_of(&memory, owner, cur, &act, &bank) == STEAL_OK &&
                (act == STEAL_ENTRY_ACTION || act == STEAL_ENTRY_OLD1 ||
                 act == STEAL_ENTRY_OLD2  || act == STEAL_ENTRY_OLD3))
                begin_borrow();
            /* Older fallback, in case Seth reached 0x67 before we got here. */
            else if (motion == STEAL_TRIGGER_MOTION)
                begin_borrow();
        }
    }
    return original;
}

/* ------------------------------------------------------------------ load */

static void attach(HANDLE module) {
    char path[1024];
    U8 current[29], jump[14];
    U64 nodes = 0, p1 = 0, p2 = 0;
    DWORD protection, ignored;
    HANDLE pinned;
    void *target;
    U32 n, i;

    n = GetModuleFileNameA(module, path, sizeof(path));
    if (n && n < sizeof(path)-14) {
        char prev[1040];
        U32 k;
        path[n++]='.'; path[n++]='l'; path[n++]='o'; path[n++]='g'; path[n]=0;
        /* The previous session's log is kept as .log.anterior: a crash is
           usually followed by a relaunch, and the relaunch used to wipe the
           one log that explained the crash. */
        for (k = 0; k < n; k++) prev[k] = path[k];
        prev[k++]='.'; prev[k++]='a'; prev[k++]='n'; prev[k++]='t'; prev[k++]='e';
        prev[k++]='r'; prev[k++]='i'; prev[k++]='o'; prev[k++]='r'; prev[k]=0;
        MoveFileExA(path, prev, 1 /* MOVEFILE_REPLACE_EXISTING */);
        log_file = CreateFileA(path, 0x40000000, 3, 0, 2, 0x80, 0);
        path[n-4] = '.'; path[n-3] = 'i'; path[n-2] = 'n'; path[n-1] = 'i';
        rules_load(path);
    }
    log_text("SethRuntimeProbe v4 - roubo generico do assist alpha (0x0AC)\r\n");

    memory.read = read_memory; memory.context = 0;
    memory.exe = (U64)GetModuleHandleA(0);
    if (memory.exe != 0x140000000) { log_text("DISABLED: base de executavel diferente\r\n"); return; }

    target = (void *)(memory.exe + 0x5a3450);
    if (!read_memory(0, (U64)target, current, sizeof(current))) {
        log_text("DISABLED: resolvedor indisponivel\r\n"); return;
    }
    for (i = 0; i < sizeof(current); i++) if (current[i] != probe_signature[i]) {
        log_text("DISABLED: assinatura diferente ou outro hook no resolvedor\r\n"); return;
    }
    /* Startup only: we refuse to attach to a match that's already running. */
    if (!read_memory(0, memory.exe + STEAL_ROSTER_RVA, &nodes, 8)) {
        log_text("DISABLED: lista de personagens ilegivel\r\n"); return;
    }
    if (nodes && (!read_memory(0, nodes + STEAL_TEAM0_HEAD, &p1, 8) ||
                  !read_memory(0, nodes + STEAL_TEAM1_HEAD, &p2, 8) || p1 || p2)) {
        log_text("DISABLED: carregamento tardio; reinicie o jogo\r\n"); return;
    }
    if (!GetModuleHandleExA(5, (const char *)&resolver, &pinned)) {
        log_text("DISABLED: nao foi possivel fixar a DLL\r\n"); return;
    }
    jump[0]=0xff; jump[1]=0x25;
    for (i = 2; i < 6; i++) jump[i]=0;
    for (i = 0; i < 8; i++) jump[6+i] = (U8)((U64)&resolver >> (i*8));
    if (!VirtualProtect(target, 14, 0x40, &protection)) {
        log_text("DISABLED: VirtualProtect falhou\r\n"); return;
    }
    __atomic_store_n(&SethStealStats.enabled, 1, __ATOMIC_RELEASE);
    for (i = 0; i < 14; i++) ((volatile U8 *)target)[i] = jump[i];
    if (!FlushInstructionCache(GetCurrentProcess(), target, 14)) {
        for (i = 0; i < 14; i++) ((volatile U8 *)target)[i] = probe_signature[i];
        __atomic_store_n(&SethStealStats.enabled, 0, __ATOMIC_RELEASE);
        FlushInstructionCache(GetCurrentProcess(), target, 14);
        VirtualProtect(target, 14, protection, &ignored);
        log_text("DISABLED: flush do codigo falhou; bytes originais repostos\r\n"); return;
    }
    if (!VirtualProtect(target, 14, protection, &ignored))
        log_text("WARNING: nao restaurou protecao da pagina\r\n");
    prop_zeros = VirtualAlloc(0, 0x1000, 0x3000, 4 /* PAGE_READWRITE */);
    if (!prop_zeros) log_text("WARNING: sem pagina de props; doador com prop pode crashar\r\n");
    trim_buf = VirtualAlloc(0, TRIM_CAP, 0x3000, 4 /* PAGE_READWRITE */);
    if (!trim_buf) log_text("WARNING: sem buffer; o prologo do assist vai aparecer\r\n");
    if (spawn_install()) {
        log_text("TIRO DONO READY: nascimento de projetil sob o doador\r\n");
        log_value("  callsites (3_30 + 3_31 + 3_32)", (U64)spawn_installed);
    }
    else log_text("WARNING: nao consegui interceptar o nascimento do projetil\r\n");
    if (lookup_install()) log_text("BUSCA READY: GoTo Frame acha o registro emprestado\r\n");
    else log_text("WARNING: busca de acao NAO interceptada; GoTo Frame em golpe copiado pode crashar\r\n");
    if (compose_install()) log_text("CAMINHO READY: composicao de caminho interceptada\r\n");
    else log_text("WARNING: composicao de caminho NAO interceptada\r\n");
    if (efl_install()) log_text("EFL READY: consultas de efeito redirecionaveis\r\n");
    else log_text("WARNING: EFL nao redirecionado; efeitos virao errados\r\n");
    log_text("READY: hook instalado; nenhum golpe roubado ainda\r\n");
}

BOOL DllMain(HANDLE module, DWORD reason, void *reserved) {
    (void)reserved;
    if (reason == 1) { DisableThreadLibraryCalls(module); attach(module); }
    if (reason == 0 && log_file && log_file != (HANDLE)(U64)-1) CloseHandle(log_file);
    return 1;
}
