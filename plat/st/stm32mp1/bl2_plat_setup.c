/*
 * Copyright (c) 2015-2023, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/desc_image_load.h>
#include <drivers/clk.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/mmc.h>
#include <drivers/st/bsec.h>
#include <drivers/st/nvmem.h>
#include <drivers/st/regulator_fixed.h>
#include <drivers/st/regulator_gpio.h>
#include <drivers/st/stm32_iwdg.h>
#if STM32MP13
#include <drivers/st/stm32_mce.h>
#endif
#include <drivers/st/stm32_rng.h>
#include <drivers/st/stm32_uart.h>
#include <drivers/st/stm32mp1_clk.h>
#include <drivers/st/stm32mp1_pwr.h>
#include <drivers/st/stm32mp1_ram.h>
#include <drivers/st/stm32mp_pmic.h>
#include <lib/fconf/fconf.h>
#include <lib/fconf/fconf_dyn_cfg_getter.h>
#include <libfdt.h>
#include <lib/mmio.h>
#include <lib/optee_utils.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <platform_def.h>
#include <stm32mp_common.h>
#include <stm32mp1_context.h>
#include <stm32mp1_dbgmcu.h>

#define PLL1_NOMINAL_FREQ_IN_KHZ	650000U /* 650MHz */

#if !STM32MP1_OPTEE_IN_SYSRAM
IMPORT_SYM(uintptr_t, __BSS_START__, BSS_START);
IMPORT_SYM(uintptr_t, __BSS_END__, BSS_END);
IMPORT_SYM(uintptr_t, __DATA_START__, DATA_START);
IMPORT_SYM(uintptr_t, __DATA_END__, DATA_END);
#endif

#if DEBUG
static const char debug_msg[] = {
	"***************************************************\n"
	"** DEBUG ACCESS PORT IS OPEN!                    **\n"
	"** This boot image is only for debugging purpose **\n"
	"** and is unsafe for production use.             **\n"
	"**                                               **\n"
	"** If you see this message and you are not       **\n"
	"** debugging report this immediately to your     **\n"
	"** vendor!                                       **\n"
	"***************************************************\n"
};
#endif

static void print_reset_reason(void)
{
	uint32_t rstsr = mmio_read_32(stm32mp_rcc_base() + RCC_MP_RSTSCLRR);

	if (rstsr == 0U) {
		WARN("Reset reason unknown\n");
		return;
	}

	INFO("Reset reason (0x%x):\n", rstsr);

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) == 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_STDBYRSTF) != 0U) {
			INFO("System exits from STANDBY\n");
			return;
		}

		if ((rstsr & RCC_MP_RSTSCLRR_CSTDBYRSTF) != 0U) {
			INFO("MPU exits from CSTANDBY\n");
			return;
		}
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PORRSTF) != 0U) {
		INFO("  Power-on Reset (rst_por)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_BORRSTF) != 0U) {
		INFO("  Brownout Reset (rst_bor)\n");
		return;
	}

#if STM32MP15
	if ((rstsr & RCC_MP_RSTSCLRR_MCSYSRSTF) != 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
			INFO("  System reset generated by MCU (MCSYSRST)\n");
		} else {
			INFO("  Local reset generated by MCU (MCSYSRST)\n");
		}
		return;
	}
