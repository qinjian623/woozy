#!/usr/bin/env python3
"""Build and exercise the real Native chat programs on owned loopback sockets."""
import argparse
from collections import deque
import json
import os
from pathlib import Path
import select
import selectors
import signal
import socket
import subprocess
import time


MAX_LINE = 16384
MAX_FRAME = 16640
TEMPORARY_ACCEPT_ERRORS = (103, 100, 71, 92, 112, 64, 113, 95, 101)


def short(value):
    text = repr(value)
    return text if len(text) <= 240 else text[:240] + '...'


class Endpoint:
    def __init__(self, name, reader, writer, log, process=None, connection=None):
        self.name, self.reader, self.writer = name, reader, writer
        self.log, self.process, self.connection = log, process, connection
        self.pending, self.lines, self.eof = bytearray(), deque(), False
        self.history = deque(maxlen=8)


class Chat:
    def __init__(self, directory):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True)
        self.selector = selectors.DefaultSelector()
        self.endpoints, self.processes, self.logs = [], [], []
        self.serial = 0

    def endpoint(self, name, reader, writer, process=None, connection=None):
        self.serial += 1
        log = open(self.directory / f'{self.serial:02d}-{name}.stdout', 'wb')
        self.logs.append(log)
        endpoint = Endpoint(name, reader, writer, log, process, connection)
        os.set_blocking(reader, False)
        if writer is not None:
            os.set_blocking(writer, False)
        self.selector.register(reader, selectors.EVENT_READ, endpoint)
        self.endpoints.append(endpoint)
        return endpoint

    def start(self, program, arguments, name, stdin=True):
        error_path = self.directory / f'{self.serial + 1:02d}-{name}.stderr'
        errors = open(error_path, 'wb')
        self.logs.append(errors)
        argv = [str(program), *map(str, arguments)]
        process = subprocess.Popen(argv, stdin=subprocess.PIPE if stdin else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=errors, bufsize=0)
        self.processes.append((process, argv, error_path))
        return self.endpoint(name, process.stdout.fileno(),
                             process.stdin.fileno() if stdin else None, process=process)

    def raw(self, port, name, nickname=None, receive_buffer=None):
        connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            if receive_buffer is not None:
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.settimeout(3)
            connection.connect(('127.0.0.1', port))
            endpoint = self.endpoint(name, connection.fileno(), connection.fileno(),
                                     connection=connection)
        except BaseException:
            connection.close()
            raise
        if nickname is not None:
            self.send(endpoint, nickname.encode('utf-8') + b'\n')
        return endpoint

    def pause_reading(self, endpoint):
        self.selector.unregister(endpoint.reader)

    def poll(self, timeout):
        for key, _ in self.selector.select(max(0, timeout)):
            endpoint = key.data
            for _ in range(4):
                try:
                    data = os.read(endpoint.reader, 65536)
                except BlockingIOError:
                    break
                except ConnectionResetError:
                    data = b''
                if not data:
                    endpoint.eof = True
                    self.selector.unregister(endpoint.reader)
                    assert not endpoint.pending, (endpoint.name, 'unterminated output',
                                                  short(bytes(endpoint.pending)))
                    break
                endpoint.log.write(data)
                endpoint.pending.extend(data)
                while True:
                    end = endpoint.pending.find(b'\n')
                    if end < 0:
                        break
                    line = bytes(endpoint.pending[:end]).removesuffix(b'\r')
                    del endpoint.pending[:end + 1]
                    assert len(line) <= MAX_FRAME, (endpoint.name, 'oversized server output')
                    line.decode('utf-8')
                    endpoint.lines.append(line)
                    endpoint.history.append(short(line))
                assert len(endpoint.pending) <= MAX_FRAME, (endpoint.name, 'unbounded output frame')

    def line(self, endpoint, timeout=4):
        deadline = time.monotonic() + timeout
        while not endpoint.lines:
            assert not endpoint.eof, (endpoint.name, 'unexpected EOF', list(endpoint.history))
            remaining = deadline - time.monotonic()
            assert remaining > 0, (endpoint.name, 'line timeout', list(endpoint.history))
            self.poll(min(remaining, .05))
        return endpoint.lines.popleft()

    def expect(self, endpoint, line, timeout=4):
        actual = self.line(endpoint, timeout)
        expected = line.encode('utf-8') if isinstance(line, str) else line
        assert actual == expected, (endpoint.name, short(actual), short(expected))

    def send(self, endpoint, data, timeout=4, allow_closed=False):
        deadline = time.monotonic() + timeout
        sent = 0
        while sent < len(data):
            try:
                count = os.write(endpoint.writer, data[sent:])
                assert count > 0, (endpoint.name, 'zero write')
                sent += count
            except BlockingIOError:
                remaining = deadline - time.monotonic()
                assert remaining > 0, (endpoint.name, 'write timeout', sent, len(data))
                select.select([], [endpoint.writer], [], min(remaining, .02))
            except (BrokenPipeError, ConnectionResetError):
                if allow_closed:
                    return sent
                raise
            self.poll(0)
        return sent

    def quiet(self, endpoints, duration=.12):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.poll(min(.02, deadline - time.monotonic()))
            for endpoint in endpoints:
                assert not endpoint.lines, (endpoint.name, 'unexpected line',
                                             short(endpoint.lines[0]))

    def closed(self, endpoint, timeout=4):
        deadline = time.monotonic() + timeout
        lines = []
        while not endpoint.eof:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (endpoint.name, 'connection did not close')
            self.poll(min(.05, remaining))
            while endpoint.lines:
                lines.append(endpoint.lines.popleft())
        while endpoint.lines:
            lines.append(endpoint.lines.popleft())
        return lines

    def exit(self, endpoint, expected=0, timeout=4):
        deadline = time.monotonic() + timeout
        while endpoint.process.poll() is None:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (endpoint.name, 'process did not exit with stdin still open')
            self.poll(min(.05, remaining))
        self.poll(0)
        assert endpoint.process.returncode == expected, (
            endpoint.name, endpoint.process.returncode, expected)
        return endpoint.process.returncode

    def close_peer(self, endpoint):
        try:
            self.selector.unregister(endpoint.reader)
        except KeyError:
            pass
        endpoint.connection.close()
        endpoint.eof = True

    def stop(self, endpoint):
        if endpoint.process.poll() is None:
            endpoint.process.terminate()
            endpoint.process.wait(timeout=2)
        self.poll(0)

    def cleanup(self):
        for process, _, _ in self.processes:
            if process.poll() is None:
                process.terminate()
        for process, _, _ in self.processes:
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
            if process.stdin:
                process.stdin.close()
            process.stdout.close()
        for endpoint in self.endpoints:
            if endpoint.connection is not None:
                endpoint.connection.close()
        self.selector.close()
        for log in self.logs:
            log.close()

    def summary(self):
        return [dict(argv=argv, pid=process.pid, exit=process.returncode,
                     stderr=str(errors)) for process, argv, errors in self.processes]


