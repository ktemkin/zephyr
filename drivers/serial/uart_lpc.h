/*
 * Copyright (c) 2016 Kyle J. Temkin <kyle@ktemkin.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Temporarily mask errors in messy LPC code. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuninitialized"
#include <chip.h>
#include <uart_18xx_43xx.h>
#pragma GCC diagnostic pop

#ifndef __UART_LPC__
#define __UART_LPC__

/*
 * Stores the premade configuration for a given LPC UART.
 */
struct uart_lpc_configuration {

	/* A pointer to the UART's register block. */
	LPC_USART_T *uart;

	/* The baud rate for the given UART. */
	uint32_t baudrate;

	/* The base clock for the given UART. */
	CHIP_CCU_CLK_T baseclock;

	/* The pinmux configruation for the Rx and Tx pins. */
	PINMUX_GRP_T pinmux_rx;
	PINMUX_GRP_T pinmux_tx;

};

/*
 * Stores the runtime data associated with an LPC UART.
 */
struct uart_lpc_data {

};


/* Convenience macros to access the properties of a HAL device. */
#define DEV_CFG(dev)  ((struct uart_lpc_configuration * const)(dev)->config->config_info)
#define DEV_DATA(dev) ((struct uart_lpc_data * const)(dev)->driver_data)
#define DEV_UART(dev) (DEV_CFG(dev)->uart)


#endif