#endif

	if ((rstsr & RCC_MP_RSTSCLRR_MPSYSRSTF) != 0U) {
		INFO("  System reset generated by MPU (MPSYSRST)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_HCSSRSTF) != 0U) {
		INFO("  Reset due to a clock failure on HSE\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG1RSTF) != 0U) {
		INFO("  IWDG1 Reset (rst_iwdg1)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG2RSTF) != 0U) {
		INFO("  IWDG2 Reset (rst_iwdg2)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP0RSTF) != 0U) {
		INFO("  MPU Processor 0 Reset\n");
		return;
	}

#if STM32MP15
	if ((rstsr & RCC_MP_RSTSCLRR_MPUP1RSTF) != 0U) {
		INFO("  MPU Processor 1 Reset\n");
		return;
	}
#endif

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
		INFO("  Pad Reset from NRST\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_VCORERSTF) != 0U) {
		INFO("  Reset due to a failure of VDD_CORE\n");
		return;
	}

	ERROR("  Unidentified reset reason\n");
}

void bl2_el3_early_platform_setup(u_register_t arg0,
				  u_register_t arg1 __unused,
				  u_register_t arg2 __unused,
				  u_register_t arg3 __unused)
{
	stm32mp_setup_early_console();

	stm32mp_save_boot_ctx_address(arg0);
}

void bl2_platform_setup(void)
{
	int ret;

	ret = stm32mp1_ddr_probe();
	if (ret < 0) {
		ERROR("Invalid DDR init: error %d\n", ret);
		panic();
	}

	if (!stm32mp1_ddr_is_restored()) {
#if STM32MP15
		struct nvmem_cell magic_number;
		struct nvmem_cell branch_address;
		uint32_t reg_val = 0;

		stm32_get_magic_number_cell(&magic_number);
		stm32_get_core1_branch_address_cell(&branch_address);

		/* Clear backup register */
		nvmem_cell_write(&branch_address, (uint8_t *)&reg_val,
				 sizeof(reg_val));
		/* Clear backup register magic */
		nvmem_cell_write(&magic_number, (uint8_t *)&reg_val,
				 sizeof(reg_val));
#endif

		/* Clear the context in BKPSRAM */
		stm32_clean_context();
	}

	/* Map DDR for binary load, now with cacheable attribute */
	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				      STM32MP_DDR_MAX_SIZE, MT_MEMORY | MT_RW | MT_SECURE);
	if (ret < 0) {
		ERROR("DDR mapping: error %d\n", ret);
		panic();
	}
}

#if STM32MP15
static void update_monotonic_counter(void)
{
	uint32_t version;
	uint32_t otp;

	CASSERT(STM32_TF_VERSION <= MAX_MONOTONIC_VALUE,
		assert_stm32mp1_monotonic_counter_reach_max);

	/* Check if monotonic counter needs to be incremented */
	if (stm32_get_otp_index(MONOTONIC_OTP, &otp, NULL) != 0) {
		panic();
	}

	if (stm32_get_otp_value_from_idx(otp, &version) != 0) {
		panic();
	}

	if ((version + 1U) < BIT(STM32_TF_VERSION)) {
		uint32_t result;

		/* Need to increment the monotonic counter. */
		version = BIT(STM32_TF_VERSION) - 1U;

		result = bsec_program_otp(version, otp);
		if (result != BSEC_OK) {
			ERROR("BSEC: MONOTONIC_OTP program Error %u\n",
			      result);
			panic();
		}
		INFO("Monotonic counter has been incremented (value 0x%x)\n",
		     version);
	}
}
#endif

static void reset_backup_domain(void)
{
	uintptr_t pwr_base = stm32mp_pwr_base();
	uintptr_t rcc_base = stm32mp_rcc_base();

	/*
	 * Disable the backup domain write protection.
	 * The protection is enable at each reset by hardware
	 * and must be disabled by software.
	 */
	mmio_setbits_32(pwr_base + PWR_CR1, PWR_CR1_DBP);

	while ((mmio_read_32(pwr_base + PWR_CR1) & PWR_CR1_DBP) == 0U) {
		;
	}

	/* Reset backup domain on cold boot cases */
	if ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_RTCSRC_MASK) == 0U) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);

		while ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_VSWRST) == 0U) {
			;
		}

		mmio_clrbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);
	}
}

static void stm32_tamp_check_tamper_event(void)
{
	uint32_t sr = mmio_read_32(TAMP_BASE + TAMP_SR);

	if (sr != 0U) {
		ERROR("\n");
		while (sr != 0U) {
			unsigned int bit_off = __builtin_ctz(sr);
			bool is_internal = bit_off >= TAMP_SR_INT_SHIFT;

			ERROR("** INTRUSION ALERT: %s TAMPER %u DETECTED **\n",
			      is_internal ? "INTERNAL" : "EXTERNAL",
			      is_internal ? (bit_off - TAMP_SR_INT_SHIFT + 1U) : (bit_off + 1U));

			sr &= ~BIT(bit_off);
		}
		ERROR("\n");
	}
}

void bl2_el3_plat_arch_setup(void)
{
	const char *board_model;
	boot_api_context_t *boot_context =
		(boot_api_context_t *)stm32mp_get_boot_ctx_address();

	if (bsec_probe() != 0U) {
		panic();
	}

	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE,
			BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);

	/* Prevent corruption of preloaded Device Tree */
	mmap_add_region(DTB_BASE, DTB_BASE,
			DTB_LIMIT - DTB_BASE,
			MT_RO_DATA | MT_SECURE);

	configure_mmu();

	if (dt_open_and_check(STM32MP_DTB_BASE) < 0) {
		panic();
	}

	reset_backup_domain();

	/*
	 * Set minimum reset pulse duration to 31ms for discrete power
	 * supplied boards.
	 */
	if (dt_pmic_status() <= 0) {
		mmio_clrsetbits_32(stm32mp_rcc_base() + RCC_RDLSICR,
				   RCC_RDLSICR_MRD_MASK,
				   31U << RCC_RDLSICR_MRD_SHIFT);
	}

	generic_delay_timer_init();