def broadcast(chat, peers, line):
    for peer in peers:
        chat.expect(peer, line)


def join_raw(chat, port, nickname, peers, label):
    peer = chat.raw(port, label, nickname)
    broadcast(chat, [*peers, peer], f'* {nickname} joined')
    return peer


def rejected(chat, port, peers, label, frame, nickname=None):
    peer = chat.raw(port, label)
    if nickname is not None:
        chat.send(peer, nickname.encode() + b'\n')
        broadcast(chat, [*peers, peer], f'* {nickname} joined')
    chat.send(peer, frame, allow_closed=True)
    errors = chat.closed(peer)
    assert all(line.startswith(b'ERROR ') for line in errors), (label, errors)
    if nickname is not None:
        broadcast(chat, peers, f'* {nickname} left')
    chat.quiet(peers)
    chat.close_peer(peer)
    return dict(error_lines=[line.decode() for line in errors])


def start_server(chat, program):
    server = chat.start(program, ['127.0.0.1', '0'], 'server', stdin=False)
    banner = chat.line(server).decode().split(' ')
    assert len(banner) == 3 and banner[:2] == ['LISTENING', '127.0.0.1'], banner
    port = int(banner[2])
    assert 0 < port <= 65535, banner
    return server, port


def exercise(chat, server_program, client_program, opt, cases):
    def case(name, function):
        row = dict(name=name, status='failed')
        cases.append(row)
        row['details'] = function()
        row['status'] = 'passed'

    server, port = start_server(chat, server_program)
    alice_name, bob_name = '甲😀', 'beta'
    alice = chat.start(client_program, ['127.0.0.1', port, alice_name], 'alice')
    chat.expect(alice, f'* {alice_name} joined')
    bob = chat.start(client_program, ['127.0.0.1', port, bob_name], 'bob')
    broadcast(chat, [alice, bob], '* beta joined')
    watcher = join_raw(chat, port, 'watch', [alice, bob], 'watcher')
    peers = [alice, bob, watcher]

    def native_exchange():
        chat.send(alice, 'hello 中😀\r\n\none\ntwo\r\n'.encode())
        for message in ('hello 中😀', 'one', 'two'):
            broadcast(chat, peers, f'[{alice_name}] {message}')
        chat.quiet(peers)
        chat.send(bob, 'reply 😀\n'.encode())
        broadcast(chat, peers, '[beta] reply 😀')
        return dict(woozy_clients=2, messages=4, empty_line_ignored=True, crlf=True)
    case('native-multiclient-unicode-and-coalesced-lines', native_exchange)

    def fragments():
        peer = chat.raw(port, 'fragmented')
        for byte in 'split😀\n'.encode():
            chat.send(peer, bytes([byte]))
            chat.poll(.002)
        broadcast(chat, [*peers, peer], '* split😀 joined')
        for byte in '分包😀\r\n'.encode():
            chat.send(peer, bytes([byte]))
            chat.poll(.002)
        broadcast(chat, [*peers, peer], '[split😀] 分包😀')
        chat.send(peer, b'first\n\nsecond\n')
        for message in ('first', 'second'):
            broadcast(chat, [*peers, peer], f'[split😀] {message}')
        chat.quiet([*peers, peer])
        chat.send(peer, b'/quit\n')
        broadcast(chat, peers, '* split😀 left')
        remaining = chat.closed(peer)
        assert all(line == '* split😀 left'.encode() for line in remaining), remaining
        chat.close_peer(peer)
        return dict(bytewise_utf8=True, coalesced_frames=3)
    case('raw-tcp-fragmentation', fragments)

    def maximum_frame():
        nickname = '😀' * 32
        peer = join_raw(chat, port, nickname, peers, 'maximum')
        payload = 'x' * MAX_LINE
        chat.send(peer, payload.encode() + b'\r\n')
        broadcast(chat, [*peers, peer], f'[{nickname}] {payload}')
        chat.close_peer(peer)
        broadcast(chat, peers, f'* {nickname} left')
        return dict(nickname_codepoints=32, message_bytes=MAX_LINE,
                    server_frame_bytes=len(f'[{nickname}] {payload}'.encode()))
    case('maximum-message-with-unicode-prefix', maximum_frame)

    for label, frame in (
            ('duplicate', b'watch\n'), ('empty', b'\n'),
            ('leading-space', b' bad\n'), ('trailing-space', b'bad \n'),
            ('nickname-c0', b'bad\x00\n'), ('nickname-c1', 'bad\u0085\n'.encode()),
            ('nickname-too-long', ('😀' * 33 + '\n').encode()),
            ('nickname-invalid-utf8', b'bad\xff\n')):
        case('reject-' + label,
             lambda label=label, frame=frame: rejected(chat, port, peers, label, frame))
    for label, frame in (
            ('message-too-long', b'x' * (MAX_LINE + 1) + b'\n'),
            ('frame-too-long', b'x' * (MAX_FRAME + 1) + b'\n'),
            ('message-control', b'bad\x00message\n'),
            ('message-invalid-utf8', b'bad\xf0\x28\x8c\x28\n')):
        case('reject-' + label,
             lambda label=label, frame=frame: rejected(
                 chat, port, peers, label, frame, nickname=label))

    def local_validation():
        results = []
        for label, data in (('long', b'x' * (MAX_LINE + 1) + b'\n'),
                            ('utf8', b'bad\xff\n'), ('control', b'bad\x00\n')):
            nickname = 'local-' + label
            peer = chat.start(client_program, ['127.0.0.1', port, nickname], nickname)
            broadcast(chat, [*peers, peer], f'* {nickname} joined')
            error_path = chat.processes[-1][2]
            before = error_path.read_bytes()
            message = f'after-{label} 😀'
            chat.send(peer, data + message.encode() + b'\n')
            broadcast(chat, [*peers, peer], f'[{nickname}] {message}')
            chat.quiet([*peers, peer])
            assert peer.process.poll() is None, (nickname, 'client did not recover')
            errors = error_path.read_bytes()[len(before):]
            assert errors, (nickname, 'invalid stdin line had no stderr notice')
            chat.send(peer, b'/quit\n')
            broadcast(chat, peers, f'* {nickname} left')
            chat.exit(peer)
            chat.quiet(peers)
            results.append(dict(nickname=nickname, notice=errors.decode('utf-8')))
        return results
    case('client-invalid-stdin-recovery', local_validation)

    def duplicate_client():
        peer = chat.start(client_program, ['127.0.0.1', port, 'watch'], 'duplicate-client')
        chat.exit(peer, expected=1)
        lines = chat.closed(peer)
        assert all(line.startswith(b'ERROR ') for line in lines), lines
        chat.quiet(peers)
        return dict(exit=1, server_lines=[line.decode() for line in lines])
    case('native-client-server-ERROR-status', duplicate_client)

    def eof_and_quit():
        chat.send(bob, 'final 😀'.encode())
        bob.process.stdin.close()
        broadcast(chat, peers, '[beta] final 😀')
        broadcast(chat, [alice, watcher], '* beta left')
        chat.exit(bob)
        chat.send(alice, b'/quit\nignored-after-quit\n')
        chat.expect(watcher, f'* {alice_name} left')
        chat.exit(alice)
        chat.quiet([watcher])
        return dict(unterminated_eof_line=True, quit_stdin_remained_open=True)
    case('client-eof-and-quit', eof_and_quit)

    def reconnect():
        for index in range(2):
            peer = join_raw(chat, port, 'again', [watcher], f'reconnect-{index}')
            chat.send(peer, b'back\n')
            broadcast(chat, [watcher, peer], '[again] back')
            chat.close_peer(peer)
            chat.expect(watcher, '* again left')
        return dict(reconnections=1, abrupt_disconnects=2)
    case('disconnect-and-nickname-reuse', reconnect)

    if opt == 2:
        def slow_reader():
            slow = chat.raw(port, 'slow', 'slow', receive_buffer=1024)
            broadcast(chat, [watcher, slow], '* slow joined')
            chat.pause_reading(slow)
            deadline = time.monotonic() + 10
            sent, removed = 0, False
            for index in range(512):
                if time.monotonic() >= deadline:
                    break
                payload = f'pressure-{index}:' + 'x' * 8000
                frame = payload.encode() + b'\n'
                chat.send(watcher, frame, timeout=2)
                sent += len(frame)
                while True:
                    line = chat.line(watcher, timeout=2)
                    if line == b'* slow left':
                        removed = True
                        continue
                    assert line == ('[watch] ' + payload).encode(), short(line)
                    break
                if removed:
                    break
            chat.send(watcher, 'responsive 😀\n'.encode())
            line = chat.line(watcher, timeout=2)
            if line == b'* slow left':
                removed = True
                line = chat.line(watcher, timeout=2)
            assert line == '[watch] responsive 😀'.encode(), short(line)
            chat.close_peer(slow)
            if not removed:
                chat.expect(watcher, '* slow left')
            return dict(pressure_bytes=sent, message_limit=512, time_limit_seconds=10,
                        slow_peer_removed_during_pressure=removed,
                        healthy_peer_received_marker=True)
        case('bounded-slow-reader-O2', slow_reader)

    def server_close():
        peer = chat.start(client_program, ['127.0.0.1', port, 'linger'], 'linger')
        broadcast(chat, [watcher, peer], '* linger joined')
        assert not peer.process.stdin.closed
        started = time.monotonic()
        chat.stop(server)
        chat.exit(peer, timeout=3)
        elapsed = time.monotonic() - started
        assert elapsed < 3, ('client exit after server close', elapsed)
        return dict(stdin_still_open=not peer.process.stdin.closed,
                    exit_seconds=elapsed)
    case('server-close-with-client-stdin-open', server_close)

    def connection_failure():
        # Reserve a loopback port without listening, so another service cannot
        # take it between selection and this client's refused connection.
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as reserved:
            reserved.bind(('127.0.0.1', 0))
            started = time.monotonic()
            peer = chat.start(client_program, ['127.0.0.1', reserved.getsockname()[1],
                                               'refused'], 'connection-failure')
            chat.exit(peer, expected=1, timeout=6)
            return dict(exit_seconds=time.monotonic() - started)
    case('client-connection-failure', connection_failure)

    def untrusted_server():
        results = []
        for label, frame in (('escape', b'[evil] \x1b[31mred\n'),
                             ('nul', b'[evil] bad\x00text\n'),
                             ('utf8', b'[evil] bad\xff\n')):
            # This fixture tests only the real client's input validation.
            # Native server/client exchange is covered separately above.
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                listener.settimeout(3)
                peer = chat.start(client_program, ['127.0.0.1', listener.getsockname()[1],
                                                   'trust-test'], 'untrusted-' + label)
                with listener.accept()[0] as connection:
                    connection.settimeout(3)
                    nickname = bytearray()
                    while b'\n' not in nickname:
                        part = connection.recv(128)
                        assert part and len(nickname) < 128, (label, nickname)
                        nickname.extend(part)
                    assert bytes(nickname) == b'trust-test\n', (label, nickname)
                    connection.sendall(frame)
                    chat.exit(peer, expected=1, timeout=3)
                lines = chat.closed(peer)
                errors = chat.processes[-1][2].read_bytes()
                assert not lines, (label, 'untrusted line was printed', lines)
                assert not any(byte in errors for byte in (b'\x1b', b'\x00', b'\xff')), (label, errors)
                results.append(dict(input=label, exit=1))
        return dict(scope='Native client with Python TCP trust-boundary fixture', runs=results)
    case('client-untrusted-server-input', untrusted_server)
    return port


