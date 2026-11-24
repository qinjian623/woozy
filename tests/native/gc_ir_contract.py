"""IR contract for the Native automatic-GC safepoint poll.

The backend inlines the unrequested fast path at every original safepoint:
function entry and natural-loop headers.  The slow controller entry point is
still available, but a generated program must not call it from a safepoint.

This module is test-only.  It parses the pre-optimization LLVM text emitted by
``--out-llvm-before``; it does not use LLVM Python bindings.
"""
from __future__ import annotations

import re
from collections import OrderedDict

REQUEST = "lang_gc_world_request_word_v1"
LIMIT = "lang_gc_runtime_pressure_limit_v1"
ACCUM = "lang_gc_runtime_pressure_accum_v1"
WORLD = "lang_gc_world_poll_v1"
PRESSURE = "lang_gc_runtime_poll_pressure_v1"
LEGACY = "lang_gc_runtime_poll_v1"

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
_AND_RE = re.compile(
    r'^(?P<value>' + _VALUE + r')\s*=\s*and i32 (?P<input>' +
    _VALUE + r'), 1$'
)
_ICMP_NE_RE = re.compile(
    r'^(?P<value>' + _VALUE + r')\s*=\s*icmp ne i32 (?P<input>' +
    _VALUE + r'), 0$'
)
_ICMP_UGE_RE = re.compile(
    r'^(?P<value>' + _VALUE + r')\s*=\s*icmp uge i64 (?P<left>' +
    _VALUE + r'), (?P<right>' + _VALUE + r')$'
)
_BR_COND_RE = re.compile(
    r'^br i1 (?P<condition>' + _VALUE + r'), label ' + _TARGET +
    r', label ' + _TARGET + r'(?:, !prof ![0-9]+)?$'
)
_BR_LABEL_RE = re.compile(r'^br label (?P<target>' + _TARGET + r')$')
_CALL_RE = re.compile(r'^call void @(?P<name>[^\s(]+)\(\)$')


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
    blocks = function["blocks"]
    lines = blocks[block]
    request_index, request = _first(
        lines, _REQUEST_RE, f"acquire load of @{REQUEST}", context
    )
    if request_index + 3 >= len(lines):
        raise ContractError(f"{context}: truncated request branch")
    and_match = _AND_RE.match(lines[request_index + 1])
    if not and_match or and_match.group("input") != request.group("value"):
        raise ContractError(f"{context}: request mask is not the next operation")
    ne_match = _ICMP_NE_RE.match(lines[request_index + 2])
    if not ne_match or ne_match.group("input") != and_match.group("value"):
        raise ContractError(f"{context}: request comparison is not next")
    br_match = _BR_COND_RE.match(lines[request_index + 3])
    if not br_match or br_match.group("condition") != ne_match.group("value"):
        raise ContractError(f"{context}: request branch is not next")
    world = _unquote(br_match.group(2))
    pressure = _unquote(br_match.group(3))
    if world not in blocks or pressure not in blocks:
        raise ContractError(f"{context}: request branch targets are missing")
    if _call_count(blocks[world], WORLD) != 1:
        raise ContractError(
            f"{context}: world-request block must call @{WORLD} exactly once"
        )
    if _call_count(blocks[world], PRESSURE) != 0:
        raise ContractError(f"{context}: world-request block calls the optimizer")
    world_br = _only(
        blocks[world], _BR_LABEL_RE, "world-request block branch", context
    )[1]
    if _target_name(world_br.group("target")) != pressure:
        raise ContractError(
            f"{context}: world-request block must continue to the pressure check"
        )

    pressure_lines = blocks[pressure]
    limit_index, limit = _first(
        pressure_lines, _LIMIT_RE, f"acquire load of @{LIMIT}", context
    )
    accum_index, accum = _first(
        pressure_lines, _ACCUM_RE, f"acquire load of @{ACCUM}", context
    )
    if limit_index >= accum_index:
        raise ContractError(
            f"{context}: pressure limit must be loaded before the accumulator"
        )
    compare_index, compare = _first(
        pressure_lines, _ICMP_UGE_RE, "unsigned pressure comparison", context
    )
    if compare_index != accum_index + 1:
        raise ContractError(
            f"{context}: pressure comparison must immediately follow both loads"
        )
    if compare.group("left") != accum.group("value"):
        raise ContractError(
            f"{context}: pressure comparison must use accum as the left operand"
        )
    if compare.group("right") != limit.group("value"):
        raise ContractError(
            f"{context}: pressure comparison must use limit as the right operand"
        )
    pressure_br_index = compare_index + 1
    if pressure_br_index >= len(pressure_lines):
        raise ContractError(f"{context}: missing pressure conditional branch")
    pressure_br = _BR_COND_RE.match(pressure_lines[pressure_br_index])
    if not pressure_br or pressure_br.group("condition") != compare.group("value"):
        raise ContractError(
            f"{context}: pressure branch reads the wrong condition"
        )
    collect = _unquote(pressure_br.group(2))
    done = _unquote(pressure_br.group(3))
    if collect not in blocks or done not in blocks:
        raise ContractError(f"{context}: pressure branch targets are missing")

    collect_lines = blocks[collect]
    if _call_count(collect_lines, PRESSURE) != 1:
        raise ContractError(
            f"{context}: pressure block must call @{PRESSURE} exactly once"
        )
    if _call_count(collect_lines, WORLD) != 0:
        raise ContractError(f"{context}: pressure block repeats the world poll")
    collect_br = _only(
        collect_lines, _BR_LABEL_RE, "pressure block branch", context
    )[1]
    if _target_name(collect_br.group("target")) != done:
        raise ContractError(
            f"{context}: pressure block must continue to the common done block"
        )


