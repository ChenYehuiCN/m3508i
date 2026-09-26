[简体中文](README_zh-CN.md)

# M3508I RS485 Protocol

This repository contains a small C implementation of the M3508I motor RS485 protocol, an RS485 adapter design, and an STM32 example.

## Protocol Summary

| Item | Value |
|---|---|
| Physical layer | RS485 half-duplex |
| Serial format | 921600 baud, 8 data bits, no parity, 1 stop bit |
| Motor IDs | 0, 1, 2, 3 |
| Downlink command frame | 22 bytes |
| Uplink reply frame | 32 bytes |
| Angle scale | 32768 counts per mechanical revolution |

This README calls the control-board-to-motor packet the downlink command frame and the motor-to-control-board packet the uplink reply frame. Every command frame updates all four motor slots. Byte 7 selects the motor that replies to that frame.

One request/reply transaction consists of one 22-byte downlink command frame and one 32-byte uplink reply. During the frequency test, responder IDs 0, 1, 2, and 3 were sent in rotation. The measured reliable limit was 1333 transactions, or broadcasts, per second; 1334 was not reliable. A complete four-motor poll therefore takes four transactions and runs at about 333.25 polls per second. Each motor's reply rate is about 333.25 Hz, while every command frame still updates all four motor slots.

## Downlink Command Frame

All multi-byte values are little-endian.

| Offset | Length | Field | Description |
|---:|---:|---|---|
| 0..6 | 7 | Constant | `55 16 00 9D A0 00 00` |
| 7 | 1 | Responder ID | Motor ID 0..3 that must send a reply |
| 8..9 | 2 | Speed slot 0 | Motor 0 |
| 10..11 | 2 | Speed slot 1 | Motor 1 |
| 12..13 | 2 | Speed slot 2 | Motor 2 |
| 14..15 | 2 | Speed slot 3 | Motor 3 |
| 16..19 | 4 | Constant | `FF FF FF FF` |
| 20..21 | 2 | CRC16 | CRC of bytes 0..19, little-endian |

### Speed Slot

The slot is a 16-bit value. Its low 14 bits carry the signed speed code.

The high two bits select the motor state:

| bit15 | bit14 | State |
|---:|---:|---|
| 0 | 0 | Enabled |
| 0 | 1 | Disabled, no torque output |
| 1 | 0 | Enabled; bit 15 alone is not a disable flag |
| 1 | 1 | Enabled |

The exact command encoding used by this project is:

```c
slot = (uint16_t)((16384 + (int)lroundf(speed_rpm * 8.191f)) % 16384 | !enable << 14);
```

The tested command range is -1000 to +1000 rpm. The corresponding endpoint codes are `0x2001` and `0x1FFF`. `0x2000` is a valid enabled slot and decodes as signed code `-8192`. When `enable` is false, the program sets bit 14 with `!enable << 14`; the low 14 bits still come from `speed_rpm`.

## Uplink Reply Frame

All multi-byte values are little-endian.

| Offset | Length | Field | Description |
|---:|---:|---|---|
| 0..5 | 6 | Constant | `55 20 00 1A A0 01` |
| 6 | 1 | Motor ID | Replying motor, 0..3 |
| 7 | 1 | Constant | `00` |
| 8 | 1 | Variable byte | Unknown meaning |
| 9 | 1 | Bus voltage | `voltage_v = (byte9 + 3) / 4`; resolution 0.25 V/count |
| 10..11 | 2 | Current/torque raw value | Signed `int16_t` protocol value |
| 12..13 | 2 | Internal temperature raw value | Value from the SPD1078 internal temperature sensor |
| 14..15 | 2 | Measured speed | Signed value, 1 rpm/count; frame-to-frame noise is present |
| 16..17 | 2 | Control-slot echo | Low 14 bits of the command slot, interpreted as a signed 14-bit code |
| 18..19 | 2 | Coil temperature raw value | Value from the temperature sensor attached to the motor coil |
| 20 | 1 | State | `50` enabled, `90` disabled |
| 21 | 1 | Constant | `00` |
| 22..23 | 2 | Absolute angle | 15-bit count; wraps from 32767 to 0 modulo 32768; 32768 counts per revolution |
| 24..25 | 2 | Sequence | Per-motor reply counter; increments by one and wraps from `FFFF` to `0000` |
| 26..29 | 4 | Constant | `00 00 00 00` |
| 30..31 | 2 | CRC16 | CRC of bytes 0..29, little-endian |

The current/torque field is a signed protocol count. Its ampere and torque scales are not part of this library. The two temperature fields are raw sensor counts; this repository does not define a temperature conversion formula.

To calculate speed from angle feedback, unwrap the signed angle difference modulo 32768:

```text
delta = signed_wrap(angle_new - angle_old, 32768)
rpm = delta * 60 / (32768 * elapsed_seconds)
```

This angle-derived speed is smoother than the measured-speed field and is suitable for speed or position control.

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

`m3508i_parse_frame` verifies the reply CRC and fills the fields exposed by `struct m3508i_reply`. Keep the received 32-byte frame when the raw current or temperature fields are required.

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
