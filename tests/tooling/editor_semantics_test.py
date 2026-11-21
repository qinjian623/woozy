#!/usr/bin/env python3
"""Black-box semantic editor checks for the Woozy language server.

The fixture is a real temporary project with a manifest, a UTF-8 source path
containing spaces, and a cross-package import.  All checks go through the same
JSON-RPC stream an editor uses; no compiler or language-server internals are
imported.
"""

import argparse
import json
import os
from pathlib import Path
import re
import select
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from editor_cli_test import Lsp, TIMEOUT  # noqa: E402


MANIFEST = '''schema_version = 1
[package]
name = "editor_semantics"
source_root = "src"
entry = "app"
[build]
backend = "native"
word_bits = 64
'''

MAIN = '''package app;
import "app/support" as support;

func local_scope(seed i32) i32 {
    let count i32 = seed + 1;
    let outside i32 = 3;
    if count > 0 {
        /*😀*/ let count i32 = 7;
        /*😀*/ let total i32 = count;
        return total + outside;
    }
    return count;
}

func other_scope() i32 {
    let other_only i32 = 5;
    return other_only;
}

func current_text() i32 {
    let current_marker i32 = 11;
    return current_marker;
}

func main() i32 {
    let seed i32 = 2;
    let value i64 = support.triple(seed);
    let adjusted i32 = local_scope(seed);
    let limit i32 = support.PUBLIC_LIMIT;
    return adjusted + i32(value) + limit;
}
'''

SUPPORT = '''package support;

pub const PUBLIC_LIMIT i32 = 5;
const PUBLIC_SECRET i32 = 9;
const SECRET_LIMIT i32 = 11;

/*😀*/ pub func triple(value i32) i64 {
    return i64(value * 3);
}

func triple_hidden(value i32) i32 {
    return value + SECRET_LIMIT;
}
'''

INCOMPLETE = MAIN.replace(
    'let adjusted i32 = local_scope(seed);', 'let adjusted i32 = local'
).replace(
    'let value i64 = support.triple(seed);', 'let value i64 = support.tri'
)

TYPE_ERROR = MAIN.replace(
    'let count i32 = seed + 1;', 'let count i32 = "wrong";'
)

CURRENT = MAIN + '\n// current revision 😀\n'

STALE = MAIN.replace(
    'let current_marker i32 = 11;', 'let stale_marker i32 = 11;'
).replace(
    'return current_marker;', 'return stale_marker;'
)

SETTLE_SECONDS = 0.3


class BaselineUnsupported(Exception):
    """The executable predates the semantic editor protocol surface."""


class MethodUnavailable(Exception):
    """A required method was advertised but is still not implemented."""


class RpcFailure(Exception):
    def __init__(self, method, error):
        super().__init__(f'{method}: {error}')
        self.method = method
        self.error = error


def offset_after(text, marker, prefix):
    start = text.index(marker)
    assert text.startswith(prefix, start), (marker, prefix)
    return len(text[:start].encode('utf-8')) + len(prefix.encode('utf-8'))


def utf16_position(text, byte):
    raw = text.encode('utf-8')
    assert 0 <= byte <= len(raw), (byte, len(raw))
    prefix = raw[:byte].decode('utf-8')
    line = prefix.count('\n')
    tail = prefix.rsplit('\n', 1)[-1]
    return {'line': line, 'character': len(tail.encode('utf-16-le')) // 2}


def position_after(text, marker, prefix):
    return utf16_position(text, offset_after(text, marker, prefix))


def span_after(text, marker, prefix, token):
    begin = offset_after(text, marker, prefix)
    end = begin + len(token.encode('utf-8'))
    return {'start': utf16_position(text, begin), 'end': utf16_position(text, end)}


def has_label(labels, name):
    return any(label == name or label.endswith('.' + name) for label in labels)


def completion_labels(result):
    if result is None:
        return []
    if isinstance(result, dict):
        assert 'items' in result, result
        items = result['items']
    else:
        items = result
    assert isinstance(items, list), result
    labels = []
    for item in items:
        if isinstance(item, str):
            labels.append(item)
        elif isinstance(item, dict):
            label = item.get('label')
            assert isinstance(label, str), item
            labels.append(label)
        else:
            raise AssertionError(f'unexpected completion item: {item!r}')
    return labels


def hover_text(result):
    assert isinstance(result, dict), result
    contents = result.get('contents')
    if isinstance(contents, str):
        return contents
    if isinstance(contents, dict):
        value = contents.get('value')
        assert isinstance(value, str), contents
        return value
    if isinstance(contents, list):
        parts = []
        for item in contents:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict) and isinstance(item.get('value'), str):
                parts.append(item['value'])
            else:
                raise AssertionError(f'unexpected hover contents: {item!r}')
        return '\n'.join(parts)
    raise AssertionError(f'unexpected hover result: {result!r}')


