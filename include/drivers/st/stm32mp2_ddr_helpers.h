/*
 * Copyright (c) 2021, STMicroelectronics - All Rights Reserved
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef STM32MP2_DDR_HELPERS_H
#define STM32MP2_DDR_HELPERS_H

#include <stdbool.h>
#include <stdint.h>

enum stm32mp2_ddr_sr_mode {
	DDR_SR_MODE_INVALID = 0,
	DDR_SSR_MODE,
	DDR_HSR_MODE,
	DDR_ASR_MODE,
};

int ddr_sw_self_refresh_exit(void);
uint32_t ddr_get_io_calibration_val(void);
int ddr_standby_sr_entry(void);
enum stm32mp2_ddr_sr_mode ddr_read_sr_mode(void);
void ddr_set_sr_mode(enum stm32mp2_ddr_sr_mode mode);
void ddr_save_sr_mode(void);
void ddr_restore_sr_mode(void);

#endif /* STM32MP2_DDR_HELPERS_H */