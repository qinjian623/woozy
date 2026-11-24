"""IR contract for the Native automatic-GC safepoint poll.

The backend inlines a one-load advisory fast path at every original safepoint
(function entry and natural-loop headers).  The slow path calls the proven
controller entry point exactly once; a generated program must not inline the
authoritative request/pressure words, and must not call the world poll or the
pressure handler directly.

This module is test-only.  It parses the pre-optimization LLVM text emitted by
``--out-llvm-before``; it does not use LLVM Python bindings.
"""
from __future__ import annotations

import re
from collections import OrderedDict

SAFEPOINT = "lang_gc_safepoint_word_v1"
SLOW = "lang_gc_runtime_poll_v1"
WORLD = "lang_gc_world_poll_v1"
PRESSURE = "lang_gc_runtime_poll_pressure_v1"
# The authoritative words are still exported for tests and for older inline
# fast paths.  A generated safepoint must not read them itself, and a module
# that still does is rejected below instead of silently passing.
REQUEST = "lang_gc_world_request_word_v1"
LIMIT = "lang_gc_runtime_pressure_limit_v1"
ACCUM = "lang_gc_runtime_pressure_accum_v1"

_VALUE = r"%[-A-Za-z0-9_.$]+"
_TARGET = r'%("[^"]+"|[-A-Za-z0-9_.$]+)'
_DEFINE_RE = re.compile(
    r'^define\b.*?@(?P<name>"[^"]+"|[^\s(]+)\s*\('
)
_DEFAULT_HIR_BLOCK_RE = re.compile(r'^b[0-9]+$')
_LABEL_RE = re.compile(
    r'^(?P<name>"[^"]+"|[^\s:]+):(?:\s*;.*)?$'
)
_ASSIGN_RE = re.compile(
    r'^(?P<value>' + _VALUE + r')\s*=\s*(?P<expr>.*)$'
)
_TARGET_RE = re.compile(r'label\s+' + _TARGET)
def _global_reference(name):
    return r'@("' + re.escape(name) + r'"|' + re.escape(name) + r')'


_REQUEST_RE = re.compile(
    r'^(?P<value>' + _VALUE +
    r')\s*=\s*load atomic volatile i32, ptr ' + _global_reference(REQUEST) +
    r' acquire, align 4$'
)
_LIMIT_RE = re.compile(
    r'^(?P<value>' + _VALUE +
    r')\s*=\s*load atomic volatile i64, ptr ' + _global_reference(LIMIT) +
    r' acquire, align 8$'
)
_ACCUM_RE = re.compile(
    r'^(?P<value>' + _VALUE +
    r')\s*=\s*load atomic volatile i64, ptr ' + _global_reference(ACCUM) +
    r' acquire, align 8$'
)
_SAFEPOINT_RE = re.compile(
    r'^(?P<value>' + _VALUE +
    r')\s*=\s*load atomic volatile i64, ptr ' +
    _global_reference(SAFEPOINT) + r' acquire, align 8$'
)
_ICMP_SLE_RE = re.compile(
    r'^(?P<value>' + _VALUE +
    r')\s*=\s*icmp sle i64 (?P<input>' + _VALUE + r'), 0$'
)
_BR_COND_RE = re.compile(
    r'^br i1 (?P<condition>' + _VALUE + r'), label ' + _TARGET +
    r', label ' + _TARGET + r', !prof ![0-9]+$'
)
# Every atomic load, regardless of ordering keyword, volatility, or the
# spelling of its result and pointer operands (plain or quoted SSA names).
# Used to reject a safepoint that re-reads the authoritative words itself
# (quoted or unquoted, directly or through a pointer alias) or that loads the
# advisory word with anything but the exact acquire shape.
_SSA = r'%(?:"[^"]+"|[-A-Za-z0-9_.$]+)'
_SSA_TOKEN_RE = re.compile(_SSA)
_ANY_ATOMIC_LOAD_RE = re.compile(
    r'^(?P<value>' + _SSA + r')\s*=\s*load atomic\b(?P<rest>.*)$'
)
_POINTER_ALIAS_RE = re.compile(
    r'^(?P<value>' + _SSA + r')\s*=\s*'
    r'(?:getelementptr|bitcast|addrspacecast)\b'
)
_AUTHORITATIVE_NAMES = (REQUEST, LIMIT, ACCUM)
_AUTHORITATIVE_REF_RE = re.compile(
    r'@(?:"(?:' + '|'.join(re.escape(name) for name in _AUTHORITATIVE_NAMES) +
    r')"|(?:' + '|'.join(re.escape(name) for name in _AUTHORITATIVE_NAMES) +
    r'))(?![A-Za-z0-9_.$-])'
)
_SAFEPOINT_REF_RE = re.compile(
    r'@(?:"' + re.escape(SAFEPOINT) + r'"|' + re.escape(SAFEPOINT) +
    r')(?![A-Za-z0-9_.$-])'
)
_BR_LABEL_RE = re.compile(r'^br label (?P<target>' + _TARGET + r')$')
# A controller/world/pressure call in any tail position is still a call.
_CALL_RE = re.compile(
    r'^(?:(?:tail|musttail|notail) )?call void @(?P<name>[^\s(]+)\(\)$'
)