def only_location(result):
    assert result is not None, 'definition returned null'
    if isinstance(result, dict):
        locations = [result]
    else:
        assert isinstance(result, list), result
        locations = result
    assert len(locations) == 1, locations
    location = locations[0]
    if 'targetUri' in location:
        uri = location['targetUri']
        span = location.get('targetSelectionRange', location.get('targetRange'))
    else:
        uri = location.get('uri')
        span = location.get('range')
    assert isinstance(uri, str), location
    assert isinstance(span, dict), location
    assert {'start', 'end'} <= set(span), location
    return uri, span


class SemanticLsp(Lsp):
    """The shared Lsp helpers plus notification collection and short polling."""

    def __init__(self, process):
        super().__init__(process)
        self.inbox = []

    def _extract_packet(self):
        if b'\r\n\r\n' not in self.pending:
            return None
        header, _, _ = self.pending.partition(b'\r\n\r\n')
        fields = dict(
            line.lower().split(b':', 1) for line in bytes(header).split(b'\r\n')
        )
        assert b'content-length' in fields, header
        offset = len(header) + 4
        end = offset + int(fields[b'content-length'])
        if len(self.pending) < end:
            return None
        packet = json.loads(self.pending[offset:end])
        del self.pending[:end]
        assert packet['jsonrpc'] == '2.0', packet
        return packet

    def receive_ready(self, timeout):
        deadline = time.monotonic() + timeout
        pipe = self.process.stdout.fileno()
        while True:
            packet = self._extract_packet()
            if packet is not None:
                return packet
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if not select.select([pipe], [], [], remaining)[0]:
                return None
            data = os.read(pipe, 8192)
            assert data, 'LSP terminated before sending a response'
            self.pending.extend(data)

    def request(self, method, params=None):
        self.next_id += 1
        request_id = self.next_id
        self.send(method, params, request_id)
        notifications = []
        while True:
            packet = self.receive()
            if packet.get('id') == request_id and 'method' not in packet:
                if 'error' in packet:
                    error = packet['error']
                    if error.get('code') == -32601:
                        raise MethodUnavailable(f'{method}: {error}')
                    raise RpcFailure(method, error)
                self.inbox.extend(notifications)
                return packet['result']
            notifications.append(packet)

    def take_inbox(self):
        packets = self.inbox
        self.inbox = []
        return packets

    def _diagnostic_batches(self, packets, uri, version):
        batches = []
        for packet in packets:
            if packet.get('method') != 'textDocument/publishDiagnostics':
                continue
            params = packet.get('params', {})
            if params.get('uri') != uri:
                continue
            if version is not None and params.get('version') != version:
                continue
            diagnostics = params.get('diagnostics')
            assert isinstance(diagnostics, list), packet
            batches.append(diagnostics)
        return batches

    def wait_for_diagnostics(self, uri, version, predicate):
        batches = []

        def consume(packets):
            batches.extend(self._diagnostic_batches(packets, uri, version))
            return predicate(batches)

        if consume(self.take_inbox()):
            matched = True
        else:
            matched = False
            while not matched:
                matched = consume([self.receive()])
        while True:
            packet = self.receive_ready(SETTLE_SECONDS)
            if packet is None:
                break
            consume([packet])
        return batches


def expect_empty(batches):
    assert batches, 'server did not publish any diagnostics'
    assert all(not diagnostics for diagnostics in batches), batches


