/*
 * m3508i.h
 *
 * Copyright (c) 2026 Chen Yehui
 * SPDX-License-Identifier: MIT
 */

#ifndef M3508I_H
#define M3508I_H

#include <stdbool.h>
#include <stdint.h>

#define M3508I_COMMAND_FRAME_SIZE 22
#define M3508I_REPLY_FRAME_SIZE 32
#define M3508I_ANGLE_COUNTS_PER_TURN 32768

struct m3508i_cmd {
	float speed_rpm;
	bool enable;
};

struct m3508i_reply {
	uint8_t motor_id;
	float voltage_v;
	int16_t measured_speed_rpm;
	uint16_t control_slot;
	uint8_t enable_state;
	uint16_t angle_count;
	uint16_t sequence;
};

void m3508i_build_frame(uint8_t (*out_frame)[M3508I_COMMAND_FRAME_SIZE], uint8_t responder_id, struct m3508i_cmd (*motor_cmd)[4]);
bool m3508i_parse_frame(struct m3508i_reply *out_reply, uint8_t (*in_frame)[M3508I_REPLY_FRAME_SIZE]);

#endif
