#!/usr/bin/env python3
"""Builds the v4 ASI (the generic assist-alpha steal) without installing it and
without touching the game.

The result goes to build/SethRuntimeProbe.v4.asi.  Copying it next to the game
executable is a separate, deliberate step - building should never be able to
break a working install.
"""
from pathlib import Path
import hashlib, json, subprocess, sys

HERE = Path(__file__).resolve().parent
OUT = HERE / 'build'
GAME = HERE.parents[2]


def run(args):
    subprocess.run([str(x) for x in args], cwd=OUT, check=True)


def main():
    OUT.mkdir(exist_ok=True)

    # 1. The pure core has to compile cleanly as native code under the
    #    sanitizers first, so a logic bug shows up here on Linux instead of
    #    as a crash in the game.
    run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
         '-fsanitize=address,undefined', '-c', HERE / 'steal_core.c',
         '-o', OUT / 'steal_core_checked.o'])

    exports = ['GetModuleHandleA', 'GetModuleFileNameA', 'GetCurrentProcess',
               'ReadProcessMemory', 'VirtualProtect', 'FlushInstructionCache',
               'DisableThreadLibraryCalls', 'CreateFileA', 'WriteFile', 'CloseHandle',
               'GetModuleHandleExA', 'ReadFile', 'LoadLibraryA', 'GetProcAddress', 'ExitProcess',
               'VirtualAlloc']
    (OUT / 'kernel32.def').write_text('LIBRARY KERNEL32.dll\nEXPORTS\n' + '\n'.join(exports) + '\n')
    run(['llvm-dlltool', '-m', 'i386:x86-64', '-d', OUT / 'kernel32.def', '-l', OUT / 'kernel32.lib'])

    flags = ['clang', '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2', '-ffreestanding',
             '-fno-builtin', '-fno-stack-protector', '-funwind-tables',
             '-Wall', '-Wextra', '-Werror', '-c']
    for name in ['probe_core', 'steal_core', 'steal_win']:
        run(flags + [HERE / (name + '.c'), '-o', OUT / (name + '.v4.obj')])

    run(['lld-link', '/dll', '/entry:DllMain', '/nodefaultlib', '/machine:x64',
         '/dynamicbase', '/nxcompat', '/timestamp:0', '/opt:ref',
         '/out:' + str(OUT / 'SethRuntimeProbe.v4.asi'),
         OUT / 'probe_core.v4.obj', OUT / 'steal_core.v4.obj', OUT / 'steal_win.v4.obj',
         OUT / 'kernel32.lib'])

    art = OUT / 'SethRuntimeProbe.v4.asi'
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    installed = GAME / 'SethRuntimeProbe.asi'
    manifest = {
        'artifact': art.name,
        'sha256': sha(art),
        'size': art.stat().st_size,
        'status': 'compilado; NAO instalado; nao testado em jogo',
        'installed_asi_sha256': sha(installed) if installed.exists() else None,
        'source_sha256': {p.name: sha(p) for p in sorted(HERE.glob('steal_*.[ch]'))},
    }
    (OUT / 'manifest-v4.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('ASI v4:', art)
    print('SHA256:', manifest['sha256'], f"({manifest['size']} bytes)")
    print('instalado continua sendo a v3:', manifest['installed_asi_sha256'])
    return 0


if __name__ == '__main__':
    sys.exit(main())