def expect_error(batches):
    assert batches, 'server did not publish any diagnostics'
    errors = [
        diagnostic
        for diagnostics in batches
        for diagnostic in diagnostics
        if diagnostic.get('severity') == 1
    ]
    assert errors, batches


def check_capabilities(capabilities):
    missing = [
        name for name in (
            'hoverProvider', 'definitionProvider', 'completionProvider'
        ) if not capabilities.get(name)
    ]
    if missing:
        raise BaselineUnsupported(
            'missing advertised capabilities: ' + ', '.join(missing)
        )
    assert capabilities.get('positionEncoding') == 'utf-16', capabilities


def check_completion(client, uri, main_text):
    # The first completion request uses a syntactically valid cursor inside a
    # function name, before any intentionally incomplete text is introduced.
    call = position_after(
        main_text, 'let adjusted i32 = local_scope(seed);',
        'let adjusted i32 = local'
    )
    labels = completion_labels(client.request('textDocument/completion', {
        'textDocument': {'uri': uri}, 'position': call,
    }))
    assert has_label(labels, 'local_scope'), labels

    # The parameter and the outer local are visible at their use sites.
    # other_only shares a prefix with outside but belongs to another function,
    # so it must not leak into this scope.
    for marker, prefix, expected in (
            ('let count i32 = seed + 1;', 'let count i32 = see', 'seed'),
            ('return total + outside;', 'return total + out', 'outside')):
        position = position_after(main_text, marker, prefix)
        labels = completion_labels(client.request('textDocument/completion', {
            'textDocument': {'uri': uri}, 'position': position,
        }))
        assert has_label(labels, expected), (expected, labels)
        assert not has_label(labels, 'other_only'), labels

    prefixed = position_after(
        main_text, '/*😀*/ let total i32 = count;',
        '/*😀*/ let total i32 = cou'
    )
    labels = completion_labels(client.request('textDocument/completion', {
        'textDocument': {'uri': uri}, 'position': prefixed,
    }))
    assert has_label(labels, 'count'), labels

    # A cursor inside a valid qualified token tests both the public item and
    # the visibility filter: private siblings share the same prefix.
    qualified = position_after(
        main_text, 'let value i64 = support.triple(seed);',
        'let value i64 = support.tri'
    )
    labels = completion_labels(client.request('textDocument/completion', {
        'textDocument': {'uri': uri}, 'position': qualified,
    }))
    assert has_label(labels, 'triple'), labels
    assert not has_label(labels, 'triple_hidden'), labels

    qualified_constant = position_after(
        main_text, 'let limit i32 = support.PUBLIC_LIMIT;',
        'let limit i32 = support.PUBLIC'
    )
    labels = completion_labels(client.request('textDocument/completion', {
        'textDocument': {'uri': uri}, 'position': qualified_constant,
    }))
    assert has_label(labels, 'PUBLIC_LIMIT'), labels
    assert not has_label(labels, 'PUBLIC_SECRET'), labels


