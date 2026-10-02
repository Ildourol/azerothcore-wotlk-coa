CLI_DESCRIPTION = """Check the Extensions.dll compatibility layer's embedded Lua without a game client.

The CoA glue the DLL registers into the client's live Lua state is C++ string literals, so a typo in
it is invisible to every other suite: nothing compiles it until the client runs it, where it surfaces
as a Lua error in the player's face. This extracts every embedded Lua chunk from the DLL's sources and
compiles it, then checks the glue against the bindings it calls and the install steps it names, and
that the trees' refresh guards are still wired onto the frames.

The DLL checkout is found beside this one (--dll-root, or COA_DLL_ROOT). With no checkout or no Lua
compiler the suite skips rather than fails: it is a check of another repository's sources.

The second half is the other repository again, one layer out: the shipped CoA add-on whose window the
glue drives. Every name the glue reaches for on a client table or frame has to exist in the sources
the client loads, found beside this one too (--live-lua-dir, or COA_LIVE_LUA_DIR); with none the
name checks skip.
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
WORKSPACE = HERE.parents[5]
DLL_SUBDIR = 'ascension-extensions-reconstruction/ascension-extensions-reconstruction'
GLUE_SOURCE = 'src/Ascension/AscCAMgr.cpp'
BINDING_SOURCE = 'src/Ascension/AscBindings.cpp'

CONTINUED = re.compile(r'^"(?P<body>(?:[^"\\]|\\.)*)"\\$')
TERMINATED = re.compile(r'^"(?P<body>(?:[^"\\]|\\.)*)";$')
INLINE_LUA = re.compile(r'RunLua\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
BINDING = re.compile(r'\{"C_CharacterAdvancement",\s*"(\w+)"')
STEP = re.compile(r"InstallStep\('(\w+)',\s*(\w+)\)")
LUA_MARKER = re.compile(r'(?m)^\s*(?:local function|function|local \w+\s*=|if .*\bthen$|-- )')
LIVE_SUBDIR = Path('.scratch') / 'live'

CLIENT_TABLE = ('CoATalentFrame', 'TalentTreeBaseMixin', 'CharacterAdvancementUtil', 'C_ClassInfo',
                'C_BuildCreator', 'BuildCreatorUtil')
FIELD = re.compile(r'\b(?:' + '|'.join(CLIENT_TABLE) + r'|view|tree|gateInfo|self)[.:](\w+)')
NOT_THE_ADDONS = {'HookScript', 'GetParent', 'StaticPopupDialogs', 'ACCEPT', 'CANCEL', 'CreateFrame',
                  'StaticPopup_Show', 'HideUIPanel', 'coaTreeGuarded', 'coaTreeGuardsHooked',
                  'coaBrowseEndsHooked'}


class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, value, name):
        if value:
            self.passed += 1
            print(f'PASS: {name}')
        else:
            self.failed += 1
            print(f'FAIL: {name}')


def unescape(literal):
    return json.loads('"' + literal + '"')


def literal_at(lines, index):
    return (TERMINATED.match(lines[index].strip()) or CONTINUED.match(lines[index].strip()))


def chunks(source):
    lines = source.split('\n')
    found, index = [], 0
    while index < len(lines):
        if literal_at(lines, index) is None:
            index += 1
            continue
        start, parts = index, []
        while index < len(lines):
            if lines[index].strip() == '':
                lookahead = index
                while lookahead < len(lines) and lines[lookahead].strip() == '':
                    lookahead += 1
                if lookahead >= len(lines) or literal_at(lines, lookahead) is None:
                    break
                index = lookahead
                continue
            match = literal_at(lines, index)
            if match is None:
                break
            parts.append(unescape(match.group('body')))
            terminated = TERMINATED.match(lines[index].strip()) is not None
            index += 1
            if terminated:
                break
        found.append((start + 1, ''.join(parts), index - start))
    return found


def lua_compiler():
    try:
        import lupa
        runtime = lupa.LuaRuntime()
        return runtime.compile
    except ImportError:
        pass
    try:
        from luaparser import ast
        return lambda text: ast.parse(text)
    except ImportError:
        return None


def client_lua(argument):
    for candidate in filter(None, [argument, os.environ.get('COA_LIVE_LUA_DIR'),
                                   WORKSPACE / LIVE_SUBDIR]):
        path = Path(candidate)
        if path.is_dir() and any(path.glob('*.lua')):
            return {p.name: p.open(encoding='utf-8', errors='replace').read()
                    for p in list(path.glob('*.lua')) + list(path.glob('*.xml'))}
    return {}


def dll_root(argument):
    for candidate in filter(None, [argument, os.environ.get('COA_DLL_ROOT'),
                                   WORKSPACE / DLL_SUBDIR]):
        path = Path(candidate)
        if (path / GLUE_SOURCE).is_file():
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=CLI_DESCRIPTION)
    parser.add_argument('--dll-root', type=Path, help='The Extensions.dll checkout to read.')
    parser.add_argument('--live-lua-dir', type=Path,
                        help='The shipped CoA add-on sources the client loads, for the name checks.')
    args = parser.parse_args()

    root = dll_root(args.dll_root)
    if root is None:
        print(f'SKIP: no Extensions.dll checkout found (--dll-root, COA_DLL_ROOT or '
              f'{WORKSPACE / DLL_SUBDIR})')
        return
    compile_lua = lua_compiler()
    if compile_lua is None:
        print('SKIP: no Lua compiler (install lupa or luaparser)')
        return

    checks = Checks()
    sources = {path.relative_to(root): path.open(encoding='utf-8', newline='').read()
               for path in sorted((root / 'src').rglob('*.cpp'))}
    registered = sources[Path(GLUE_SOURCE)]
    glue = next(text for _, text, length in chunks(registered)
                if length > 1 and 'local function InstallAll' in text)

    lua = [(name, line, text)
           for name, body in sources.items()
           for line, text, length in chunks(body)
           if length > 1 and LUA_MARKER.search(text)]
    lua += [(name, 0, unescape(match))
            for name, text in sources.items()
            for match in INLINE_LUA.findall(text)]
    checks.check(lua, f'{root.name} embeds Lua chunks to check ({len(lua)} found)')
    for name, line, text in lua:
        try:
            compile_lua(text)
            checks.check(True, f'{name}:{line} compiles ({len(text)} bytes)')
        except Exception as error:
            checks.check(False, f'{name}:{line} compiles: {error}')

    bound = set().union(*[set(BINDING.findall(text)) for text in sources.values()])
    checks.check(bound, f'the binding tables name the bindings checked ({len(bound)} names)')
    called = set(re.findall(r'C_CharacterAdvancement\.(\w+)', glue))
    missing = sorted(called - bound)
    checks.check(not missing,
                 f'every C_CharacterAdvancement the glue calls is bound (missing: {missing or "none"})')

    steps = STEP.findall(glue)
    undefined = sorted({name for _, name in steps
                        if not re.search(r'(?m)^\s*(?:local )?function ' + name + r'\(', glue)})
    checks.check(steps and not undefined,
                 f'the install steps call functions the glue defines ({len(steps)} steps, '
                 f'undefined: {undefined or "none"})')
    checks.check(('InstallTreeGuards', 'InstallTreeGuards') in steps,
                 'the install runs the trees guard install')

    checks.check('tree.Update = function' in glue and 'tree.UpdateGates = function' in glue,
                 'both tree guards are installed on the frames themselves')
    checks.check("frame:HookScript('OnShow', GuardWindowTrees)" in glue,
                 'the window re-asserts the guards every time it is shown')
    mark = glue[glue.index('function CoACompatMarkTrees'):]
    checks.check('InstallTreeGuards()' in mark[:mark.index('\nend\n')],
                 'every build written to the window re-asserts the guards')
    checks.check('pcall(self.CreateGates, self)' in glue,
                 'a gate build that fails clears its own guard rather than disabling it')

    checks.check('GetEnvironmentVariableA("COA_COMPAT_TRACE"' in registered,
                 'the browse/preview trace has one switch, read from the environment')
    checks.check(registered.count('if (!CoATraceEnabled())') >= 2,
                 'the trace writer and the build formatter both stop before they do any work')
    checks.check('local enabled = C_CharacterAdvancement.CompatTraceEnabled' in glue and
                 'enabled and enabled()' in glue,
                 "the trace's Lua half asks the switch before replacing the window's save path")

    live = client_lua(args.live_lua_dir)
    if not live:
        print(f'SKIP: no shipped CoA add-on sources found (--live-lua-dir, COA_LIVE_LUA_DIR or '
              f'{WORKSPACE / LIVE_SUBDIR}); the client-name checks did not run')
    else:
        reached = {name for name in FIELD.findall(glue) if name not in NOT_THE_ADDONS}
        absent = sorted(name for name in reached if not any(name in text for text in live.values()))
        checks.check(not absent,
                     f'every client name the glue reaches for exists in the shipped add-on '
                     f'({len(reached)} names in {len(live)} file(s), missing: {absent or "none"})')

    print(f'{checks.passed}/{checks.passed + checks.failed} checks passed')
    raise SystemExit(1 if checks.failed else 0)


if __name__ == '__main__':
    main()
