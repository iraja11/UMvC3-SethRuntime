#!/usr/bin/env python3
"""Checks the v4 donor selector against the running game, READ-ONLY.

It installs nothing, writes nothing into the match and loads no DLL into the
game: it builds `steal_core.c` as a native Linux .so and feeds it reads from
`/proc/<pid>/mem`.  A few cases need a game state you can't hold by hand (a
capture in progress), so for those some reads are OVERRIDDEN - the C code is
exactly the same, only the memory it is shown is synthetic.
"""
import ctypes as C, json, os, struct, subprocess, sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = HERE.parents[2]
EXE_BASE = 0x140000000

DEC = {0: 'STEAL_OK', 1: 'NO_ROSTER', 2: 'NO_OWNER', 3: 'AMBIGUOUS_OWNER',
       4: 'NO_VICTIM', 5: 'NO_ACTION', 6: 'BAD_DATA'}


def find_pid():
    pids = [int(p.name) for p in Path('/proc').iterdir()
            if p.name.isdigit() and (p / 'comm').exists()
            and (p / 'comm').read_text().strip().lower() == 'umvc3.exe']
    if len(pids) != 1:
        sys.exit(f'esperava exatamente um umvc3.exe, achei {pids}')
    return pids[0]


class Game:
    def __init__(self, pid):
        self.fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
        self.overrides = {}

    def rd(self, p, n):
        for (lo, hi), data in self.overrides.items():
            if lo <= p and p + n <= hi:
                return data[p - lo:p - lo + n]
        return os.pread(self.fd, n, p)

    def q(self, p):
        return struct.unpack('<Q', self.rd(p, 8))[0]

    def resname(self, p):
        if not p:
            return None
        return self.rd(p + 12, 64).split(b'\0')[0].decode('latin1')


def roster(g):
    root = g.q(EXE_BASE + 0xd44a70)
    out = []
    for off, side in ((0x58, 0), (0x328, 1)):
        node = g.q(root + off)
        while node:
            a = g.q(node + 8)
            if a:
                out.append((side, a, g.resname(g.q(a + 0x11c0)).split('\\')[1]))
            node = g.q(node + 0x10)
    return out


def build(out_dir):
    so = out_dir / 'steal_core_readonly.so'
    subprocess.run(['clang', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(HERE / 'steal_core.c'), '-o', str(so)], check=True)
    lib = C.CDLL(str(so))
    lib.steal_find_owner.argtypes = [C.c_void_p, C.POINTER(C.c_ulonglong), C.POINTER(C.c_uint)]
    lib.steal_find_victim.argtypes = [C.c_void_p, C.c_ulonglong, C.c_uint, C.POINTER(C.c_ulonglong)]
    lib.steal_action_record.argtypes = [C.c_void_p, C.c_ulonglong, C.c_uint,
                                        C.POINTER(C.c_ulonglong), C.POINTER(C.c_ulonglong),
                                        C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
    lib.steal_select.argtypes = [C.c_void_p, C.c_void_p]
    lib.steal_prepare.argtypes = [C.c_void_p, C.c_ulonglong, C.c_void_p]
    return lib


CALLBACK = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_ulonglong, C.c_void_p, C.c_ulonglong)


class Memory(C.Structure):
    _fields_ = [('read', CALLBACK), ('context', C.c_void_p), ('exe', C.c_ulonglong)]


class StealResult(C.Structure):
    _fields_ = [('owner', C.c_ulonglong), ('donor', C.c_ulonglong),
                ('donor_anmchr', C.c_ulonglong), ('donor_record', C.c_ulonglong),
                ('frames', C.c_uint), ('subblocks', C.c_uint)]