def check_semantics(client, uri, main_text, support_uri, support_text):
    local_hover = position_after(
        main_text, 'return total + outside;', 'return '
    )
    text = hover_text(client.request('textDocument/hover', {
        'textDocument': {'uri': uri}, 'position': local_hover,
    }))
    assert re.search(r'(?<![A-Za-z0-9_])i32(?![A-Za-z0-9_])', text), text
    assert 'total' in text, text

    imported_hover = position_after(
        main_text, 'let value i64 = support.triple(seed);',
        'let value i64 = support.'
    )
    text = hover_text(client.request('textDocument/hover', {
        'textDocument': {'uri': uri}, 'position': imported_hover,
    }))
    assert 'triple' in text, text
    assert re.search(r'(?<![A-Za-z0-9_])i32(?![A-Za-z0-9_])', text), text
    assert re.search(r'(?<![A-Za-z0-9_])i64(?![A-Za-z0-9_])', text), text

    inner_count = span_after(
        main_text, '/*😀*/ let count i32 = 7;', '/*😀*/ let ', 'count'
    )
    inner_use = position_after(
        main_text, '/*😀*/ let total i32 = count;',
        '/*😀*/ let total i32 = '
    )
    definition_uri, definition_range = only_location(
        client.request('textDocument/definition', {
            'textDocument': {'uri': uri}, 'position': inner_use,
        })
    )
    assert definition_uri == uri, (definition_uri, uri)
    assert definition_range == inner_count, (definition_range, inner_count)

    outer_count = span_after(
        main_text, 'let count i32 = seed + 1;', 'let ', 'count'
    )
    outer_use = position_after(
        main_text, 'return count;', 'return '
    )
    definition_uri, definition_range = only_location(
        client.request('textDocument/definition', {
            'textDocument': {'uri': uri}, 'position': outer_use,
        })
    )
    assert definition_uri == uri, (definition_uri, uri)
    assert definition_range == outer_count, (definition_range, outer_count)

    imported_use = position_after(
        main_text, 'let value i64 = support.triple(seed);',
        'let value i64 = support.'
    )
    triple = span_after(
        support_text, '/*😀*/ pub func triple', '/*😀*/ pub func ', 'triple'
    )
    definition_uri, definition_range = only_location(
        client.request('textDocument/definition', {
            'textDocument': {'uri': uri}, 'position': imported_use,
        })
    )
    assert definition_uri == support_uri, (definition_uri, support_uri)
    assert definition_range == triple, (definition_range, triple)


def check(cli, root, main_path, support_path, main_uri, support_uri):
    expected_disk = {
        main_path: main_path.read_bytes(),
        support_path: support_path.read_bytes(),
        root / 'lang.toml': (root / 'lang.toml').read_bytes(),
    }
    with subprocess.Popen(
            [cli, 'lsp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE
    ) as process:
        try:
            client = SemanticLsp(process)
            initialized = client.request('initialize', {
                'processId': None,
                'rootUri': root.as_uri(),
                'workspaceFolders': [{
                    'uri': root.as_uri(), 'name': 'editor semantics',
                }],
                'capabilities': {
                    'general': {'positionEncodings': ['utf-16']},
                    'textDocument': {
                        'completion': {
                            'completionItem': {'snippetSupport': False},
                        },
                    },
                },
            })
            check_capabilities(initialized['capabilities'])
            client.send('initialized', {})

            document = {'uri': main_uri}
            client.send('textDocument/didOpen', {
                'textDocument': {
                    **document, 'languageId': 'woozy', 'version': 1,
                    'text': MAIN,
                },
            })
            expect_empty(client.wait_for_diagnostics(
                main_uri, 1, lambda batches: any(not b for b in batches)
            ))

            check_completion(client, main_uri, MAIN)
            check_semantics(client, main_uri, MAIN, support_uri, SUPPORT)

            def change(version, text):
                client.send('textDocument/didChange', {
                    'textDocument': {**document, 'version': version},
                    'contentChanges': [{'text': text}],
                })

            # Intentionally incomplete edits may rely on a narrower recovery
            # path, so only the relevant prefix completion is required here.
            change(2, INCOMPLETE)
            client.wait_for_diagnostics(
                main_uri, 2, lambda batches: any(b for b in batches)
            )
            for marker, prefix, expected in (
                    ('let adjusted i32 = local',
                     'let adjusted i32 = local', 'local_scope'),
                    ('let value i64 = support.tri',
                     'let value i64 = support.tri', 'triple')):
                position = position_after(INCOMPLETE, marker, prefix)
                labels = completion_labels(client.request(
                    'textDocument/completion',
                    {'textDocument': {'uri': main_uri},
                     'position': position}
                ))
                assert has_label(labels, expected), (expected, labels)
            assert main_path.read_bytes() == expected_disk[main_path]

            change(3, MAIN)
            expect_empty(client.wait_for_diagnostics(
                main_uri, 3, lambda batches: any(not b for b in batches)
            ))

            change(4, TYPE_ERROR)
            expect_error(client.wait_for_diagnostics(
                main_uri, 4, lambda batches: any(b for b in batches)
            ))
            assert main_path.read_bytes() == expected_disk[main_path]

            change(5, MAIN)
            expect_empty(client.wait_for_diagnostics(
                main_uri, 5, lambda batches: any(not b for b in batches)
            ))

            change(10, CURRENT)
            expect_empty(client.wait_for_diagnostics(
                main_uri, 10, lambda batches: any(not b for b in batches)
            ))

            # Version 9 was never used by the current revision.  A stale
            # replacement must neither replace the buffer nor publish a result.
            client.take_inbox()
            change(9, STALE)
            marker_use = position_after(
                MAIN, 'return current_marker;', 'return '
            )
            marker_decl = span_after(
                MAIN, 'let current_marker i32 = 11;', 'let ',
                'current_marker'
            )
            definition_uri, definition_range = only_location(
                client.request('textDocument/definition', {
                    'textDocument': {'uri': main_uri},
                    'position': marker_use,
                })
            )
            stale_diagnostics = client._diagnostic_batches(
                client.take_inbox(), main_uri, 9
            )
            assert not stale_diagnostics, stale_diagnostics
            assert definition_uri == main_uri, (definition_uri, main_uri)
            assert definition_range == marker_decl, (
                definition_range, marker_decl
            )

            assert main_path.read_bytes() == expected_disk[main_path]
            assert support_path.read_bytes() == expected_disk[support_path]

            check_symbol_edges(client, main_uri)

            client.take_inbox()
            client.send('textDocument/didClose', {'textDocument': document})
            expect_empty(client.wait_for_diagnostics(
                main_uri, None, lambda batches: any(not b for b in batches)
            ))
            assert main_path.read_bytes() == expected_disk[main_path]

            assert client.request('shutdown') is None
            client.send('exit')
            assert process.wait(timeout=TIMEOUT) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=TIMEOUT)