def _poll_blocks(function):
    return {
        name for name, lines in function["blocks"].items()
        if any(_REQUEST_RE.match(line) for line in lines)
    }


def _legacy_poll_blocks(function):
    return {
        name for name, lines in function["blocks"].items()
        if _call_count(lines, LEGACY) == 1
    }


def _check_functions(functions, target_prefix="lang.fn.", target_pattern=None,
                     hir_block=None):
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
        total_sites += len(actual)
        if actual != expected:
            missing = sorted(expected - actual)
            extra = sorted(actual - expected)
            positions_exact = False
            raise ContractError(
                f"{name}: safepoint set changed; missing={missing} extra={extra}"
            )
        for block in sorted(actual):
            _check_site(function, block, f"{name}:{block}")
    if total_sites == 0:
        raise ContractError("no inlined GC safepoint polls")
    return {
        "sites": total_sites,
        "functions": checked_functions,
        "positions_exact": positions_exact,
    }


def check_gc_poll_conditions(llvm: str):
    """Check every inlined poll's exact order without asserting positions."""
    functions = _parse_functions(llvm)
    if not functions:
        raise ContractError("LLVM text defines no functions")
    total_sites = 0
    for name, function in functions.items():
        for block in sorted(_poll_blocks(function)):
            _check_site(function, block, f"{name}:{block}")
            total_sites += 1
    if total_sites == 0:
        raise ContractError("no inlined GC safepoint polls")
    legacy_calls = sum(
        _call_count(lines, LEGACY)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    world_calls = sum(
        _call_count(lines, WORLD)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    pressure_calls = sum(
        _call_count(lines, PRESSURE)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    if legacy_calls != 0:
        raise ContractError(
            f"generated safepoints still call the legacy @{LEGACY} entry point"
        )
    if world_calls != total_sites or pressure_calls != total_sites:
        raise ContractError("slow poll counts do not match the safepoint count")
    return {
        "sites": total_sites,
        "functions": len({name for name, function in functions.items()
                          if _poll_blocks(function)}),
        "legacy_calls": legacy_calls,
        "world_calls": world_calls,
        "pressure_calls": pressure_calls,
    }


def check_gc_poll_contract(llvm: str, target_prefix="lang.fn.",
                           target_pattern=None, hir_block=None):
    """Return counts after checking positions and the exact fast-path order."""
    functions = _parse_functions(llvm)
    if not functions:
        raise ContractError("LLVM text defines no functions")
    result = _check_functions(functions, target_prefix, target_pattern,
                              hir_block)
    result["legacy_calls"] = sum(
        _call_count(lines, LEGACY)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    result["world_calls"] = sum(
        _call_count(lines, WORLD)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    result["pressure_calls"] = sum(
        _call_count(lines, PRESSURE)
        for function in functions.values()
        for lines in function["blocks"].values()
    )
    if result["legacy_calls"] != 0:
        raise ContractError(
            f"generated safepoints still call the legacy @{LEGACY} entry point"
        )
    if result["world_calls"] != result["sites"]:
        raise ContractError(
            "world poll count does not match the number of safepoints"
        )
    if result["pressure_calls"] != result["sites"]:
        raise ContractError(
            "pressure handler count does not match the number of safepoints"
        )
    return result


def legacy_poll_blocks(llvm: str):
    """Diagnostic helper: old poll-call blocks for position comparisons."""
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