class ContractError(RuntimeError):
    """A generated safepoint or its fast-path condition is wrong."""


def _unquote(name: str) -> str:
    if len(name) >= 2 and name[0] == '"' and name[-1] == '"':
        return name[1:-1]
    return name


def _target_name(name: str) -> str:
    if name.startswith("%"):
        name = name[1:]
    return _unquote(name)


def _first(lines, pattern, what, context):
    for index, line in enumerate(lines):
        match = pattern.match(line)
        if match:
            return index, match
    raise ContractError(f"{context}: missing {what}")


def _only(lines, pattern, what, context):
    found = []
    for index, line in enumerate(lines):
        match = pattern.match(line)
        if match:
            found.append((index, match))
    if len(found) != 1:
        raise ContractError(
            f"{context}: expected exactly one {what}, found {len(found)}"
        )
    return found[0]


def _call_count(lines, symbol):
    count = 0
    for line in lines:
        match = _CALL_RE.match(line)
        if match and _unquote(match.group("name")) == symbol:
            count += 1
    return count


def _parse_functions(llvm: str):
    functions = OrderedDict()
    current = None
    block = None
    for raw in llvm.splitlines():
        line = raw.strip()
        match = _DEFINE_RE.match(line)
        if match:
            name = _unquote(match.group("name"))
            current = {"name": name, "blocks": OrderedDict()}
            functions[name] = current
            block = None
            continue
        if current is None:
            continue
        if line == "}":
            current = None
            block = None
            continue
        match = _LABEL_RE.match(line)
        if match:
            block = _unquote(match.group("name"))
            current["blocks"][block] = []
            continue
        if block is not None:
            current["blocks"][block].append(line)
    return functions


def _block_targets(lines):
    targets = []
    for line in lines:
        if line.startswith(";"):
            continue
        targets.extend(_unquote(name) for name in _TARGET_RE.findall(line))
    return targets