def command(argv, environment, timeout=60):
    argv = list(map(str, argv))
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, env=environment, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
        status = process.returncode
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate(timeout=2)
        status = 'timeout'
    return dict(argv=argv, exit=status,
                stdout=stdout.decode('utf-8', 'backslashreplace'),
                stderr=stderr.decode('utf-8', 'backslashreplace'))


def succeeded(row):
    assert row['exit'] == 0, row


def elf_checks(program, environment):
    headers = command(['readelf', '-l', program], environment, 10)
    dynamic = command(['readelf', '-d', program], environment, 10)
    symbols = command(['nm', '--undefined-only', program], environment, 10)
    for row in (headers, dynamic, symbols):
        succeeded(row)
    assert 'INTERP' not in headers['stdout'], headers
    assert 'NEEDED' not in dynamic['stdout'], dynamic
    undefined = [line.split() for line in symbols['stdout'].splitlines() if line.strip()]
    optional_hooks = {
        'lang_vec_observe_v1', 'lang_process_init_v1',
        'lang_async_fork_reserve_v1', 'lang_async_fork_release_v1',
        'lang_async_fork_prepare_v1', 'lang_async_fork_parent_v1',
        'lang_async_fork_child_v1',
    }
    assert all(kind == 'w' and name in optional_hooks
               for kind, name in undefined), undefined
    return dict(headers=headers, dynamic=dynamic, undefined=undefined)


