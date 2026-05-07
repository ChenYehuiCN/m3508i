[简体中文](README_zh-CN.md)

# m3508i

This project presents the results of reverse-engineering the communication protocol of the **M3508I brushless motor** used in the DJI RoboMaster S1. By capturing RS485 data packets between the control board and the motor, the command frame format was decoded. A C implementation capable of generating compatible data packets is provided. The project includes protocol generation code (`m3508i.c`/`m3508i.h`), an RS485 adapter board hardware design (`rs485-converter/`), and an STM32 example project (`m3508i-test/`), helping developers easily drive the M3508I motor.

## Motor Basic Information

- **Model**: M3508I
- **Communication Interface**: RS485 (half-duplex)
- **Baud Rate**: 921600
- **Power Supply**: 3S LiPo battery (approx. 12V)
- **ID Range**: 0–3 (up to 4 motors sharing the bus)
- **Speed Range**: -1000 rpm ~ +1000 rpm
- **Enable Control**: The `enable` field controls whether the motor outputs torque. When `enable = false`, the motor is disabled and can spin freely without resistance.
- **Automatic Protection**: If no valid command is received for more than 1 second, the motor automatically enters the disabled state to prevent a runaway machine if control is lost.

## Protocol Format

Each frame sent from the control board to the motors is **22 bytes** long with the following structure:

| Offset | Length | Content          | Description                                                                 |
|--------|--------|------------------|-----------------------------------------------------------------------------|
| 0      | 7      | Frame Header     | Fixed value `0x55 0x16 0x00 0x9D 0xA0 0x00 0x00`                           |
| 7      | 1      | Responding Motor ID | Specifies which motor should reply to this communication                 |
| 8      | 8      | Motor Speed Data | 2 bytes per motor (4 motors), little-endian byte order, format described below |
| 16     | 4      | Padding          | Fixed as `0xFF 0xFF 0xFF 0xFF`                                              |
| 20     | 2      | CRC16            | Checksum calculated over the first 20 bytes, 16-bit value in little-endian order, algorithm described later |

> **Note**: The frame is broadcast, so all motors on the bus immediately update their speed setpoints, but only the motor specified by `Responding Motor ID` will reply with a response packet (the response packet format is not covered in this project).

### Speed Data Encoding

Each motor corresponds to 2 bytes of speed data. The 16-bit structure is defined as follows (stored in little-endian byte order):

| Bit Field | Name         | Description                                                                 |
|-----------|--------------|-----------------------------------------------------------------------------|
| bit15     | Reserved     | **Must be 0**                                                               |
| bit14     | Disable Flag | 1: motor disabled (no torque output); 0: motor enabled                      |
| bit13–0   | Speed Code   | 14-bit signed integer (two's complement), representing a linear mapping of the target speed |

The data is generated from the `enable` and `speed_rpm` fields of the `m3508i_cmd` structure:

```c
struct m3508i_cmd {
    bool enable;       /* true: enable motor; false: disable motor */
    float speed_rpm;   /* target speed, range -1000.0 ~ 1000.0 rpm */
};
```

**Encoding Rules:**

- **Disable motor** (`enable = false`):  
  Set bit14 to 1; the lower 14 bits can be arbitrary (the code retains the speed calculation value, but the motor only cares about bit14).  
  The motor does not output torque and can spin freely.

- **Enable motor** (`enable = true`):  
  bit14 = 0; the lower 14 bits hold a 14-bit signed integer obtained by linearly mapping the speed:
  1. Linearly map the speed range `-1000 ~ 1000 rpm` to `-8191 ~ +8191`:
     ```
     raw = (int)lroundf(speed_rpm * (8191.0f / 1000.0f))
     ```
  2. `raw` is the desired signed value, placed directly into the lower 14 bits as a 14-bit two's complement integer.

**Code Implementation:**

```c
uint16_t data = (16384 + (int)lroundf(speed_rpm * 8.191f)) % 16384 | !enable << 14;
```

### CRC16 Check Algorithm

The checksum uses a custom CRC16-CCITT variant, calculated as follows:

1. Initial value `0x496C`
2. Polynomial `0x1021` (CCITT standard polynomial)
3. **Data Preprocessing**: Each byte is **bit-reversed** (`reverse8`) before being processed
4. **Shift Calculation**: Left-shift method, processing 8 bits at a time
5. **Result Postprocessing**: The resulting 16-bit value is again **bit-reversed** (`reverse16`)

Corresponding code implementation:

```c
uint16_t generate_crc16(const uint8_t *data, int len)
{
    int i, j;
    uint16_t crc = 0x496C;
    const uint16_t poly = 0x1021;
    for (i = 0; i < len; i++) {
        uint8_t byte = reverse8(data[i]);  /* bit reverse */
        crc ^= (uint16_t)byte << 8;
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ poly;
            else
                crc = crc << 1;
        }
    }
    return reverse16(crc);  /* result bit reverse */
}
```

Where `reverse8` and `reverse16` are standard bit-reversal functions. The calculated CRC16 value is finally written to bytes 20 and 21 of the frame in **little-endian** byte order.

## Project Structure

- `m3508i.h` / `m3508i.c`: Core protocol generation code, providing the `m3508i_build_frame` function to construct a complete command frame.
- `rs485-converter/`: PCB project for an RS485 adapter board used to drive the motor.
- `m3508i-test/`: STM32-based example project showing how to send commands to control the motor.

## Usage Example

```c
#include "m3508i.h"

/* Prepare commands for 4 motors */
struct m3508i_cmd cmds[4] = {
    { .enable = true, .speed_rpm = 100.0f },   /* ID0 forward 100 rpm */
    { .enable = true, .speed_rpm = -50.0f },   /* ID1 reverse 50 rpm */
    { .enable = false },                       /* ID2 disabled */
    { .enable = true, .speed_rpm = 200.0f }    /* ID3 forward 200 rpm */
};

uint8_t frame[22];
m3508i_build_frame(&frame, 0, &cmds);  /* Build frame, designate ID0 as the responding motor */

/* Send the frame array (22 bytes) via RS485 */
```

After sending, all four motors immediately update their speeds, and the ID0 motor will reply with a response packet.

## License

This project is licensed under the **MIT License**. You are free to use, modify, and distribute the code, provided that the copyright notice is retained. Please be aware that reverse engineering may involve legal risks; ensure compliance with local laws and regulations as well as DJI's relevant terms before using this code.

## Disclaimer

This code is provided for educational and research purposes only. Full compatibility with official equipment is not guaranteed. The project contributors assume no legal liability for any issues arising from the use of this code.
