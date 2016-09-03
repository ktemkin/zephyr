/*
 * Copyright (c) 2014-2015 Wind River Systems, Inc.
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
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

/**
 * @file
 * @brief System/hardware module for fsl_frdm_k64f platform
 *
 * This module provides routines to initialize and support board-level
 * hardware for the fsl_frdm_k64f platform.
 */

#include <nanokernel.h>
#include <device.h>
#include <init.h>
#include <soc.h>
#include <uart.h>
#include <sections.h>
#include <arch/cpu.h>


/**
 *
 * @brief Initialize the system clock
 * @return N/A
 *
 */
static ALWAYS_INLINE void clkInit(void)
{
    //TODO
}

/**
 *
 * @brief Perform basic hardware initialization
 *
 * Initialize the interrupt controller device drivers and the
 * Kinetis UART device driver.
 * Also initialize the timer device driver, if required.
 *
 * @return 0
 */

static int lpc43xx_init(struct device *arg)
{
	ARG_UNUSED(arg);

	int oldLevel; /* old interrupt lock level */

	/* disable interrupts */
	oldLevel = irq_lock();

	/* clear all faults */

	_ScbMemFaultAllFaultsReset();
	_ScbBusFaultAllFaultsReset();
	_ScbUsageFaultAllFaultsReset();

	_ScbHardFaultAllFaultsReset();

	/* Initialize PLL/system clock to 120 MHz */
	clkInit();

	/*
	 * install default handler that simply resets the CPU
	 * if configured in the kernel, NOP otherwise
	 */
	NMI_INIT();

	/* restore interrupt state */
	irq_unlock(oldLevel);
	return 0;
}

SYS_INIT(lpc43xx_init, PRIMARY, 0);