def accept_errors(args, environment, server_object, client_program, opt, cases):
    for error in (*TEMPORARY_ACCEPT_ERRORS, 9, 22):
        fatal = error not in TEMPORARY_ACCEPT_ERRORS
        directory = args.out / f'O{opt}' / 'accept-errors' / str(error)
        directory.mkdir(parents=True, exist_ok=True)
        row = dict(name=f'simulated-accept-error-{error}', status='failed', errno=error,
                   fatal=fatal, scope='runtime accept symbol wrap; not a kernel network fault')
        cases.append(row)
        fixture = directory / 'fixture.o'
        row['compile'] = compiled = command([
            args.clang, '-std=c++20', f'-O{opt}', '-ffreestanding', '-fno-exceptions',
            '-fno-rtti', '-fno-stack-protector', '-fno-sanitize=all', '-fno-builtin',
            f'-DCHAT_ACCEPT_ERROR={error}', '-c', args.root / 'tests/native/chat_accept_error.cpp',
            '-o', fixture], environment)
        succeeded(compiled)
        program = directory / 'server-wrapped'
        row['link'] = linked = command([
            args.clang, '-nostdlib', '-static', f'--ld-path={args.lld}',
            '-Wl,--wrap=lang_net_accept_record_v1', server_object, fixture,
            args.runtime, '-o', program], environment)
        succeeded(linked)
        row['elf'] = elf_checks(program, environment)
        chat = Chat(directory / 'logs')
        try:
            server, port = start_server(chat, program)
            alice = chat.start(client_program, ['127.0.0.1', port, 'accept-alice'], 'alice')
            chat.expect(alice, '* accept-alice joined')
            # The first accept succeeded and its draining accept saw EAGAIN.
            # Only now connect Bob, so the fixture fails accept call three.
            try:
                bob = chat.raw(port, 'bob')
            except ConnectionResetError:
                if not fatal:
                    raise
                # A fatal accept closes the listener before connect can finish.
                bob = None
            if bob is not None:
                chat.send(bob, b'accept-bob\n', allow_closed=fatal)
            if fatal:
                chat.exit(server, expected=1)
                chat.exit(alice)
                row['details'] = dict(server_exit=1, existing_client_exited=True)
            else:
                broadcast(chat, [alice, bob], '* accept-bob joined')
                chat.send(alice, 'after-accept-error 中😀\n'.encode())
                broadcast(chat, [alice, bob], '[accept-alice] after-accept-error 中😀')
                chat.send(bob, 'reply 😀\n'.encode())
                broadcast(chat, [alice, bob], '[accept-bob] reply 😀')
                assert server.process.poll() is None, ('server exited after transient accept error', error)
                row['details'] = dict(server_alive=True, peers=2, bidirectional_broadcast=True)
            stderr = chat.processes[0][2].read_bytes().decode('utf-8')
            marker = f'CHAT_ACCEPT_ERROR:{error}\n'
            assert stderr.count(marker) == 1, ('simulated accept error did not occur exactly once', error, stderr)
            if fatal:
                assert f'chat-server: error {error}\n' in stderr, (error, stderr)
            row['details']['simulation_markers'] = 1
        finally:
            chat.cleanup()
            row['processes'] = chat.summary()
        row['status'] = 'passed'