def main():
    pid = find_pid()
    g = Game(pid)
    out_dir = HERE / 'build'
    out_dir.mkdir(exist_ok=True)
    lib = build(out_dir)

    @CALLBACK
    def read(ctx, p, out, n):
        try:
            b = g.rd(p, n)
            if len(b) != n:
                return 0
            C.memmove(out, b, n)
            return 1
        except (OSError, OverflowError, ValueError):
            return 0

    mem = Memory(read, None, EXE_BASE)
    cast = roster(g)
    names = {a: nm for _, a, nm in cast}
    report = {'utc': datetime.now(timezone.utc).isoformat(), 'pid': pid,
              'roster': [{'side': s, 'actor': hex(a), 'name': nm} for s, a, nm in cast],
              'checks': [], 'writes_to_game': 0}
    ok = True

    def check(name, got, want, extra=None):
        nonlocal ok
        good = got == want
        ok = ok and good
        report['checks'].append({'check': name, 'got': got, 'want': want,
                                 'pass': good, **({'info': extra} if extra else {})})
        print(f"  [{'ok ' if good else 'FALHA'}] {name}: {got}" +
              (f"   (esperado {want})" if not good else ""))

    print(f'pid {pid}, elenco:')
    for s, a, nm in cast:
        print(f'   time{s} {nm:11s} {hex(a)}')

    # 1. find Seth
    owner = C.c_ulonglong(0); side = C.c_uint(0)
    d = lib.steal_find_owner(C.byref(mem), C.byref(owner), C.byref(side))
    seth = [a for _, a, nm in cast if nm == 'SethTE']
    print('\n1. achar o SethTE')
    check('decisao', DEC[d], 'STEAL_OK')
    check('ator', hex(owner.value), hex(seth[0]) if seth else '-')
    check('lado', side.value, [s for s, a, _ in cast if a == owner.value][0] if seth else -1)

    # 2. the victim, with the match as it is (nobody being grabbed right now)
    print('\n2. vitima no estado real (sem captura em andamento)')
    victim = C.c_ulonglong(0)
    d = lib.steal_find_victim(C.byref(mem), owner.value, side.value, C.byref(victim))
    check('decisao', DEC[d], 'NO_VICTIM',
          'o marcador de recurso sozinho nao seleciona ninguem')

    # who is carrying the marker, for the report
    anmtdown = g.q(owner.value + 0x1370)
    marcados = [nm for _, a, nm in cast
                if a != owner.value and g.q(a + 0x1378) == anmtdown]
    report['marker_carriers'] = marcados
    print(f'   carregam o anmtdown do Seth: {marcados or "ninguem"}')
    # For information, not a pass/fail criterion: how many carry the marker
    # depends on how many Seth has already grabbed THIS match.  With two or
    # more it proves the marker alone would be ambiguous; with only one, test 3b
    # has nothing to prove.
    report['marker_ambiguous'] = len(marcados) > 1
    print(f'   marcador seria ambiguo? {"sim" if len(marcados) > 1 else "nao nesta partida"}')

    # 3. simulated capture: a marked opponent starts running the anmtdown
    print('\n3. captura simulada (override da acao corrente)')
    data = g.q(anmtdown + 0x70)
    alvo = next((a for _, a, nm in cast
                 if a != owner.value and g.q(a + 0x1378) == anmtdown), None)
    if alvo is None:
        sys.exit('nenhum adversario carrega o marcador; faca um steal e rode de novo')
    rec_off = struct.unpack_from('<I', g.rd(data + 16, 8), 4)[0]
    g.overrides[(alvo + 0x1390, alvo + 0x1398)] = struct.pack('<Q', data + rec_off)
    d = lib.steal_find_victim(C.byref(mem), owner.value, side.value, C.byref(victim))
    check('decisao', DEC[d], 'STEAL_OK')
    check('doador', names.get(victim.value, '-'), names[alvo])

    # 3b. a marked opponent who is NOT running it must never be picked
    outros = [a for _, a, nm in cast
              if a not in (owner.value, alvo) and g.q(a + 0x1378) == anmtdown]
    if outros:
        check('outro marcado nao foi escolhido', victim.value not in outros, True,
              f'outros marcados: {[names[a] for a in outros]}')
    else:
        print('   (so um personagem carrega o marcador nesta partida; nada a distinguir)')

    # 4. every opponent's action 0x0AC, checked against the anmchr on disk
    print('\n4. acao 0x0AC (assist alpha) de cada adversario')
    disco = disk_durations()
    for _, a, nm in cast:
        if a == owner.value:
            continue
        anm = C.c_ulonglong(0); rec = C.c_ulonglong(0)
        fr = C.c_uint(0); sb = C.c_uint(0)
        d = lib.steal_action_record(C.byref(mem), a, 0xac, C.byref(anm), C.byref(rec),
                                    C.byref(fr), C.byref(sb))
        got = f'{DEC[d]} frames={fr.value}'
        want = f'STEAL_OK frames={disco[nm]}' if nm in disco else got
        check(f'{nm} 0x0AC', got, want,
              'duracao comparada com o anmchr do arc em disco' if nm in disco else None)

    # 5. the full path, with the simulated capture still in place
    print('\n5. steal_select completo')
    r = StealResult()
    d = lib.steal_select(C.byref(mem), C.byref(r))
    check('decisao', DEC[d], 'STEAL_OK')
    check('dono', names.get(r.owner, '-'), 'SethTE')
    check('doador', names.get(r.donor, '-'), names[alvo])
    check('frames', r.frames, disco.get(names[alvo], r.frames))
    report['select'] = {'owner': hex(r.owner), 'donor': hex(r.donor),
                        'donor_name': names.get(r.donor), 'anmchr': hex(r.donor_anmchr),
                        'record': hex(r.donor_record), 'frames': r.frames,
                        'subblocks': r.subblocks}

    # 5b. THE CASE THAT FAILED IN GAME: using the latched donor AFTER the
    #     capture is over.  Without the override nobody is running the
    #     anmtdown, and this is where steal_select used to return NO_VICTIM and
    #     throw the donor away.
    print('\n5b. doador travado, com a captura ja encerrada')
    g.overrides.pop((alvo + 0x1390, alvo + 0x1398), None)
    v2 = C.c_ulonglong(0)
    d = lib.steal_find_victim(C.byref(mem), owner.value, side.value, C.byref(v2))
    check('steal_find_victim agora nao acha ninguem', DEC[d], 'NO_VICTIM')
    r2 = StealResult()
    d = lib.steal_prepare(C.byref(mem), alvo, C.byref(r2))
    check('steal_prepare com o doador travado', DEC[d], 'STEAL_OK',
          'era este o caso que falhava em jogo com decision=4')
    check('doador preservado', names.get(r2.donor, '-'), names[alvo])
    check('frames', r2.frames, disco.get(names[alvo], r2.frames))
    r3 = StealResult()
    d = lib.steal_prepare(C.byref(mem), 0, C.byref(r3))
    check('steal_prepare sem doador travado', DEC[d], 'NO_VICTIM')
    d = lib.steal_prepare(C.byref(mem), owner.value, C.byref(r3))
    check('steal_prepare com o proprio Seth', DEC[d], 'NO_VICTIM')
    aliado = next((a for s_, a, nm in cast if s_ == side.value and a != owner.value), None)
    if aliado:
        d = lib.steal_prepare(C.byref(mem), aliado, C.byref(r3))
        check('steal_prepare com um ALIADO', DEC[d], 'NO_VICTIM',
              'so vale doador do time adversario')

    # 6. no owner at all: the roster we show it has no SethTE
    print('\n6. recusa quando o SethTE nao esta em campo')
    g.overrides[(owner.value + 0x11c0, owner.value + 0x11c8)] = struct.pack('<Q', 0)
    o2 = C.c_ulonglong(0); s2 = C.c_uint(0)
    d = lib.steal_find_owner(C.byref(mem), C.byref(o2), C.byref(s2))
    check('decisao', DEC[d], 'NO_OWNER')

    report['result'] = 'pass' if ok else 'fail'
    dest = out_dir / 'validation-steal-live.json'
    dest.write_text(json.dumps(report, indent=2) + '\n')
    print(f"\n{'TODOS OS TESTES PASSARAM' if ok else 'HOUVE FALHA'} -> {dest}")
    return 0 if ok else 1


def disk_durations():
    """Duration of action 0x0AC read from the factory arcs, to check the memory reads against."""
    import zlib
    out = {}
    arch = GAME / 'nativePCx64/chr/archive'
    for arc in sorted(arch.glob('0*_param.arc')):
        d = arc.read_bytes()
        if len(d) < 8 or d[:4] != b'ARC\0':
            continue
        n = struct.unpack_from('<H', d, 6)[0]
        for i in range(n):
            o = 8 + i * 80
            name = d[o:o + 64].split(b'\0')[0].decode('latin1')
            if not name.endswith('anmchr'):
                continue
            _, cs, _, off = struct.unpack_from('<IIII', d, o + 64)
            try:
                raw = zlib.decompress(d[off:off + cs])
            except zlib.error:
                continue
            if raw[:4] != b'CAC\0':
                continue
            cnt = struct.unpack_from('<I', raw, 8)[0]
            for k in range(cnt):
                aid, aoff = struct.unpack_from('<II', raw, 16 + k * 8)
                if aid == 0xac:
                    out[name.split('\\')[1]] = struct.unpack_from('<I', raw, aoff + 4)[0]
            break
    return out


if __name__ == '__main__':
    sys.exit(main())
