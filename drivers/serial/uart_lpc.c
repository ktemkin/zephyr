/*
 * Copyright (c) Kyle J. Temkin <kyle@ktemkin.com>
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
#include <nanokernel.h>
#include <arch/cpu.h>
#include <stdint.h>

#include <board.h>
#include <init.h>
#include <uart.h>
#include <toolchain.h>
#include <sections.h>

#include "uart_lpc.h"


static int uart_lpc_init(struct device *dev);

/**
 * Initialize each UART we have.
 */

#ifdef CONFIG_UART_LPC_0

static struct uart_lpc_data uart_lpc_uart0_data = {};
static struct uart_lpc_configuration uart_lpc_uart0_configuration = {
	.uart = LPC_USART0,
	.baudrate = CONFIG_UART_LPC_0_BAUDRATE,
	.baseclock = CLK_BASE_UART0,
	.pinmux_tx = {0x6, 4, (SCU_MODE_REPEATER | SCU_MODE_FUNC2)},
	.pinmux_rx = {0x6, 5, (SCU_MODE_PULLUP | SCU_MODE_INBUFF_EN | SCU_MODE_ZIF_DIS | SCU_MODE_FUNC2)},
};

DEVICE_INIT(uart_lpc_0, CONFIG_UART_LPC_0_NAME, uart_lpc_init,
		&uart_lpc_uart0_data, &uart_lpc_uart0_configuration, PRIMARY,
		CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

#endif // CONFIG_UART_LPC_0

/*
 * TODO: Support additional UARTs!
 */

/**
 * Reads a byte from the LPC UART, if possible.
 */ 
static int uart_lpc_poll_in(struct device *dev, unsigned char *data)
{
	LPC_USART_T *uart = DEV_UART(dev);

	/* If there's no data ready, abort! */
	if(!(Chip_UART_ReadLineStatus(uart) & UART_LSR_RDR)) {
			return -1;
	}

	/* Otherwise, read the provided byte. */
	*data = (unsigned char)Chip_UART_ReadByte(uart);
	return 0;
}

/**
 * Writes a byte to the LPC UART. May block until there's room for the given byte.
 */
static unsigned char uart_lpc_poll_out(struct device *dev, unsigned char data)
{
	LPC_USART_T *uart = DEV_UART(dev);

	/* Wait for there to be room to transmit the given byte... */
	while(!(Chip_UART_ReadLineStatus(uart) & UART_LSR_THRE));

	/* ... and then send it. */
	Chip_UART_SendByte(uart, data);
	return 0;
}


static struct uart_driver_api api = {
	.poll_in = uart_lpc_poll_in,
	.poll_out = uart_lpc_poll_out
	/* TODO: Handle additional features (e.g. interrupt driven). */
};

/**
 * Initialize a LPC uart.
 */
static int uart_lpc_init(struct device *dev)
{
	struct uart_lpc_configuration *cfg = DEV_CFG(dev);
	LPC_USART_T * uart = DEV_UART(dev);

	/* Set up the base clock for the UART. */
	Chip_Clock_SetBaseClock(cfg->baseclock, CLKIN_MAINPLL, true, false);
	Chip_Clock_EnableBaseClock(cfg->baseclock);

	/* Set up the device's pin multiplexing. */
	Chip_SCU_PinMuxSet(cfg->pinmux_rx.pingrp, cfg->pinmux_rx.pinnum, cfg->pinmux_rx.modefunc);
	Chip_SCU_PinMuxSet(cfg->pinmux_tx.pingrp, cfg->pinmux_tx.pinnum, cfg->pinmux_tx.modefunc);

	/* Set up the UART for the desired baud, and 8N1. */
	Chip_UART_Init(uart);
	Chip_UART_SetBaud(uart, cfg->baudrate);
	Chip_UART_ConfigData(uart, UART_LCR_WLEN8 | UART_LCR_SBS_1BIT | UART_LCR_PARITY_DIS);

	/* Enable Tx; Rx is implicitly enabled on init. */
	Chip_UART_TXEnable(uart);

	/* Disable hardware flow control. */
	/* TODO: Make this configurable! */
	Chip_UART_ClearModemControl(uart, UART_MCR_AUTO_RTS_EN | UART_MCR_AUTO_CTS_EN);

	/* TODO: Support IRQ-driven modes! */

	dev->driver_api = &api;
	return 0;
}
