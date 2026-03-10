/*
 * m3508i.c
 *
 *  Created on: Feb 28, 2026
 *      Author: Chen Yehui
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "m3508i.h"

static uint8_t reverse8(uint8_t b)
{
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

static uint16_t reverse16(uint16_t x)
{
	x = (x & 0x5555) << 1 | (x & 0xAAAA) >> 1;
	x = (x & 0x3333) << 2 | (x & 0xCCCC) >> 2;
	x = (x & 0x0F0F) << 4 | (x & 0xF0F0) >> 4;
	x = (x & 0x00FF) << 8 | (x & 0xFF00) >> 8;
	return x;
}

static uint16_t generate_crc16(const uint8_t *data, int len)
{
	int i, j;
	uint16_t crc = 0x496C;
	const uint16_t poly = 0x1021;
	for (i = 0; i < len; i++) {
		uint8_t byte = data[i];
		byte = reverse8(byte);
		crc ^= (uint16_t)byte << 8;
		for (j = 0; j < 8; j++) {
			if (crc & 0x8000)
				crc = (crc << 1) ^ poly;
			else
				crc = crc << 1;
		}
	}
	return reverse16(crc);
}

void m3508i_build_frame(uint8_t (*out_frame)[22], uint8_t responder_id, struct m3508i_cmd (*motor_cmd)[4])
{
	int i;
	uint16_t crc16;
	uint16_t motor_speed_data[4];
	const uint8_t head[7] = {0x55, 0x16, 0x00, 0x9D, 0xA0, 0x00, 0x00};
	memcpy(*out_frame, head, sizeof(head));
	(*out_frame)[7] = responder_id;
	for (i = 0; i < 4; i++)
		motor_speed_data[i] = (*motor_cmd)[i].enable ? (16384 + (int)lroundf((*motor_cmd)[i].speed_rpm * 8.191f)) % 16384 : 0x4000;
	memcpy(*out_frame + 8, motor_speed_data, sizeof(motor_speed_data));
	memset(*out_frame + 16, 0xFF, 4);
	crc16 = generate_crc16(*out_frame, 20);
	memcpy(*out_frame + 20, &crc16, sizeof(crc16));
}
