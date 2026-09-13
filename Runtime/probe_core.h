#ifndef SETH_PROBE_CORE_H
#define SETH_PROBE_CORE_H

typedef unsigned char U8;
typedef unsigned short U16;
typedef unsigned int U32;
typedef unsigned long long U64;
#define PROBE_VERSION 3
#define PROBE_FIRST 240u
#define PROBE_LAST 242u
#define PROBE_DONOR_FIRST 53u
#define PROBE_DONOR_PATH "chr\\Chunli\\motion\\Chunli_l1"
typedef int (*ProbeRead)(void *context, U64 address, void *out, U64 size);
typedef struct { ProbeRead read; void *context; U64 exe; } ProbeMemory;
typedef struct {
    U64 owner, donor, donor_resource, donor_motion;
    U32 frames, tracks;
} ProbeResult;
enum ProbeDecision {
    PROBE_UNRELATED, PROBE_BORROW, PROBE_NO_ROSTER,
    PROBE_AMBIGUOUS_OWNER, PROBE_NO_DONOR, PROBE_INVALID_MOTION
};

/* Never writes game data; whatever it returns is a motion that already exists. */
int probe_select(const ProbeMemory *m, U64 resource, U32 motion, ProbeResult *out);
/* Does exactly what the game's own resolver does, for every call that isn't ours
   to redirect. */
void *probe_original(void *resource, U32 motion);
extern const U8 probe_signature[29];
#endif