#if STM32MP_UART_PROGRAMMER
	/* Disable programmer UART before changing clock tree */
	if (boot_context->boot_interface_selected ==
	    BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART) {
		uintptr_t uart_prog_addr =
			get_uart_address(boot_context->boot_interface_instance);

		stm32_uart_stop(uart_prog_addr);
	}
#endif
	if (stm32mp1_clk_probe() < 0) {
		panic();
	}

	if (stm32mp1_clk_init(PLL1_NOMINAL_FREQ_IN_KHZ) < 0) {
		panic();
	}

	stm32_tamp_nvram_init();

	stm32_save_boot_info(boot_context);

#if STM32MP_USB_PROGRAMMER && STM32MP15
	/* Deconfigure all UART RX pins configured by ROM code */
	stm32mp1_deconfigure_uart_pins();
#endif

	if (stm32mp_uart_console_setup() != 0) {
		goto skip_console_init;
	}

	/* Enter in boot mode */
	stm32mp_syscfg_boot_mode_enable();

	stm32mp_print_cpuinfo();

	board_model = dt_get_board_model();
	if (board_model != NULL) {
		NOTICE("Model: %s\n", board_model);
	}

	stm32mp_print_boardinfo();

	if (boot_context->auth_status != BOOT_API_CTX_AUTH_NO) {
		NOTICE("Bootrom authentication %s\n",
		       (boot_context->auth_status == BOOT_API_CTX_AUTH_FAILED) ?
		       "failed" : "succeeded");
	}

skip_console_init:
	stm32_tamp_check_tamper_event();

#if !TRUSTED_BOARD_BOOT
	if (stm32mp_check_closed_device() == STM32MP_CHIP_SEC_CLOSED) {
		/* Closed chip mandates authentication */
		ERROR("Secure chip: TRUSTED_BOARD_BOOT must be enabled\n");
		panic();
	}
#endif

	if (fixed_regulator_register() != 0) {
		panic();
	}

#if (PLAT_NB_GPIO_REGUS > 0)
	if (gpio_regulator_register() != 0) {
		panic();
	}
#endif

	if (dt_pmic_status() > 0) {
		initialize_pmic();
		if (!stm32mp_is_wakeup_from_standby() &&
		    pmic_voltages_init() != 0) {
			ERROR("PMIC voltages init failed\n");
			panic();
		}
		print_pmic_info_and_debug();
	}

	stm32mp_syscfg_init();

	if (stm32_iwdg_init() < 0) {
		panic();
	}

	stm32_iwdg_refresh();

	if (bsec_read_debug_conf() != 0U) {
		if (stm32mp_check_closed_device() == STM32MP_CHIP_SEC_CLOSED) {
#if DEBUG
			WARN("\n%s", debug_msg);
#else
			ERROR("***Debug opened on closed chip***\n");
#endif
		}
	}

#if STM32MP13
	if (stm32_rng_init() != 0) {
		panic();
	}
#endif

	stm32mp1_arch_security_setup();

	print_reset_reason();

#if STM32MP15
	update_monotonic_counter();
#endif

	stm32mp_syscfg_enable_io_compensation_finish();

	fconf_populate("TB_FW", STM32MP_DTB_BASE);

	if (stm32mp_skip_boot_device_after_standby()) {
		bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(FW_CONFIG_ID);

		assert(bl_mem_params != NULL);

		bl_mem_params->image_info.h.attr |= IMAGE_ATTRIB_SKIP_LOADING;
	} else {
		stm32mp_io_setup();
	}
}