def _expected_safepoints(function, hir_block=None):
    blocks = function["blocks"]
    names = list(blocks)
    if not names:
        return set()
    hir_block = hir_block or _DEFAULT_HIR_BLOCK_RE
    entry = next((name for name in names if hir_block.match(name)), names[0])
    index = {name: position for position, name in enumerate(names)}
    successors = {name: _block_targets(lines) for name, lines in blocks.items()}
    predecessors = {name: [] for name in names}
    for source, targets in successors.items():
        for target in targets:
            if target in predecessors:
                predecessors[target].append(source)

    reachable = set()
    work = [entry]
    while work:
        name = work.pop()
        if name in reachable:
            continue
        reachable.add(name)
        work.extend(target for target in successors[name]
                    if target in blocks and target not in reachable)

    if not reachable:
        return set()
    all_bits = 0
    for name in reachable:
        all_bits |= 1 << index[name]
    dominators = {name: all_bits for name in reachable}
    dominators[entry] = 1 << index[entry]
    changed = True
    while changed:
        changed = False
        for name in names[1:]:
            if name not in reachable:
                continue
            bits = all_bits
            has_predecessor = False
            for predecessor in predecessors[name]:
                if predecessor not in reachable:
                    continue
                if not has_predecessor:
                    bits = dominators[predecessor]
                    has_predecessor = True
                else:
                    bits &= dominators[predecessor]
            if not has_predecessor:
                bits = 0
            bits |= 1 << index[name]
            if bits != dominators[name]:
                dominators[name] = bits
                changed = True

    headers = set()
    exits = set()
    for source in reachable:
        for target in successors[source]:
            if target not in reachable:
                continue
            if not (dominators[source] & (1 << index[target])):
                continue
            headers.add(target)
            body = {target}
            if source != target:
                body.add(source)
            pending = [source] if source != target else []
            while pending:
                node = pending.pop()
                for predecessor in predecessors[node]:
                    if predecessor in reachable and predecessor not in body:
                        body.add(predecessor)
                        pending.append(predecessor)
            for node in body:
                for successor in successors[node]:
                    if successor in reachable and successor not in body:
                        exits.add(successor)

    headers.discard(entry)  # The entry poll is added separately below.
    exits.discard(entry)
    # The LLVM CFG also contains lowering-only panic/cleanup exits.  Keep only
    # the emitter's own HIR block names; callers select the pattern because C++
    # uses ``b<N>`` and the Wz emitter uses ``b<N>.<label>``.
    return {
        name for name in ({entry} | headers | exits)
        if hir_block.match(name)
    }


def _check_site(function, block, context):
    """Validate every advisory safepoint sequence starting in one block."""
    blocks = function["blocks"]
    lines = blocks[block]
    starts = [index for index, line in enumerate(lines)
              if _SAFEPOINT_RE.match(line)]
    if not starts:
        raise ContractError(f"{context}: missing acquire load of @{SAFEPOINT}")
    for index in starts:
        word = _SAFEPOINT_RE.match(lines[index])
        if index + 2 >= len(lines):
            raise ContractError(f"{context}: truncated safepoint hint branch")
        compare_match = _ICMP_SLE_RE.match(lines[index + 1])
        if (not compare_match or
                compare_match.group("input") != word.group("value")):
            raise ContractError(
                f"{context}: signed <= 0 hint test is not next"
            )
        br_match = _BR_COND_RE.match(lines[index + 2])
        if (not br_match or
                br_match.group("condition") != compare_match.group("value")):
            raise ContractError(
                f"{context}: weighted (!prof) hint branch is not next"
            )
        slow = _unquote(br_match.group(2))
        done = _unquote(br_match.group(3))
        if slow not in blocks or done not in blocks:
            raise ContractError(f"{context}: hint branch targets are missing")
        if slow == done:
            raise ContractError(f"{context}: hint branch has identical targets")
        slow_lines = blocks[slow]
        for line in slow_lines:
            if _ANY_ATOMIC_LOAD_RE.match(line):
                raise ContractError(
                    f"{context}: slow block performs an atomic load; the "
                    "controller poll owns the authoritative re-read"
                )
        if _call_count(slow_lines, SLOW) != 1:
            raise ContractError(
                f"{context}: slow block must call @{SLOW} exactly once"
            )
        if _call_count(slow_lines, WORLD) != 0:
            raise ContractError(
                f"{context}: slow block calls @{WORLD} instead of the controller"
            )
        if _call_count(slow_lines, PRESSURE) != 0:
            raise ContractError(
                f"{context}: slow block calls @{PRESSURE} instead of the "
                "controller"
            )
        slow_br = _only(
            slow_lines, _BR_LABEL_RE, "slow block branch", context
        )[1]
        if _target_name(slow_br.group("target")) != done:
            raise ContractError(
                f"{context}: slow block must continue to the common done block"
            )


