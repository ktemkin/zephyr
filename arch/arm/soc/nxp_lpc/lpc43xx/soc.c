/*
 * Copyright (c) 2014-2015 Wind River Systems, Inc.
 * Copyright (c) 2016, Kyle J. Temkin <kyle@ktemkin.com>
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
#include <device.h>
#include <init.h>
#include <soc.h>
#include <uart.h>
#include <sections.h>
#include <arch/cpu.h>

#include <chip.h>


/**
 * @brief Initialize the core system clock, and set up the main PLL.
 * @return N/A
 */
static void lpc43xx_init_core_clock(void)
{
    /*
     * Determine the system's clock source based on configuration.
     * If an external crystal frequency has been provided, use the xtal.
     */
    CHIP_CGU_CLKIN_T clk_source 
      = CONFIG_XTAL_FREQUENCY ? CLKIN_CRYSTAL : CLKIN_IRC;

    /* If we're using the XTAL, enable it. */
    if(clk_source == CLKIN_CRYSTAL)
        Chip_Clock_EnableCrystal();

    /* Set up the main PLL to generate our core system clock. */
    Chip_Clock_SetupMainPLLHz(clk_source,
        CONFIG_SYSCLK_FREQUENCY - CONFIG_SYSCLK_DELTA,
        CONFIG_SYSCLK_FREQUENCY,
        CONFIG_SYSCLK_FREQUENCY + CONFIG_SYSCLK_DELTA);
    Chip_Clock_SetBaseClock(CLK_BASE_MX, CLKIN_MAINPLL, true, false);
}


/**
 * @brief Initialize the clocks for the system peripherals.
 * @return N/A
 */
static void lpc43xx_init_peripheral_clocks(void)
{
    /* Switch the core peripherals over to the main PLL. */
    Chip_Clock_SetBaseClock(CLK_BASE_PERIPH, CLKIN_MAINPLL, true, false);
    Chip_Clock_SetBaseClock(CLK_BASE_APB1, CLKIN_MAINPLL, true, false);
    Chip_Clock_SetBaseClock(CLK_BASE_APB3, CLKIN_MAINPLL, true, false);

    /* Switch the globally-present peripherals over to the main PLL.*/
    Chip_Clock_SetBaseClock(CLK_BASE_SSP0, CLKIN_MAINPLL, true, false);
    Chip_Clock_SetBaseClock(CLK_BASE_SSP1, CLKIN_MAINPLL, true, false);

    /*
     * All other devices should set up their base clock in their driver's
     * initialization code.
     */
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

    /* Initialize our clocks. */
    lpc43xx_init_core_clock();
    lpc43xx_init_peripheral_clocks();

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
