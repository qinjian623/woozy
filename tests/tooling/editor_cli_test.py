import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time


SOURCE = b'package app;\nfunc main() i32 {\n return 0;\n}\n'
COMMENT = '// keep this comment 😀'.encode()
RAW_LITERAL = '`first line\n  中文 😀\nlast line`'.encode()
UNFORMATTED = SOURCE.replace(
    b' return 0;', b' ' + COMMENT + b'\n let value = ' + RAW_LITERAL + b';\n return 0;'
)
PREFIX = '    let label = "😀"; '
INVALID = SOURCE.decode().replace(' return 0;', PREFIX + '@\n    return 0;')
TIMEOUT = 10


def run(cli, *args, source=None, expected=0):
    result = subprocess.run(
        [cli, *args], input=source, capture_output=True, timeout=TIMEOUT
    )
    assert result.returncode == expected, (args, result)
    return result


def check_cli(cli, path):
    path.write_bytes(UNFORMATTED)
    assert json.loads(run(cli, 'check', '--syntax-only', '--format', 'json', path).stdout)[
        'status'
    ] == 'ok'
    formatted = run(cli, 'fmt', path).stdout
    assert formatted != UNFORMATTED
    assert b'\n    return 0;\n' in formatted, formatted
    assert COMMENT in formatted and RAW_LITERAL in formatted, formatted
    assert run(cli, 'fmt', '-', source=UNFORMATTED).stdout == formatted
    assert run(cli, 'fmt', '-', source=formatted).stdout == formatted
    windows_source = b'\xef\xbb\xbf' + UNFORMATTED.replace(b'\n', b'\r\n')
    windows_formatted = b'\xef\xbb\xbf' + formatted.replace(b'\n', b'\r\n')
    assert run(cli, 'fmt', '-', source=windows_source).stdout == windows_formatted
    assert run(cli, 'fmt', '--check', '-', source=windows_formatted).stdout == b''

    mixed_source = b'\r\n/*a\nb*/' + UNFORMATTED
    mixed_formatted = run(cli, 'fmt', '-', source=mixed_source).stdout
    assert b'/*a\nb*/' in mixed_formatted
    assert run(cli, 'fmt', '--check', '-', source=mixed_formatted).stdout == b''
    assert run(cli, 'fmt', '--check', path, expected=1).stdout == b''
    assert path.read_bytes() == UNFORMATTED

    path.write_bytes(formatted)
    assert run(cli, 'fmt', '--check', path).stdout == b''
    assert path.read_bytes() == formatted

    path.write_text(INVALID, encoding='utf-8')
    failure = run(cli, 'fmt', path, expected=1)
    assert failure.stderr and not failure.stdout
    assert path.read_text(encoding='utf-8') == INVALID
    diagnostics = json.loads(run(
        cli, 'check', '--syntax-only', '--format', 'json', path, expected=1
    ).stdout)
    assert diagnostics['status'] == 'error' and diagnostics['diagnostics']
    return formatted.decode()


class Lsp:
    def __init__(self, process):
        self.process = process
        self.pending = bytearray()
        self.next_id = 0

    def send(self, method, params=None, request_id=None):
        packet = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            packet['params'] = params
        if request_id is not None:
            packet['id'] = request_id
        body = json.dumps(packet, ensure_ascii=False).encode()
        self.process.stdin.write(f'Content-Length: {len(body)}\r\n\r\n'.encode() + body)
        self.process.stdin.flush()

    def receive(self):
        deadline = time.monotonic() + TIMEOUT
        pipe = self.process.stdout.fileno()

        def fill():
            remaining = deadline - time.monotonic()
            assert remaining > 0 and select.select([pipe], [], [], remaining)[0], 'LSP timed out'
            data = os.read(pipe, 8192)
            assert data, 'LSP terminated before sending its response'
            self.pending.extend(data)

        while b'\r\n\r\n' not in self.pending:
            fill()
        header, _, _ = self.pending.partition(b'\r\n\r\n')
        fields = dict(line.lower().split(b':', 1) for line in bytes(header).split(b'\r\n'))
        offset = len(header) + 4
        end = offset + int(fields[b'content-length'])
        while len(self.pending) < end:
            fill()
        packet = json.loads(self.pending[offset:end])
        del self.pending[:end]
        assert packet['jsonrpc'] == '2.0', packet
        return packet

    def request(self, method, params=None):
        self.next_id += 1
        self.send(method, params, self.next_id)
        packet = self.receive()
        assert packet.get('id') == self.next_id and 'error' not in packet, packet
        return packet['result']

    def diagnostics(self, uri, version=None):
        packet = self.receive()
        assert packet.get('method') == 'textDocument/publishDiagnostics', packet
        params = packet['params']
        assert params['uri'] == uri, packet
        if version is not None:
            assert params['version'] == version, packet
        return params['diagnostics']