def _site_count(function):
    return sum(1 for lines in function["blocks"].values()
               for line in lines if _SAFEPOINT_RE.match(line))


def _collect_aliases(function):
    """SSA names of this function derived from an authoritative word.

    Scoped per function (the same SSA name in another function is unrelated)
    and iterated to a fixpoint, because a legal definition may live in a
    dominating block that the module printer emits after its use.
    """
    lines = [line for block in function["blocks"].values() for line in block]
    aliases = set()
    changed = True
    while changed:
        changed = False
        for line in lines:
            alias = _POINTER_ALIAS_RE.match(line)
            if not alias:
                continue
            sources = _SSA_TOKEN_RE.findall(line.split("=", 1)[1])
            if not (_AUTHORITATIVE_REF_RE.search(line) or
                    any(token in aliases for token in sources)):
                continue
            if alias.group("value") not in aliases:
                aliases.add(alias.group("value"))
                changed = True
    return aliases


def _audit_atomic_loads(functions):
    """Reject authoritative re-reads and non-acquire advisory loads.

    Coverage is bounded to what the emitters can produce: a load of a global
    (any quoting or ordering keyword, including ``monotonic``), or a load
    through a ``getelementptr``/``bitcast``/``addrspacecast`` chain rooted at
    that global.  Atomics on unrelated pointers (weak/ARC counters) are left
    alone; a safepoint's own block is additionally checked by ``_check_site``.
    Aliases and loads are audited per function, after collecting the aliases.
    """
    for name, function in functions.items():
        aliases = _collect_aliases(function)
        for block, lines in function["blocks"].items():
            for line in lines:
                load = _ANY_ATOMIC_LOAD_RE.match(line)
                if not load:
                    continue
                rest = load.group("rest")
                through_alias = any(
                    token in aliases for token in _SSA_TOKEN_RE.findall(rest)
                )
                if _AUTHORITATIVE_REF_RE.search(rest) or through_alias:
                    raise ContractError(
                        f"{name}:{block}: safepoint code loads an "
                        "authoritative world/pressure word; the controller "
                        "poll must own it"
                    )
                if (_SAFEPOINT_REF_RE.search(rest) and
                        not _SAFEPOINT_RE.match(line)):
                    raise ContractError(
                        f"{name}:{block}: advisory word must be loaded "
                        "exactly as volatile i64 acquire, align 8"
                    )


def _poll_blocks(function):
    return {
        name for name, lines in function["blocks"].items()
        if any(_SAFEPOINT_RE.match(line) for line in lines)
    }


def _legacy_poll_blocks(function):
    """Blocks that inline an authoritative word or bypass the controller."""
    return {
        name for name, lines in function["blocks"].items()
        if any(_REQUEST_RE.match(line) or _LIMIT_RE.match(line) or
               _ACCUM_RE.match(line) for line in lines)
        or _call_count(lines, WORLD) == 1
        or _call_count(lines, PRESSURE) == 1
    }


def _check_functions(functions, target_prefix="lang.fn.", target_pattern=None,
                     hir_block=None):
    _audit_atomic_loads(functions)
    total_sites = 0
    checked_functions = 0
    positions_exact = True
    for name, function in functions.items():
        if not function["blocks"]:
            continue
        actual = _poll_blocks(function)
        target = (target_pattern.match(name) if target_pattern is not None
                  else name.startswith(target_prefix))
        if not target and not actual:
            continue
        if not target:
            raise ContractError(
                f"{name}: poll outside the source-function safepoint set"
            )
        expected = _expected_safepoints(function, hir_block)
        checked_functions += 1
        if actual != expected:
            missing = sorted(expected - actual)
            extra = sorted(actual - expected)
            positions_exact = False
            raise ContractError(
                f"{name}: safepoint set changed; missing={missing} extra={extra}"
            )
        for block in sorted(actual):
            _check_site(function, block, f"{name}:{block}")
            total_sites += sum(1 for line in function["blocks"][block]
                               if _SAFEPOINT_RE.match(line))
    if total_sites == 0:
        raise ContractError("no inlined GC safepoint polls")
    return {
        "sites": total_sites,
        "functions": checked_functions,
        "positions_exact": positions_exact,
    }