#if STM32MP13
static void prepare_encryption(void)
{
	uint8_t mkey[MCE_KEY_SIZE_IN_BYTES];

	stm32_mce_init();

#if STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER
	if (stm32_rng_read(mkey, MCE_KEY_SIZE_IN_BYTES) != 0) {
		panic();
	}
#else /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */
	if (stm32mp_is_wakeup_from_standby()) {
		stm32mp1_pm_get_mce_mkey_from_context(mkey);
		stm32_mce_reload_configuration();
	} else {
		/* Generate MCE master key from RNG */
		if (stm32_rng_read(mkey, MCE_KEY_SIZE_IN_BYTES) != 0) {
			panic();
		}

		stm32mp1_pm_save_mce_mkey_in_context(mkey);
	}
#endif /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */

	if (stm32_mce_write_master_key(mkey) != 0) {
		panic();
	}

	stm32_mce_lock_master_key();
}
#endif

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
	bl_mem_params_node_t *bl32_mem_params;
	bl_mem_params_node_t *pager_mem_params __unused;
	bl_mem_params_node_t *paged_mem_params __unused;
	const struct dyn_cfg_dtb_info_t *config_info;
	bl_mem_params_node_t *tos_fw_mem_params;
	unsigned int i;
	unsigned int idx;
	unsigned long long ddr_top __unused;
#if STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER
	bool wakeup_ddr_sr = false;
#else /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */
	bool wakeup_ddr_sr = stm32mp1_ddr_is_restored();
#endif /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */
	const unsigned int image_ids[] = {
		BL32_IMAGE_ID,
		BL33_IMAGE_ID,
		HW_CONFIG_ID,
		TOS_FW_CONFIG_ID,
	};

	assert(bl_mem_params != NULL);

	switch (image_id) {
	case FW_CONFIG_ID:
#if STM32MP13
		if ((stm32mp_check_closed_device() == STM32MP_CHIP_SEC_CLOSED) ||
		    stm32mp_is_auth_supported()) {
			prepare_encryption();
		}
#endif
		if (stm32mp_skip_boot_device_after_standby()) {
			return 0;
		}

		/* Set global DTB info for fixed fw_config information */
		set_config_info(STM32MP_FW_CONFIG_BASE, ~0UL, STM32MP_FW_CONFIG_MAX_SIZE,
				FW_CONFIG_ID);
		fconf_populate("FW_CONFIG", STM32MP_FW_CONFIG_BASE);

		idx = dyn_cfg_dtb_info_get_index(TOS_FW_CONFIG_ID);

		/* Iterate through all the fw config IDs */
		for (i = 0U; i < ARRAY_SIZE(image_ids); i++) {
			if ((image_ids[i] == TOS_FW_CONFIG_ID) && (idx == FCONF_INVALID_IDX)) {
				continue;
			}

			bl_mem_params = get_bl_mem_params_node(image_ids[i]);
			assert(bl_mem_params != NULL);

			config_info = FCONF_GET_PROPERTY(dyn_cfg, dtb, image_ids[i]);
			if (config_info == NULL) {
				continue;
			}

			bl_mem_params->image_info.image_base = config_info->config_addr;
			bl_mem_params->image_info.image_max_size = config_info->config_max_size;

			/*
			 * If going back from CSTANDBY / STANDBY and DDR was in Self-Refresh,
			 * DDR partitions must not be reloaded.
			 */
			if (!(wakeup_ddr_sr && (config_info->config_addr >= STM32MP_DDR_BASE))) {
				bl_mem_params->image_info.h.attr &= ~IMAGE_ATTRIB_SKIP_LOADING;
			}

			switch (image_ids[i]) {
			case BL32_IMAGE_ID:
				bl_mem_params->ep_info.pc = config_info->config_addr;

				/* In case of OPTEE, initialize address space with tos_fw addr */
				pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
				assert(pager_mem_params != NULL);
				pager_mem_params->image_info.image_base = config_info->config_addr;
				pager_mem_params->image_info.image_max_size =
					config_info->config_max_size;

				/* Init base and size for pager if exist */
				paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
				if (paged_mem_params != NULL) {
					paged_mem_params->image_info.image_base = STM32MP_DDR_BASE +
						(dt_get_ddr_size() - STM32MP_DDR_S_SIZE);
					paged_mem_params->image_info.image_max_size =
						STM32MP_DDR_S_SIZE;
				}
				break;

			case BL33_IMAGE_ID:
				if (wakeup_ddr_sr) {
					/*
					 * Set ep_info PC to 0, to inform BL32 it is a reset
					 * after STANDBY
					 */
					bl_mem_params->ep_info.pc = 0U;
				} else {
					bl_mem_params->ep_info.pc = config_info->config_addr;
				}
				break;

			case HW_CONFIG_ID:
			case TOS_FW_CONFIG_ID:
				break;

			default:
				return -EINVAL;
			}
		}
		break;

	case BL32_IMAGE_ID:
#if !STM32MP_UART_PROGRAMMER && !STM32MP_USB_PROGRAMMER
		if (wakeup_ddr_sr && stm32mp_skip_boot_device_after_standby()) {
			bl_mem_params->ep_info.pc = stm32_pm_get_optee_ep();
			if (stm32mp1_addr_inside_backupsram(bl_mem_params->ep_info.pc)) {
				clk_enable(BKPSRAM);
			}
			break;
		}
#endif /* !STM32MP_UART_PROGRAMMER && !STM32MP_USB_PROGRAMMER */

		if (optee_header_is_valid(bl_mem_params->image_info.image_base)) {
			image_info_t *paged_image_info = NULL;

			/* BL32 is OP-TEE header */
			bl_mem_params->ep_info.pc = bl_mem_params->image_info.image_base;

			pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
			assert(pager_mem_params != NULL);

			paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
			if (paged_mem_params != NULL) {
				paged_image_info = &paged_mem_params->image_info;
			}

			err = parse_optee_header(&bl_mem_params->ep_info,
						 &pager_mem_params->image_info,
						 paged_image_info);
			if (err != 0) {
				ERROR("OPTEE header parse error.\n");
				panic();
			}

			/* Set optee boot info from parsed header data */
			if (paged_mem_params != NULL) {
				bl_mem_params->ep_info.args.arg0 =
					paged_mem_params->image_info.image_base;
			} else {
				bl_mem_params->ep_info.args.arg0 = 0U;
			}

			bl_mem_params->ep_info.args.arg1 = 0U; /* Unused */
			bl_mem_params->ep_info.args.arg2 = 0U; /* No DT supported */
		} else {
			bl_mem_params->ep_info.pc = bl_mem_params->image_info.image_base;
			tos_fw_mem_params = get_bl_mem_params_node(TOS_FW_CONFIG_ID);
			assert(tos_fw_mem_params != NULL);
			bl_mem_params->image_info.image_max_size +=
				tos_fw_mem_params->image_info.image_max_size;
			bl_mem_params->ep_info.args.arg0 = 0;
		}

		if (bl_mem_params->ep_info.pc >= STM32MP_DDR_BASE) {
			stm32_context_save_bl2_param();
		}
		break;

	case BL33_IMAGE_ID:
		bl32_mem_params = get_bl_mem_params_node(BL32_IMAGE_ID);
		assert(bl32_mem_params != NULL);
		bl32_mem_params->ep_info.lr_svc = bl_mem_params->ep_info.pc;
#if PSA_FWU_SUPPORT
		if (plat_fwu_is_enabled()) {
			stm32_fwu_set_boot_idx();
		}
#endif /* PSA_FWU_SUPPORT */
		break;

	default:
		/* Do nothing in default case */
		break;
	}

