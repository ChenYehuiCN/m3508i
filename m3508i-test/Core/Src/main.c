/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include "m3508i.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#define M3508I_MOTOR_COUNT 4u
#define M3508I_TARGET_SPEED_RPM 50.0f
#define M3508I_TARGET_TURNS 2
#define M3508I_PID_KP 0.1f
#define M3508I_PID_KI 0.0f
#define M3508I_PID_KD 0.0f
#define M3508I_PID_MAX_RPM 50.0f
#define M3508I_PID_MIN_RPM 20.0f
#define M3508I_PID_INTEGRAL_LIMIT 20000.0f
#define M3508I_ANGLE_TOLERANCE 64
#define M3508I_SETTLE_SAMPLES 20u
#define M3508I_MOTOR_TIMEOUT_MS 8000u
#define M3508I_UART_TIMEOUT_MS 5u

static HAL_StatusTypeDef m3508i_send(struct m3508i_cmd (*cmd)[M3508I_MOTOR_COUNT], uint8_t responder_id)
{
	uint8_t frame[M3508I_COMMAND_FRAME_SIZE];
	HAL_StatusTypeDef status;
	m3508i_build_frame(&frame, responder_id, cmd);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_SET);
	status = HAL_UART_Transmit(&huart4, frame, sizeof(frame), 10);
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_2, GPIO_PIN_RESET);
	return status;
}

static bool m3508i_exchange(uint8_t motor, struct m3508i_cmd (*cmd)[M3508I_MOTOR_COUNT], struct m3508i_reply *reply)
{
	uint8_t frame[M3508I_REPLY_FRAME_SIZE];
	if (m3508i_send(cmd, motor) != HAL_OK)
		return false;
	if (HAL_UART_Receive(&huart4, frame, sizeof(frame), M3508I_UART_TIMEOUT_MS) != HAL_OK)
		return false;
	return m3508i_parse_frame(reply, &frame) && reply->motor_id == motor;
}

static bool m3508i_disable_all(struct m3508i_cmd (*cmd)[M3508I_MOTOR_COUNT])
{
	uint8_t frame[M3508I_REPLY_FRAME_SIZE];
	uint8_t motor;
	for (motor = 0; motor < M3508I_MOTOR_COUNT; motor++) {
		(*cmd)[motor].speed_rpm = 0.0f;
		(*cmd)[motor].enable = false;
	}
	if (m3508i_send(cmd, 0) != HAL_OK)
		return false;
	return HAL_UART_Receive(&huart4, frame, sizeof(frame), M3508I_UART_TIMEOUT_MS) == HAL_OK;
}

static int32_t m3508i_angle_delta(uint16_t current, uint16_t previous)
{
	int32_t delta = (int32_t)current - (int32_t)previous;
	const int32_t half_turn = (int32_t)M3508I_ANGLE_COUNTS_PER_TURN / 2;
	if (delta > half_turn)
		delta -= (int32_t)M3508I_ANGLE_COUNTS_PER_TURN;
	if (delta < -half_turn)
		delta += (int32_t)M3508I_ANGLE_COUNTS_PER_TURN;
	return delta;
}

static int32_t m3508i_abs(int32_t value)
{
	return value < 0 ? -value : value;
}

static bool m3508i_run_turns(uint8_t motor)
{
	const uint16_t led[4] = {GPIO_PIN_3, GPIO_PIN_4, GPIO_PIN_5, GPIO_PIN_6};
	const int32_t target_counts = (int32_t)M3508I_ANGLE_COUNTS_PER_TURN * M3508I_TARGET_TURNS;
	struct m3508i_cmd cmd[M3508I_MOTOR_COUNT] = {{0.0f, false}, {0.0f, false}, {0.0f, false}, {0.0f, false}};
	struct m3508i_reply reply;
	uint16_t previous_angle;
	uint32_t previous_tick;
	uint32_t start_tick;
	uint32_t settled = 0;
	int32_t travelled = 0;
	float integral = 0.0f;
	float previous_error = (float)target_counts;

	HAL_GPIO_WritePin(GPIOA, led[motor], GPIO_PIN_RESET);
	if (!m3508i_disable_all(&cmd))
		goto fail;
	HAL_Delay(5);
	if (!m3508i_exchange(motor, &cmd, &reply))
		goto fail;
	previous_angle = reply.angle_count;
	previous_tick = HAL_GetTick();
	start_tick = previous_tick;
	cmd[motor].speed_rpm = M3508I_TARGET_SPEED_RPM;
	cmd[motor].enable = true;
	while (HAL_GetTick() - start_tick < M3508I_MOTOR_TIMEOUT_MS) {
		uint32_t now;
		float dt;
		float error_float;
		float derivative;
		float speed;
		int32_t error;
		int32_t delta;
		if (!m3508i_exchange(motor, &cmd, &reply))
			goto fail;
		delta = m3508i_angle_delta(reply.angle_count, previous_angle);
		travelled += delta;
		now = HAL_GetTick();
		dt = (float)(now - previous_tick) / 1000.0f;
		if (dt <= 0.0f)
			dt = 0.001f;
		error = target_counts - travelled;
		error_float = (float)error;
		integral += error_float * dt;
		if (integral > M3508I_PID_INTEGRAL_LIMIT)
			integral = M3508I_PID_INTEGRAL_LIMIT;
		if (integral < -M3508I_PID_INTEGRAL_LIMIT)
			integral = -M3508I_PID_INTEGRAL_LIMIT;
		derivative = (error_float - previous_error) / dt;
		speed = M3508I_PID_KP * error_float + M3508I_PID_KI * integral + M3508I_PID_KD * derivative;
		if (speed > M3508I_PID_MAX_RPM)
			speed = M3508I_PID_MAX_RPM;
		if (speed < -M3508I_PID_MAX_RPM)
			speed = -M3508I_PID_MAX_RPM;
		if (speed > 0.0f && speed < M3508I_PID_MIN_RPM)
			speed = M3508I_PID_MIN_RPM;
		if (speed < 0.0f && speed > -M3508I_PID_MIN_RPM)
			speed = -M3508I_PID_MIN_RPM;
		if (m3508i_abs(error) <= M3508I_ANGLE_TOLERANCE) {
			cmd[motor].speed_rpm = 0.0f;
			settled++;
			if (settled >= M3508I_SETTLE_SAMPLES) {
				if (!m3508i_disable_all(&cmd))
					goto fail;
				HAL_GPIO_WritePin(GPIOA, led[motor], GPIO_PIN_SET);
				return true;
			}
		} else {
			settled = 0;
			cmd[motor].speed_rpm = speed;
		}
		previous_error = error_float;
		previous_angle = reply.angle_count;
		previous_tick = now;
	}

fail:
	m3508i_disable_all(&cmd);
	HAL_GPIO_WritePin(GPIOA, led[motor], GPIO_PIN_SET);
	return false;
}

static void m3508i_stop_forever(void)
{
	struct m3508i_cmd cmd[M3508I_MOTOR_COUNT] = {{0.0f, false}, {0.0f, false}, {0.0f, false}, {0.0f, false}};
	while (1) {
		m3508i_disable_all(&cmd);
		HAL_Delay(100);
	}
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
	uint8_t motor;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_UART4_Init();
  /* USER CODE BEGIN 2 */
	HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6, GPIO_PIN_SET);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    for (motor = 0; motor < M3508I_MOTOR_COUNT; motor++) {
    	if (!m3508i_run_turns(motor)) {
    		m3508i_stop_forever();
    	}
    	HAL_Delay(300);
    }
  }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