def _inline_word_loads(functions):
    return sum(
        1
        for function in functions.values()
        for lines in function["blocks"].values()
        for line in lines
        if _REQUEST_RE.match(line) or _LIMIT_RE.match(line) or
        _ACCUM_RE.match(line)
    )


def _direct_poll_calls(functions, symbol):
    return sum(
        _call_count(lines, symbol)
        for function in functions.values()
        for lines in function["blocks"].values()
    )


def check_gc_poll_conditions(llvm: str):
    """Check every inlined poll's exact order without asserting positions."""
    functions = _parse_functions(llvm)
    if not functions:
        raise ContractError("LLVM text defines no functions")
    _audit_atomic_loads(functions)
    total_sites = 0
    for name, function in functions.items():
        for block in sorted(_poll_blocks(function)):
            _check_site(function, block, f"{name}:{block}")
            total_sites += sum(1 for line in function["blocks"][block]
                               if _SAFEPOINT_RE.match(line))
    if total_sites == 0:
        raise ContractError("no inlined GC safepoint polls")
    controller_calls = _direct_poll_calls(functions, SLOW)
    world_calls = _direct_poll_calls(functions, WORLD)
    pressure_calls = _direct_poll_calls(functions, PRESSURE)
    inline_loads = _inline_word_loads(functions)
    if world_calls != 0 or pressure_calls != 0:
        raise ContractError(
            "generated safepoints bypass the controller poll"
        )
    if inline_loads != 0:
        raise ContractError(
            "generated safepoints still load the authoritative request or "
            "pressure words instead of the advisory word"
        )
    if controller_calls != total_sites:
        raise ContractError(
            "controller poll count does not match the safepoint count"
        )
    return {
        "sites": total_sites,
        "functions": len({name for name, function in functions.items()
                          if _poll_blocks(function)}),
        "controller_calls": controller_calls,
        "direct_world_calls": world_calls,
        "direct_pressure_calls": pressure_calls,
        "inline_word_loads": inline_loads,
    }


def check_gc_poll_contract(llvm: str, target_prefix="lang.fn.",
                           target_pattern=None, hir_block=None):
    """Return counts after checking positions and the exact fast-path order."""
    functions = _parse_functions(llvm)
    if not functions:
        raise ContractError("LLVM text defines no functions")
    result = _check_functions(functions, target_prefix, target_pattern,
                              hir_block)
    result["controller_calls"] = _direct_poll_calls(functions, SLOW)
    result["direct_world_calls"] = _direct_poll_calls(functions, WORLD)
    result["direct_pressure_calls"] = _direct_poll_calls(functions, PRESSURE)
    result["inline_word_loads"] = _inline_word_loads(functions)
    if result["direct_world_calls"] != 0:
        raise ContractError(
            f"generated safepoints call @{WORLD} instead of the controller"
        )
    if result["direct_pressure_calls"] != 0:
        raise ContractError(
            f"generated safepoints call @{PRESSURE} instead of the controller"
        )
    if result["inline_word_loads"] != 0:
        raise ContractError(
            "generated safepoints still load the authoritative request or "
            "pressure words instead of the advisory word"
        )
    if result["controller_calls"] != result["sites"]:
        raise ContractError(
            "controller poll count does not match the number of safepoints"
        )
    return result


_SELFTEST_LLVM = '''@lang_gc_safepoint_word_v1 = external global i64
@lang_gc_runtime_pressure_accum_v1 = external global i64
declare void @"lang_gc_runtime_poll_v1"()

define internal i32 @"d.test.function"() {
"b0.0":
  %v0 = load atomic volatile i64, ptr @"lang_gc_safepoint_word_v1" acquire, align 8
  %v1 = icmp sle i64 %v0, 0
  br i1 %v1, label %"gc.poll.slow.1", label %"gc.poll.done.2", !prof !0
"gc.poll.slow.1":
  call void @"lang_gc_runtime_poll_v1"()
  br label %"gc.poll.done.2"
"gc.poll.done.2":
  ret i32 0
}
!0 = !{!"branch_weights", i32 1, i32 1024}
'''