#if STM32MP_SDMMC || STM32MP_EMMC
	/*
	 * Invalidate remaining data read from MMC but not flushed by load_image_flush().
	 * We take the worst case which is 2 MMC blocks.
	 */
	if ((image_id != FW_CONFIG_ID) &&
	    ((bl_mem_params->image_info.h.attr & IMAGE_ATTRIB_SKIP_LOADING) == 0U)) {
		inv_dcache_range(bl_mem_params->image_info.image_base +
				 bl_mem_params->image_info.image_size,
				 2U * MMC_BLOCK_SIZE);
	}
#endif /* STM32MP_SDMMC || STM32MP_EMMC */

	return err;
}

void bl2_el3_plat_prepare_exit(void)
{
#if STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER
	uint16_t boot_itf = stm32mp_get_boot_itf_selected();

	if ((boot_itf == BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART) ||
	    (boot_itf == BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_USB)) {
		/* Invalidate the downloaded buffer used with io_memmap */
		inv_dcache_range(DWL_BUFFER_BASE, DWL_BUFFER_SIZE);
	}
#endif /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */

#if !STM32MP1_OPTEE_IN_SYSRAM
	flush_dcache_range(BSS_START, BSS_END - BSS_START);
	flush_dcache_range(DATA_START, DATA_END - DATA_START);
#endif

#if !defined(DECRYPTION_SUPPORT_none)
	if (stm32_lock_enc_key_otp() != 0) {
		panic();
	}
#endif

	stm32mp1_security_setup();

	/* end of boot mode */
	stm32mp_syscfg_boot_mode_disable();
}
