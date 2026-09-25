[简体中文](README_zh-CN.md)

# M3508I RS485 Protocol

This repository contains a small C implementation of the M3508I motor RS485 protocol, an RS485 adapter design, and an STM32 example.

## Protocol Summary

| Item | Value |
|---|---|
| Physical layer | RS485 half-duplex |
| Serial format | 921600 baud, 8 data bits, no parity, 1 stop bit |
| Motor IDs | 0, 1, 2, 3 |
| Command frame | 22 bytes |
| Reply frame | 32 bytes |
| Angle scale | 32768 counts per mechanical revolution |

Every command frame updates all four motor slots. Byte 7 selects the motor that replies to that frame.

For a complete four-motor poll, send four frames with responder IDs 0, 1, 2, and 3. With the current ESP32 firmware, RS485 adapter, and 921600 baud configuration, 1333 complete polls per second is the measured reliable limit; 1334 polls per second is not reliable.

## Command Frame

All multi-byte values are little-endian.

| Offset | Length | Field | Value or meaning |
|---:|---:|---|---|
| 0..2 | 3 | Header | `55 16 00` |
| 3 | 1 | Header | `9D` |
| 4 | 1 | Header | `A0` |
| 5..6 | 2 | Header | `00 00` |
| 7 | 1 | Responder ID | Motor ID 0..3 that must send a reply |
| 8..9 | 2 | Speed slot 0 | Motor 0 |
| 10..11 | 2 | Speed slot 1 | Motor 1 |
| 12..13 | 2 | Speed slot 2 | Motor 2 |
| 14..15 | 2 | Speed slot 3 | Motor 3 |
| 16..19 | 4 | Fixed bytes | `FF FF FF FF` |
| 20..21 | 2 | CRC16 | CRC of bytes 0..19, little-endian |

### Speed Slot

The slot is a 16-bit value. Its low 14 bits carry the signed speed code:

```text
low14   = slot & 0x3FFF
signed  = low14 < 0x2000 ? low14 : low14 - 0x4000
p16_rpm = signed / 8.191
```

The high two bits select the motor state:

| `slot & 0xC000` | State |
|---:|---|
| `0x0000` | Enabled |
| `0x4000` | Disabled, no torque output |
| `0x8000` or `0xC000` | Enabled; bit 15 alone is not a disable flag |

The normal command encoding used by this project is:

```c
code = (0x4000 + round_away_from_zero(speed_rpm * 8.191)) % 0x4000;
slot = code | (enable ? 0x0000 : 0x4000);
```

The tested command range is -1000 to +1000 rpm. The corresponding endpoint codes are `0x2001` and `0x1FFF`. `0x2000` is a valid enabled slot and decodes as signed code `-8192`. A disabled slot may retain any low-14-bit value; `0x4000` is the usual disabled value.

## Reply Frame

All multi-byte values are little-endian. The names `p10`, `p12`, `p14`, `p16`, and `p18` refer to the corresponding byte offsets.

| Offset | Length | Field | Meaning |
|---:|---:|---|---|
| 0..2 | 3 | Header | `55 20 00` |
| 3 | 1 | Constant | `1A` |
| 4 | 1 | Constant | `A0` |
| 5 | 1 | Constant | `01` |
| 6 | 1 | Motor ID | Replying motor, 0..3 |
| 7 | 1 | Constant | `00` |
| 8 | 1 | Variable byte | Varies; no protocol meaning is assigned by this library |
| 9 | 1 | Bus voltage | `voltage_v = (byte9 + 3) / 4`; resolution 0.25 V/count |
| 10..11 | 2 | p10 | Signed `int16_t` current/torque raw value |
| 12..13 | 2 | p12 | Raw value from the SPD1078 internal temperature sensor |
| 14..15 | 2 | p14 | Signed measured speed, approximately 1 rpm/count; frame-to-frame noise is present |
| 16..17 | 2 | p16 | Echo of the command slot's low 14 bits; decode it with the signed-14-bit rule above |
| 18..19 | 2 | p18 | Raw value from the temperature sensor attached to the motor coil |
| 20 | 1 | State | `50` enabled, `90` disabled |
| 21 | 1 | Constant | `00` |
| 22..23 | 2 | Absolute angle | 15-bit count, modulo 32768; 32768 counts per revolution |
| 24..25 | 2 | Sequence | Per-motor reply counter; increments by one and wraps from `FFFF` to `0000` |
| 26..29 | 4 | Constant | `00 00 00 00` |
| 30..31 | 2 | CRC16 | CRC of bytes 0..29, little-endian |

p10 is a signed protocol count. Its ampere and torque scales are not part of this library. p12 and p18 are temperature sensor raw counts; this repository does not define a temperature conversion formula.

To calculate speed from angle feedback, unwrap the signed angle difference modulo 32768:

```text
delta = signed_wrap(angle_new - angle_old, 32768)
rpm = delta * 60 / (32768 * elapsed_seconds)
```

This angle-derived speed is smoother than p14 and is suitable for speed or position control.

## CRC16

The CRC variant uses:

- Initial value `0x496C`
- Polynomial `0x1021`
- Bit reversal on every input byte
- Left-shift processing
- Bit reversal of the final 16-bit result
- Little-endian storage on the wire

The command CRC covers 20 bytes. The reply CRC covers 30 bytes.

## C API

`m3508i.h` defines the frame sizes, angle scale, command type, reply type, and two functions:

```c
void m3508i_build_frame(
    uint8_t (*out_frame)[M3508I_COMMAND_FRAME_SIZE],
    uint8_t responder_id,
    struct m3508i_cmd (*motor_cmd)[4]);

bool m3508i_parse_frame(
    struct m3508i_reply *out_reply,
    uint8_t (*in_frame)[M3508I_REPLY_FRAME_SIZE]);
```

Example:

```c
#include "m3508i.h"

struct m3508i_cmd motors[4] = {
    { .speed_rpm = 100.0f,  .enable = true  },
    { .speed_rpm = -50.0f,  .enable = true  },
    { .speed_rpm = 0.0f,    .enable = false },
    { .speed_rpm = 200.0f,  .enable = true  },
};
uint8_t tx[M3508I_COMMAND_FRAME_SIZE];
uint8_t rx[M3508I_REPLY_FRAME_SIZE];
struct m3508i_reply reply;

m3508i_build_frame(&tx, 0, &motors);
/* Send all 22 bytes through RS485, then receive 32 bytes. */
if (m3508i_parse_frame(&reply, &rx)) {
    /* reply.motor_id, reply.angle_count, reply.sequence, ... */
}
```

`m3508i_parse_frame` verifies the reply CRC and fills the fields exposed by `struct m3508i_reply`. Keep the received 32-byte frame when raw fields such as p10, p12, or p18 are required.

## STM32 Example

The `m3508i-test/` project uses UART4 and the following pins:

| Pin | Function |
|---|---|
| PA0-WKUP | UART4_TX to RS485 DI |
| PA1 | UART4_RX from RS485 RO |
| PA2 | RS485 transmit/receive direction |
| PA3..PA6 | Active-low motor 0..3 indicator LEDs |

The example sends one complete command frame, receives the selected motor reply, and repeatedly runs motors 0 through 3 in sequence. Each motor moves two revolutions with a 50 rpm command and angle feedback.

## Repository Layout

- `m3508i.c`, `m3508i.h`: protocol implementation
- `m3508i-test/`: STM32 example project
- `rs485-converter/`: RS485 adapter board design

## License

MIT License. See [LICENSE](LICENSE).