_SELFTEST_SHARED_SSA = '''@lang_gc_safepoint_word_v1 = external global i64
@lang_gc_world_request_word_v1 = external global i32
declare void @"lang_gc_runtime_poll_v1"()
declare void @consume(ptr)

define internal i32 @"d.test.function"() {
"b0.0":
  %v0 = load atomic volatile i64, ptr @"lang_gc_safepoint_word_v1" acquire, align 8
  %v1 = icmp sle i64 %v0, 0
  br i1 %v1, label %"gc.poll.slow.1", label %"gc.poll.done.2", !prof !0
"gc.poll.slow.1":
  call void @"lang_gc_runtime_poll_v1"()
  br label %"gc.poll.done.2"
"gc.poll.done.2":
  %slot = getelementptr i8, ptr @"lang_gc_world_request_word_v1", i64 0
  call void @consume(ptr %slot)
  ret i32 0
}
define internal i32 @"d.other.function"(ptr %arg) {
"e0.0":
  %slot = getelementptr i8, ptr %arg, i64 0
  %value = load atomic i64, ptr %slot monotonic, align 8
  ret i32 0
}
!0 = !{!"branch_weights", i32 1, i32 1024}
'''


def selftest():
    """Focused mutation checks for this contract itself.

    A positive module must pass, and every mutation below must be rejected:
    extra atomic loads (including monotonic), quoted/unquoted spellings of the
    authoritative words, a non-volatile advisory load, a missing ``!prof`` and
    a slow block that bypasses or duplicates the authoritative re-read.
    """
    target = re.compile(r'^d\.test\.function$')
    hir_block = re.compile(r'^b[0-9]+\.[0-9]+$')

    def check(text):
        return check_gc_poll_contract(text, target_pattern=target,
                                      hir_block=hir_block)

    positive = check(_SELFTEST_LLVM)
    if positive["sites"] != 1 or positive["controller_calls"] != 1:
        raise ContractError("selftest: positive sample shape changed")
    # Same SSA name in another function is unrelated; a per-function alias
    # scope must not treat the second function's pointer as authoritative.
    shared = check(_SELFTEST_SHARED_SSA)
    if shared["sites"] != 1 or shared["controller_calls"] != 1:
        raise ContractError("selftest: shared-SSA positive sample shape")
    mutations = {
        "extra monotonic advisory load":
            _SELFTEST_LLVM.replace(
                "  %v1 = icmp sle",
                "  %extra = load atomic i64, ptr "
                "@\"lang_gc_safepoint_word_v1\" monotonic, align 8\n"
                "  %v1 = icmp sle"),
        "advisory load without volatile":
            _SELFTEST_LLVM.replace(
                "load atomic volatile i64",
                "load atomic i64", 1),
        "unquoted authoritative word, monotonic":
            _SELFTEST_LLVM.replace(
                "  call void @\"lang_gc_runtime_poll_v1\"()",
                "  %p = load atomic i64, ptr @lang_gc_world_request_word_v1 "
                "monotonic, align 8\n"
                "  call void @\"lang_gc_runtime_poll_v1\"()"),
        "quoted authoritative word, relaxed":
            _SELFTEST_LLVM.replace(
                "  %v1 = icmp sle",
                "  %a = load atomic i64, ptr "
                "@\"lang_gc_runtime_pressure_accum_v1\" monotonic, align 8\n"
                "  %v1 = icmp sle"),
        "quoted SSA name extra monotonic load":
            _SELFTEST_LLVM.replace(
                "  %v1 = icmp sle",
                "  %\"quoted ssa\" = load atomic i64, ptr "
                "@\"lang_gc_safepoint_word_v1\" monotonic, align 8\n"
                "  %v1 = icmp sle"),
        "tail call to the pressure handler before the controller":
            _SELFTEST_LLVM.replace(
                "  call void @\"lang_gc_runtime_poll_v1\"()",
                "  tail call void "
                "@\"lang_gc_runtime_poll_pressure_v1\"()\n"
                "  call void @\"lang_gc_runtime_poll_v1\"()"),
        "tail call to the world poll in the slow block":
            _SELFTEST_LLVM.replace(
                "  call void @\"lang_gc_runtime_poll_v1\"()",
                "  tail call void @\"lang_gc_world_poll_v1\"()\n"
                "  call void @\"lang_gc_runtime_poll_v1\"()"),
        "GEP alias to an authoritative word":
            _SELFTEST_LLVM.replace(
                "  %v1 = icmp sle",
                "  %ap = getelementptr i8, ptr "
                "@lang_gc_runtime_pressure_accum_v1, i64 0\n"
                "  %av = load atomic i64, ptr %ap monotonic, align 8\n"
                "  %v1 = icmp sle"),
        "authoritative alias defined in a dominating block printed later":
            _SELFTEST_LLVM.replace(
                "  %v1 = icmp sle",
                "  %lv = load atomic i64, ptr %latealias monotonic, align 8\n"
                "  %v1 = icmp sle").replace(
                '"b0.0":',
                '"entry":\n'
                "  br label %\"late.definition\"\n"
                '"b0.0":').replace(
                "  ret i32 0\n}\n",
                "  ret i32 0\n"
                '"late.definition":\n'
                "  %latealias = getelementptr i8, ptr "
                "@lang_gc_runtime_pressure_accum_v1, i64 0\n"
                "  br label %\"b0.0\"\n"
                "}\n"),
        "missing !prof on the hint branch":
            _SELFTEST_LLVM.replace(", !prof !0", ""),
        "slow block bypasses the controller":
            _SELFTEST_LLVM.replace(
                "  call void @\"lang_gc_runtime_poll_v1\"()",
                "  call void @\"lang_gc_world_poll_v1\"()"),
        "slow block re-reads authority as a direct pressure call":
            _SELFTEST_LLVM.replace(
                "  call void @\"lang_gc_runtime_poll_v1\"()",
                "  call void @\"lang_gc_runtime_poll_pressure_v1\"()"),
        "missing safepoint":
            _SELFTEST_LLVM.replace(
                "  %v0 = load atomic volatile i64, ptr "
                "@\"lang_gc_safepoint_word_v1\" acquire, align 8\n"
                "  %v1 = icmp sle i64 %v0, 0\n"
                "  br i1 %v1, label %\"gc.poll.slow.1\", label "
                "%\"gc.poll.done.2\", !prof !0\n"
                "\"gc.poll.slow.1\":\n"
                "  call void @\"lang_gc_runtime_poll_v1\"()\n"
                "  br label %\"gc.poll.done.2\"\n",
                ""),
    }
    for label, text in sorted(mutations.items()):
        if text == _SELFTEST_LLVM:
            raise ContractError(f"selftest: mutation not applied: {label}")
        try:
            check(text)
        except ContractError:
            continue
        raise ContractError(f"selftest: contract accepted {label}")
    return {"positive_sites": positive["sites"],
            "positives_accepted": 2,
            "mutations_rejected": len(mutations)}


def legacy_poll_blocks(llvm: str):
    """Diagnostic helper: blocks that inline or bypass the controller."""
    functions = _parse_functions(llvm)
    return {
        name: _legacy_poll_blocks(function)
        for name, function in functions.items()
        if _legacy_poll_blocks(function)
    }


def expected_safepoints(llvm: str, hir_block=None):
    """Diagnostic/self-test helper: positions for every function."""
    functions = _parse_functions(llvm)
    return {
        name: _expected_safepoints(function, hir_block)
        for name, function in functions.items()
        if function["blocks"]
    }