def check_symbol_edges(client, uri):
    document = {'uri': uri}

    def change(version, source, invalid=False):
        client.send('textDocument/didChange', {
            'textDocument': {**document, 'version': version},
            'contentChanges': [{'text': source}],
        })
        client.wait_for_diagnostics(
            uri, version, lambda batches: any(bool(batch) == invalid for batch in batches)
        )

    def query(method, source, marker, prefix):
        return client.request('textDocument/' + method, {
            'textDocument': document,
            'position': position_after(source, marker, prefix),
        })

    generic = """package app;
struct Box { pub field i32; }
func id[T](value T) T { return value; }
impl Box {
    func get[T](self Self, value T) T { return value; }
}
func main() i32 {
    let box = Box(field: id[i32](1));
    let number = box.get[i32](1);
    return box.field + number;
}
"""
    change(11, generic)
    hover = hover_text(query('hover', generic, 'return box.field', 'return b'))
    assert 'Box' in hover, hover
    assert only_location(query('definition', generic, 'box.field', 'box.f')) == (
        uri, span_after(generic, 'pub field i32', 'pub ', 'field')
    )
    method_hover = hover_text(query('hover', generic, 'box.get[i32]', 'box.g'))
    assert 'get' in method_hover and 'i32' in method_hover, method_hover
    assert only_location(query('definition', generic, 'box.get[i32]', 'box.g')) == (
        uri, span_after(generic, 'func get[T]', 'func ', 'get')
    )

    before = """package app;
struct Box { pub field i32; }
func old_name() {}
func main() {
    let Box = 1;
    let v = Box;
}
"""
    after = before.replace('func old_name() {}\n', '').replace(
        '    let v = Box;', '    let v = Box;\n    old_;\n    v.;'
    )
    change(12, before)
    change(13, after, invalid=True)
    assert 'old_name' not in completion_labels(query('completion', after, 'old_;', 'old_'))
    assert 'field' not in completion_labels(query('completion', after, 'v.;', 'v.'))

    shadowed_later = """package app;
struct Box { pub field i32; }
func main() i32 {
    let v = Box(field: 1);
    let Box = 1;
    return v.field;
}
"""
    change(14, shadowed_later)
    assert 'field' in completion_labels(query('completion', shadowed_later, 'v.field', 'v.f'))

    generic_member = """package app;
struct T { pub wrong i32; }
struct Box[T] { pub value T; }
func main() i32 {
    let box = Box[i32](value: 1);
    return box.value;
}
"""
    change(15, generic_member)
    incomplete = generic_member.replace('return box.value;', 'return box.value.;')
    change(16, incomplete, invalid=True)
    assert 'wrong' not in completion_labels(query('completion', incomplete, 'box.value.;', 'box.value.'))


