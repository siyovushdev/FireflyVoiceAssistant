#!/usr/bin/env python3
"""The existing 450 real authorization cases, observed on loopback Mosquitto.

Only inference and recognized text are synthetic. No physical voice acceptance.
The independent MQTT monitor checks actual delivered commands, not client return
codes. No system service, packages, production store or devices are used.
"""
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time


def frame(kind, body):
    assert len(body) < 128
    return bytes([kind, len(body)]) + body


def string(value):
    data = value.encode()
    return struct.pack('!H', len(data)) + data


def packet(sock):
    def take(count):
        data = b''
        while len(data) < count:
            part = sock.recv(count - len(data))
            assert part, 'MQTT monitor EOF'
            data += part
        return data
    kind, size, factor = take(1)[0], 0, 1
    for _ in range(4):
        byte = take(1)[0]
        size += (byte & 127) * factor
        if not byte & 128:
            assert size <= 4096
            return kind, take(size)
        factor *= 128
    raise AssertionError('invalid MQTT remaining length')


def run(app, root, mosquitto):
    with tempfile.TemporaryDirectory(prefix='firefly-grounding-mqtt-') as temporary:
        directory = Path(temporary)
        with socket.socket() as port_socket:
            port_socket.bind(('127.0.0.1', 0))
            port = port_socket.getsockname()[1]
        config = directory / 'broker.conf'
        config.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\n'
                          'persistence false\nlog_type all\n')
        native, monitor = None, None
        reports, commands = [], []
        with (directory / 'broker.log').open('w+') as log, (directory / 'native.log').open('w+') as errors:
            broker = subprocess.Popen([mosquitto, '-c', str(config)], stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    assert broker.poll() is None, 'isolated Mosquitto exited'
                    try:
                        monitor = socket.create_connection(('127.0.0.1', port), .1)
                        break
                    except OSError:
                        assert time.monotonic() < deadline, 'isolated broker startup timeout'
                        time.sleep(.02)
                monitor.settimeout(3)
                # No keepalive timer: the finite suite can exceed 45 seconds;
                # an idle observer must not be disconnected mid-negative case.
                monitor.sendall(frame(0x10, string('MQTT') + bytes([4, 2, 0, 0]) + string('authorization-observer')))
                assert packet(monitor) == (0x20, bytes([0, 0]))
                monitor.sendall(frame(0x82, bytes([0, 1]) + string('relay/#') + bytes([0])))
                assert packet(monitor) == (0x90, bytes([0, 1, 0]))
                assert not select.select([monitor], [], [], .05)[0], 'command before operational input'
                native = subprocess.Popen([app, root, str(port), 'mqtt'], stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=errors, text=True)
                while True:
                    assert select.select([native.stdout], [], [], 20)[0], 'authorization case timeout'
                    line = native.stdout.readline()
                    if not line:
                        break
                    report = json.loads(line)
                    assert report['pass'], report
                    expected = report['expected_post_count']  # Shared HTTP/MQTT case contract.
                    observed = []
                    if expected:
                        assert select.select([monitor], [], [], 2)[0], report
                        header, body = packet(monitor)
                        assert header == 0x30, (header, report)  # QoS0 monitor, retain=false, dup=false.
                        size = int.from_bytes(body[:2], 'big')
                        topic, payload = body[2:2 + size].decode(), body[2 + size:].decode()
                        assert topic == 'relay/' + report['expected_target'], (topic, report)
                        assert payload == ('ON' if report['expected_on'] else 'OFF'), (payload, report)
                        observed.append({'topic': topic, 'payload': payload})
                    assert not select.select([monitor], [], [], .01)[0], ('unexpected or rejected command', report)
                    report['observed_mqtt'] = observed
                    reports.append(report)
                    commands.extend(observed)
                    native.stdin.write('checked\n')
                    native.stdin.flush()
                native.stdin.close()
                assert native.wait(timeout=5) == 0, 'native authorization failure'
                assert len(reports) == 450
                # A positive control proves the monitor/executor path is live.
                assert commands and len(commands) == sum(r['expected_post_count'] for r in reports)
                log.flush()
                received = [line for line in (directory / 'broker.log').read_text().splitlines()
                            if 'Received PUBLISH' in line]
                assert len(received) == len(commands), (len(received), len(commands))
                assert all('(d0, q1, r0,' in line for line in received), received
                print(json.dumps({'result': 'PASS', 'cases': len(reports), 'operational_commands': len(commands),
                                  'rejected_zero_publish': sum(not r['expected_post_count'] for r in reports),
                                  'qos': 1, 'retain': False, 'pre_command_publish': 0,
                                  'scope': 'synthetic recognized text/inference; real SafetyGate/runtime/MQTT; no microphone'}))
                evidence = os.environ.get('FIREFLY_MQTT_GROUNDING_EVIDENCE')
                if evidence:
                    Path(evidence).write_text(json.dumps({'cases': reports, 'commands': commands}, ensure_ascii=False, indent=2))
            finally:
                if native:
                    if native.poll() is None:
                        native.kill()
                    native.wait(timeout=3)
                    native.stdout.close()
                    if not native.stdin.closed:
                        native.stdin.close()
                if monitor:
                    monitor.close()
                broker.terminate()
                try:
                    broker.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    broker.kill()
                    broker.wait(timeout=3)


if __name__ == '__main__':
    run(*sys.argv[1:])
