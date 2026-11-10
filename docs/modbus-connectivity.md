# Modbus TCP and RTU MVP

Modbus extends the existing Connection → Binding → HardwareExecutor boundary. Actions still pass the existing original-text SafetyGate and domain checks. Creating a draft, validating it, preflight, constructor and Playground send no Modbus packets. Review and explicit Apply publish configuration; a later authorized command performs a write.

The implementation follows the [Modbus application protocol](https://www.modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf) and [serial-line guide](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf). Addresses are **zero-based protocol addresses 0..65535**. A manual's `40001` notation is not automatically converted. Specify the actual protocol address from the device documentation.

## Supported contracts

- TCP: numeric IPv4, explicit port, Unit ID 1..247. No DNS or broadcast.
- RTU: a literal `/dev/ttyUSB<N>`, `/dev/ttyACM<N>`, `/dev/ttyS<N>` or `/dev/ttyAMA<N>` port; 8 data bits; none/even/odd parity; 1 or 2 stop bits. Supported rates: 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400. `/dev/pts/<N>` is also accepted for isolated tests. Symlinks and arbitrary filesystem paths are rejected.
- Write: function 5 (single coil, boolean) or 6 (single register, `uint16` or `int16`). Supply one fixed value or exactly one declared required action argument of the matching type. No implicit scaling, string conversion or overflow wrapping.
- Read: function 1/2 (one coil/discrete input) or 3/4 (one holding/input register), mapped to one declared boolean/integer state property.
- No automatic command retries, replay, background connections or background polling. Explicit state questions and the existing device-poll endpoint use the configured independent read.

`int16` is two's-complement signed 16-bit; `uint16` is 0..65535. Float32, multi-register values, byte/word-order conversions, multiple writes and programmable automation are outside this MVP. The system must request clarification rather than claim those contracts work.

## Configuration example

```json
{
  "id": "connection_relay_modbus",
  "name": "Relay controller",
  "type": "modbus_tcp",
  "enabled": true,
  "timeout_ms": 2000,
  "network": { "host": "192.168.68.120", "port": 502 },
  "modbus": { "unit_id": 1 }
}
```

An existing boolean ON action can map to:

```json
{
  "target_id": "device_relay",
  "action_id": "action_on",
  "connection_id": "connection_relay_modbus",
  "executor": "modbus_tcp",
  "modbus": { "function": 5, "address": 17, "data_type": "boolean", "value": true }
}
```

Its independent read is:

```json
{
  "target_id": "device_relay",
  "connection_id": "connection_relay_modbus",
  "executor": "modbus_tcp",
  "modbus": { "function": 1, "address": 17, "data_type": "boolean", "field": "power" },
  "fields": {}
}
```

For RTU use `type`/`executor: "modbus_rtu"` and replace `network` with explicit `serial: {"port":"/dev/ttyUSB0","baud_rate":9600,"parity":"even","stop_bits":1}`. Existing HTTP/MQTT and historical disabled transport configurations retain their canonical representation.

## Result and resource semantics

A matching write response establishes transport **Accepted/Predicted**, never physical **Confirmed**. Only an independent valid read can establish Confirmed state. A timeout may have uncertain delivery; Firefly reports failure and does not resend automatically.

One monotonic deadline covers waiting for the shared bus lock, opening/connecting, sending, receiving and RTU silence. TCP validates transaction ID, protocol ID, unit, length, function and exact write echo. RTU validates CRC, unit, function, inter-character gaps and complete frame silence. Descriptors, termios and advisory locks are released/restored on ordinary completion and error. Reads requested by voice use the smaller remaining voice-read deadline.

RTU uses nonblocking open, `O_NOFOLLOW`, character-device/TTY checks, advisory `flock` and `TIOCEXCL`. Advisory locks and exclusive-open protection cannot detect a different process that already opened the TTY without cooperation. A deployment must dedicate its serial port to this master. Use an adapter with automatic RS485 direction control or an already configured UART/driver; this change does not set driver-specific RTS/RS485 policies or alter systemd permissions.

## Verification boundary

`ModbusIsolatedIntegration` runs real TCP sockets and virtual serial PTYs. It checks valid coil/register packets and readback, malformed replies, CRC/framing failures, exceptions, disconnect, timeout/recovery, concurrent serialization, zero extra packets, termios restoration and descriptor cleanup. PTYs do not establish real UART baud, USB adapter turnaround, RS485 electrical behavior or physical device compatibility. Those require an owner-authorized hardware check before activation.

No production deployment or ConfigStore migration is performed by the tests.