def check_cross_manifest(cli):
    with tempfile.TemporaryDirectory(prefix='woozy-cross-manifest-') as directory:
        root = Path(directory)
        dependency = root / 'vendor/support'
        (root / 'src').mkdir()
        (dependency / 'src').mkdir(parents=True)
        (root / 'lang.toml').write_text(
            MANIFEST + '\n[dependencies.supportdep]\nkind = "source"\npath = "vendor/support"\n',
            encoding='utf-8',
        )
        (dependency / 'lang.toml').write_text(MANIFEST.replace('editor_semantics', 'support'))
        main = root / 'src/main.wz'
        support = dependency / 'src/module.wz'
        source = MAIN.replace('import "app/support"', 'import "supportdep"')
        main.write_text(source, encoding='utf-8')
        support.write_text(SUPPORT, encoding='utf-8')
        main_uri = main.as_uri()
        alias = root / 'dependency link.wz'
        alias.symlink_to(support)
        support_uri = alias.as_uri()

        with subprocess.Popen([cli, 'lsp'], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            try:
                client = SemanticLsp(process)
                client.request('initialize', {'rootUri': root.as_uri(), 'capabilities': {}})
                client.send('initialized', {})

                def open_buffer(uri, value):
                    client.send('textDocument/didOpen', {'textDocument': {
                        'uri': uri, 'languageId': 'woozy', 'version': 1, 'text': value,
                    }})

                open_buffer(main_uri, source)
                expect_empty(client.wait_for_diagnostics(
                    main_uri, 1, lambda batches: any(not batch for batch in batches)
                ))
                overlay = SUPPORT.replace('triple(value i32)', 'triple(value bool)').replace(
                    'return i64(value * 3);', 'return 12;'
                )
                open_buffer(support_uri, overlay)
                expect_error(client.wait_for_diagnostics(main_uri, 1, any))

                client.send('textDocument/didClose', {'textDocument': {'uri': support_uri}})
                expect_empty(client.wait_for_diagnostics(
                    main_uri, 1, lambda batches: any(not batch for batch in batches)
                ))
                assert main.read_text(encoding='utf-8') == source
                assert support.read_text(encoding='utf-8') == SUPPORT
                assert client.request('shutdown') is None
                client.send('exit')
                assert process.wait(timeout=TIMEOUT) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=TIMEOUT)


def run(cli):
    with tempfile.TemporaryDirectory(prefix='woozy-semantics-') as directory:
        root = Path(directory) / '项目 root'
        source_root = root / 'src'
        support_root = source_root / 'support'
        support_root.mkdir(parents=True)
        manifest = root / 'lang.toml'
        main_path = source_root / 'main 文件 😀.wz'
        support_path = support_root / 'support.wz'
        manifest.write_text(MANIFEST, encoding='utf-8')
        main_path.write_text(MAIN, encoding='utf-8')
        support_path.write_text(SUPPORT, encoding='utf-8')
        check(
            cli, root, main_path, support_path,
            # Unopened targets use canonical paths; open files retain their client URI.
            main_path.as_uri(), support_path.resolve(strict=True).as_uri()
        )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True, type=Path)
    cli = parser.parse_args().cli.resolve(strict=True)
    try:
        run(cli)
        check_cross_manifest(cli)
    except BaselineUnsupported as error:
        print(f'BASELINE FAILURE (old binary): {error}', file=sys.stderr)
        return 2
    except MethodUnavailable as error:
        print(
            f'IMPLEMENTATION UNAVAILABLE (advertised but absent): {error}',
            file=sys.stderr,
        )
        return 3
    except RpcFailure as error:
        print(f'IMPLEMENTATION FAILURE (JSON-RPC): {error}', file=sys.stderr)
        return 1
    except AssertionError as error:
        print(f'IMPLEMENTATION FAILURE: {error}', file=sys.stderr)
        return 1
    print('Woozy editor semantic checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
