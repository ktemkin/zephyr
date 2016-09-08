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

/**
 * LPCOpen/Zephyr compatibility definitions.
 */

#include <nanokernel.h>
#include <arch/cpu.h>
#include <stdint.h>

#include <board.h>
#include <init.h>
#include <uart.h>
#include <toolchain.h>
#include <sections.h>

const uint32_t OscRateIn = CONFIG_XTAL_FREQUENCY;
const uint32_t ExtRateIn = CONFIG_CLKIN_FREQUENCY;
