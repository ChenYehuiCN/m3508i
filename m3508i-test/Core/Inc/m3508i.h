/*
 * m3508i.h
 *
 *  Created on: Feb 28, 2026
 *      Author: Chen Yehui
 */

#ifndef INC_M3508I_H_
#define INC_M3508I_H_

#include <stdbool.h>
#include <stdint.h>

struct m3508i_cmd {
	float speed_rpm;
	bool enable;
};

void m3508i_build_frame(uint8_t (*out_frame)[22], uint8_t responder_id, struct m3508i_cmd (*motor_cmd)[4]);

#endif /* INC_M3508I_H_ */