def main():
    parser = argparse.ArgumentParser()
    for name in ('cli', 'root', 'out', 'clang', 'lld', 'runtime'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    for name in ('cli', 'root', 'out', 'clang', 'runtime'):
        setattr(args, name, getattr(args, name).resolve())
    # LLD selects its Unix driver from argv[0]; keep the ld.lld symlink name.
    args.lld = args.lld.absolute()
    args.out.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
    for parent in (args.root, args.root.parent):
        library = parent / 'toolchains/libxml2-legacy-2.13.9/usr/lib'
        if (library / 'libxml2.so.2').exists():
            previous = environment.get('LD_LIBRARY_PATH')
            environment['LD_LIBRARY_PATH'] = str(library) + (':' + previous if previous else '')
            break
    report = dict(status='failed', runs=[])
    try:
        for opt in (0, 2):
            row = dict(opt=opt, status='failed', build={}, elf={}, help={}, cases=[])
            report['runs'].append(row)
            programs = {}
            for role in ('server', 'client'):
                output = args.out / f'O{opt}' / role
                row['build'][role] = built = command([
                    args.cli, 'build', '--project', args.root / f'examples/chat/{role}.toml',
                    '--backend', 'native', '--opt', opt, '--out-dir', output,
                    '--format', 'json'], environment)
                succeeded(built)
                programs[role] = output / 'program'
                row['elf'][role] = elf_checks(programs[role], environment)
                row['help'][role] = helped = command([programs[role], '--help'], environment, 5)
                succeeded(helped)
                assert helped['stdout'] or helped['stderr'], helped
            chat = Chat(args.out / f'O{opt}' / 'logs')
            try:
                row['port'] = exercise(chat, programs['server'], programs['client'], opt, row['cases'])
            finally:
                chat.cleanup()
                row['processes'] = chat.summary()
            accept_errors(args, environment, programs['server'].with_name('program.o'),
                          programs['client'], opt, row['cases'])
            row['status'] = 'passed'
        report['status'] = 'passed'
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        path = args.out / 'chat-results.json'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(dict(status=report['status'], configurations=len(report['runs']),
                          cases=sum(len(row['cases']) for row in report['runs']), report=str(path))))


if __name__ == '__main__':
    main()
