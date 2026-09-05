#pragma once

#define CAN_RXD 17u
#define CAN_TXD 16u
#define CAN_BAUD 250000u

#define LED_ACT 18u
#define LED_HC 19u
#define LED_ERR 20u

#define DATA_MASK 0xFFFFu
#define DATA_SHIFT 0u

#define ADDR_BIT_0 21u
#define ADDR_BIT_1 22u
#define ADDR_BIT_2 26u
#define ADDR_BIT_3 27u
#define ADDR_BIT_4 28u

#define IO_CHECK_INTERVAL_MS 5u
#define PIHO_QUEUE_MAX_ITEMS 32u
#define HC_LED_TIMEOUT_MS 50u
#define ERR_LED_TIMEOUT_MS 50u
#define RESET_DELAY_MS 100u
#define MAX_UART_BYTES_PER_POLL 32u

#define INPUT_ACTIVE_LOW 1
#define OUTPUT_ACTIVE_LOW 1
