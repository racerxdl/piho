#pragma once

#define CAN_RXD 17
#define CAN_TXD 16
#define CAN_BAUD 250000

#define LED_ACT 18
#define LED_HC  19
#define LED_ERR 20

#define DATA_MASK 0xFFFF
#define DATA_SHIFT 0

#define ADDR_BIT_0 21
#define ADDR_BIT_1 22
#define ADDR_BIT_2 26
#define ADDR_BIT_3 27
#define ADDR_BIT_4 28

#define DEBOUNCE_TIME 500
#define PIHO_QUEUE_MAX_ITEMS 32
#define HC_LED_TIMEOUT 50
#define ERR_LED_TIMEOUT 50
#define IO_CHECK_INTERVAL 5
#define MAX_UART_BYTES 128


#define BIT(v, n) (v & (1 << n))
#define SETBIT(v, n) (v | (1 << n))
#define CLRBIT(v, n) (v & ~(1 << n))

