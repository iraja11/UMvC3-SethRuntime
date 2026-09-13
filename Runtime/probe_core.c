#include "probe_core.h"

const U8 probe_signature[29] = {
    0x4c,0x8b,0x41,0x70,0x4d,0x85,0xc0,0x74,0x11,0x41,
    0x0f,0xb7,0x48,0x06,0x3b,0xd1,0x73,0x08,0x8b,0xc2,
    0x49,0x8b,0x44,0xc0,0x08,0xc3,0x33,0xc0,0xc3
};

static U16 u16(const U8 *p) { return (U16)(p[0] | (p[1] << 8)); }
static U32 u32(const U8 *p) {
    return (U32)p[0] | ((U32)p[1]<<8) | ((U32)p[2]<<16) | ((U32)p[3]<<24);
}
static U64 u64(const U8 *p) { return u32(p) | ((U64)u32(p+4)<<32); }
static int read_at(const ProbeMemory *m, U64 p, void *out, U64 n) {
    return p >= 0x10000 && p+n >= p && m->read(m->context,p,out,n);
}
static int ptr(const ProbeMemory *m, U64 p, U64 *out) {
    U8 d[8];
    if (!read_at(m,p,d,8)) return 0;
    *out=u64(d); return 1;
}
static int name_is(const U8 *p, const char *name) {
    U32 i;
    for (i=0;i<64;i++) {
        if (p[i] != (U8)name[i]) return 0;
        if (!name[i]) return 1;
    }
    return 0;
}
static int resource_named(const ProbeMemory *m, U64 p, const char *name, U64 *data) {
    U8 d[0x78];
    if (!read_at(m,p,d,sizeof(d)) || u32(d+0x64)!=0x76820d81 ||
        !name_is(d+12,name)) return 0;
    *data=u64(d+0x70); return 1;
}

int probe_select(const ProbeMemory *m, U64 resource, U32 motion, ProbeResult *out) {
    U64 unused, root, head[2], owners[2]={0,0}, donors[2]={0,0};
    U64 donor_resources[2]={0,0}, seen[16], lmt, mp, tracks;
    U32 owner_n[2]={0,0}, donor_n[2]={0,0}, team, n, i, side, source;
    U8 h[96], edge[48];
    if (motion<PROBE_FIRST || motion>PROBE_LAST ||
        !resource_named(m,resource,"chr\\SethTE\\motion\\SethTE_l1",&unused))
        return PROBE_UNRELATED;
    source=PROBE_DONOR_FIRST+motion-PROBE_FIRST;
    if (!ptr(m,m->exe+0xd44a70,&root) || !root ||
        !ptr(m,root+0x58,&head[0]) || !ptr(m,root+0x328,&head[1]))
        return PROBE_NO_ROSTER;
    for (team=0;team<2;team++) {
        U64 node=head[team];
        for (n=0;node && n<16;n++) {
            U64 actor, res, next;
            for (i=0;i<n;i++) if (seen[i]==node) return PROBE_NO_ROSTER;
            seen[n]=node;
            if (!ptr(m,node+8,&actor) || !ptr(m,node+0x10,&next)) return PROBE_NO_ROSTER;
            if (actor) {
                if (!ptr(m,actor+0x11c0,&res)) return PROBE_NO_ROSTER;
                if (res==resource) { owners[team]=actor; owner_n[team]++; }
                if (resource_named(m,res,PROBE_DONOR_PATH,&unused)) {
                    donors[team]=actor; donor_resources[team]=res; donor_n[team]++;
                }
            }
            node=next;
        }
        if (node) return PROBE_NO_ROSTER;
    }
    if (owner_n[0]+owner_n[1]!=1) return PROBE_AMBIGUOUS_OWNER;
    side=owner_n[0]?0:1;
    if (donor_n[1-side]!=1) return PROBE_NO_DONOR;
    if (!resource_named(m,donor_resources[1-side],PROBE_DONOR_PATH,&lmt) ||
        !read_at(m,lmt,h,8) || u32(h)!=0x544d4c || u16(h+4)!=67 || u16(h+6)<=source ||
        !ptr(m,lmt+8+source*8,&mp) || !mp || !read_at(m,mp,h,96))
        return PROBE_INVALID_MOTION;
    tracks=u64(h);
    if (!u32(h+8) || u32(h+8)>1024 || !u32(h+12) || u32(h+12)>100000 ||
        !read_at(m,tracks,edge,48) || !read_at(m,tracks+(u32(h+8)-1)*48,edge,48))
        return PROBE_INVALID_MOTION;
    out->owner=owners[side]; out->donor=donors[1-side];
    out->donor_resource=donor_resources[1-side]; out->donor_motion=mp;
    out->frames=u32(h+12); out->tracks=u32(h+8);
    return PROBE_BORROW;
}

void *probe_original(void *resource, U32 motion) {
    U8 *data=*(U8 **)((U8 *)resource+0x70);
    if (!data || motion>=*(U16 *)(data+6)) return (void *)0;
    return *(void **)(data+8+(U64)motion*8);
}