def check_lsp(cli, path, formatted):
    path.write_bytes(UNFORMATTED)
    uri = path.as_uri()
    document = {'uri': uri}
    format_params = {
        'textDocument': document,
        'options': {'tabSize': 4, 'insertSpaces': True},
    }
    with subprocess.Popen([cli, 'lsp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE) as process:
        try:
            client = Lsp(process)
            capabilities = client.request('initialize', {
                'processId': None,
                'rootUri': path.parent.as_uri(),
                'capabilities': {'general': {'positionEncodings': ['utf-16']}},
            })['capabilities']
            assert capabilities['positionEncoding'] == 'utf-16', capabilities
            sync = capabilities['textDocumentSync']
            assert sync == 1 or sync['change'] == 1, capabilities
            assert capabilities['documentFormattingProvider'] is True, capabilities
            client.send('initialized', {})

            client.send('textDocument/didOpen', {
                'textDocument': {**document, 'languageId': 'woozy', 'version': 1, 'text': INVALID},
            })
            errors = client.diagnostics(uri, 1)
            character = len(PREFIX.encode('utf-16-le')) // 2
            expected_range = {
                'start': {'line': 2, 'character': character},
                'end': {'line': 2, 'character': character + 1},
            }
            assert any(d['range'] == expected_range and d['severity'] == 1 for d in errors), errors
            assert client.request('textDocument/formatting', format_params) == []

            def change(version, source):
                client.send('textDocument/didChange', {
                    'textDocument': {**document, 'version': version},
                    'contentChanges': [{'text': source}],
                })

            change(2, UNFORMATTED.decode())
            assert client.diagnostics(uri, 2) == []
            change(1, INVALID)
            assert client.request('textDocument/formatting', format_params) == [{
                'range': {
                    'start': {'line': 0, 'character': 0},
                    'end': {'line': UNFORMATTED.count(b'\n'), 'character': 0},
                },
                'newText': formatted,
            }]
            change(3, formatted)
            assert client.diagnostics(uri, 3) == []
            assert client.request('textDocument/formatting', format_params) == []

            # Full text sync also sends the entire buffer after a one-character edit.
            large_source = ('// ' + 'a' * 120 + '😀\n') * 1024
            change(4, large_source + formatted)
            assert client.diagnostics(uri, 4) == []
            change(5, '@' * 32768)
            many_errors = client.diagnostics(uri, 5)
            assert many_errors[0]['code'] == 'E_LEX_INVALID_TOKEN'
            assert many_errors[-1]['range']['end'] == {'line': 0, 'character': 32768}
            client.send('textDocument/didClose', {'textDocument': document})
            assert client.diagnostics(uri) == []
            assert path.read_bytes() == UNFORMATTED
            assert client.request('shutdown') is None
            client.send('exit')
            assert process.wait(timeout=TIMEOUT) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=TIMEOUT)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cli', required=True, type=Path)
    cli = parser.parse_args().cli.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='woozy-editor-') as directory:
        path = Path(directory) / '测试 file.wz'
        formatted = check_cli(cli, path)
        check_lsp(cli, path, formatted)
    print('Woozy editor CLI checks passed')


if __name__ == '__main__':
    main()
