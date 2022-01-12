/******************************************************************************
* Copyright (c) 2019 - 2021 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/


#include "xplmi_util.h"
#include "xplmi_dma.h"
#include "xpm_common.h"
#include "xpm_aie.h"
#include "xpm_regs.h"
#include "xpm_bisr.h"
#include "xpm_device.h"
#include "xpm_debug.h"

#define AIE_POLL_TIMEOUT 0X1000000U

#define COL_SHIFT 23U
#define ROW_SHIFT 18U
#define AIE1_TILE_BADDR(NocAddr, col, row)	\
	(((u64)(NocAddr)) + ((u64)(col) << COL_SHIFT) + ((u64)(row) << ROW_SHIFT))

#define AIE2_COL_SHIFT 25U
#define AIE2_ROW_SHIFT 20U
#define AIE2_TILE_BADDR(NocAddr, col, row)	\
	(((u64)(NocAddr)) + ((u64)(col) << AIE2_COL_SHIFT) + ((u64)(row) << AIE2_ROW_SHIFT))

#define AIE_CORE_STATUS_DONE_MASK   (1UL<<20U)

#define XPM_AIE_OPS                    0U
#define XPM_AIE2_OPS                   1U
#define XPM_AIE_OPS_MAX                2U

#define AieWrite64(addr, val) swea(addr, val)
#define AieRead64(addr) lwea(addr)

static inline void AieRMW64(u64 addr, u32 Mask, u32 Value)
{
	u32 l_val;
	l_val = AieRead64(addr);
	l_val = (l_val & (~Mask)) | (Mask & Value);
	AieWrite64(addr, l_val);
}

/* Buffer to hold AIE data memory zeroization elf */
/**
 * NOTE: If ProgramMem[] is updated in future, then check if the current
 * AieWaitForCoreDone() implementation is still valid or needs to be
 * updated to use events.
 */
static const u32 ProgramMem[] __attribute__ ((aligned(16))) = {
	0x0600703fU,
	0x0a000804U,
	0x000018c0U,
	0x603803f7U,
	0x00000203U,
	0x400c9803U,
	0x13201803U,
	0x31009803U,
	0x200003f7U,
	0x00000277U,
	0x800003f7U,
	0x00000257U,
	0x00000000U,
	0x39200000U,
	0x0000003dU,
	0x00000000U,
	0x00000000U,
	0x00000000U,
	0x40000000U,
	0x00001888U,
	0x00000000U,
	0x00000000U,
	0x00000000U,
	0x0000079aU,
	0x00000000U,
	0x00000000U,
	0x00000000U,
	0x00002614U,
	0x00000000U,
	0x00000000U,
	0x07428800U,
	0x00000000U,
	0x00010001U,
	0x00010001U,
	0x00030001U,
	0x00011000U,
	0x00010001U,
	0x00010001U,
	0x00010001U,
	0x00010001U,
	};

/*****************************************************************************/
/**
 * This function is used to set/clear bits in AIE PCSR
 *
 * @param Mask Mask to be written into PCSR_MASK register
 * @param Value Value to be written into PCSR_CONTROL register
 * @return
 *****************************************************************************/
static XStatus AiePcsrWrite(u32 Mask, u32 Value)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	PmOut32((BaseAddress + NPI_PCSR_MASK_OFFSET), Mask);
	/* Check mask value again for blind write check */
	PmChkRegOut32(((BaseAddress + NPI_PCSR_MASK_OFFSET)), Mask, Status);
	if (XPM_REG_WRITE_FAILED == Status) {
		DbgErr = XPM_INT_ERR_REG_WRT_NPI_PCSR_MASK;
		goto done;
	}

	PmOut32((BaseAddress + NPI_PCSR_CONTROL_OFFSET), Value);
	/* Check control value again for blind write check */
	PmChkRegMask32((BaseAddress + NPI_PCSR_CONTROL_OFFSET), Mask, Value, Status);
	if (XPM_REG_WRITE_FAILED == Status) {
		DbgErr = XPM_INT_ERR_REG_WRT_NPI_PCSR_CONTROL;
		goto done;
	}

	Status = XST_SUCCESS;

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

/*****************************************************************************/
/**
 * This function provides a delay for specified duration
 *
 * @param MicroSeconds Duration in micro seconds
 * @return
 *****************************************************************************/
static inline void AieWait(u32 MicroSeconds)
{
	usleep(MicroSeconds);
}


/*****************************************************************************/
/**
 * This function is used to enable AIE Core
 *
 * @param AieDomain Handle to AIE domain instance
 * @param Col Column index of the Core
 * @param Row Row index of the Core
 *
 * @return N/A
 *****************************************************************************/
static void AieCoreEnable(const XPm_AieDomain *AieDomain, u32 Col, u32 Row)
{
	u64 TileBaseAddress = AIE1_TILE_BADDR(AieDomain->Array.NocAddress, Col, Row);

	/* Release reset to the Core */
	AieWrite64(TileBaseAddress + AIE_CORE_CONTROL_OFFSET, 0U);

	/* Enable the Core */
	AieWrite64(TileBaseAddress + AIE_CORE_CONTROL_OFFSET, 1U);
}

/*****************************************************************************/
/**
 * This function waits for a Core's DONE bit to be set
 *
 * @param AieDomain Handle to AIE domain instance
 * @param Col Column index of the Core
 * @param Row Row index of the Core
 *
 * @return Status Code
 *****************************************************************************/
static XStatus AieWaitForCoreDone(const XPm_AieDomain *AieDomain, u32 Col, u32 Row)
{
	u64 TileBaseAddress = AIE1_TILE_BADDR(AieDomain->Array.NocAddress, Col, Row);
	u64 StatusRegAddr = TileBaseAddress + AIE_CORE_STATUS_OFFSET;
	XStatus Status = XST_FAILURE;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	Status = XPlmi_UtilPollForMask64((u32)(StatusRegAddr>>32),
				(u32)(StatusRegAddr), AIE_CORE_STATUS_DONE_MASK, 10U);
	if (Status != XST_SUCCESS) {
		DbgErr = XPM_INT_ERR_AIE_CORE_STATUS_TIMEOUT;
		PmInfo("ERROR: Poll for Done timeout \r\n");
	}

	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

/*****************************************************************************/
/**
 * This function loads a core's program memory with zeroization elf
 *
 * @param AieDomain Handle to AIE domain instance
 * @param Col Column index of the Core
 * @param Row Row index of the Core
 *
 * @return Status Code
 *****************************************************************************/
static XStatus ProgramCore(const XPm_AieDomain *AieDomain, u32 Col, u32 Row,
			   const u32 *PrgData, u32 NumOfWords)
{
	u64 TileBaseAddress = AIE1_TILE_BADDR(AieDomain->Array.NocAddress, Col, Row);
	u64 PrgAddr = TileBaseAddress + AIE_PROGRAM_MEM_OFFSET;

	return XPlmi_DmaXfr((u64)(0U)|(u32)PrgData, PrgAddr, NumOfWords, XPLMI_PMCDMA_0);
}

/*****************************************************************************/
/**
 * This function is used to cycle reset to the entire AIE array
 *
 * @return
 *****************************************************************************/
static XStatus ArrayReset(void)
{
	XStatus Status = XST_FAILURE;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_ARRAY_RESET_MASK,
					ME_NPI_REG_PCSR_MASK_ME_ARRAY_RESET_MASK);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_ARRAY_RESET;
		goto done;
	}

	/* Wait for reset to propagate (1us) */
	AieWait(1U);

	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_ARRAY_RESET_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_ARRAY_RESET_RELEASE;
	}

	/* Wait for reset to propagate (1us) */
	AieWait(1U);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

/*****************************************************************************/
/**
 * This function is used to scrub ECC enabled memories in the entire AIE array
 * Parameter: Action: ECC_SCRUB_DISABLE - Disable ECC scrub (disable PMEM Scrub using False event in all the Tiles)
 *		      ECC_SCRUB_ENABLE  - Enable ECC scrub (enable PMEM Scrub using True event in all the Tiles)
 *
 * @return
 *****************************************************************************/
static void TriggerEccScrub(const XPm_AieDomain *AieDomain, u32 Action)
{
	u16 StartCol = AieDomain->Array.StartCol;
	u16 EndCol = StartCol + AieDomain->Array.NumCols;
	u16 StartRow = AieDomain->Array.StartRow;
	u16 EndRow = StartRow + AieDomain->Array.NumRows;

	for (u16 col = StartCol; col < EndCol; col++) {
		for (u16 row = StartRow; row < EndRow; row++) {
			u64 TileBaseAddress = AIE1_TILE_BADDR(AieDomain->Array.NocAddress, col, row);
			AieWrite64(TileBaseAddress + AIE_CORE_ECC_SCRUB_EVENT_OFFSET, Action);
		}
	}
}

/*****************************************************************************/
/**
 * This function clock gates ME tiles clock column-wise
 *
 * @param AieDomain Handle to AIE domain instance
 *
 * @return N/A
 *****************************************************************************/
static void AieClkGateByCol(const XPm_AieDomain *AieDomain)
{
	u16 StartCol = AieDomain->Array.StartCol;
	u16 EndCol = StartCol + AieDomain->Array.NumCols;
	u16 StartRow = 0U;	/* Shim row is always row zero */
	u16 EndRow = AieDomain->Array.NumShimRows;

	for (u16 row = StartRow; row < EndRow; ++row) {
		for (u16 col = StartCol; col < EndCol; ++col) {
			u64 TileBaseAddress = AIE1_TILE_BADDR(AieDomain->Array.NocAddress, col, row);
			AieRMW64(TileBaseAddress + AIE_TILE_CLOCK_CONTROL_OFFSET,
				AIE_TILE_CLOCK_CONTROL_CLK_BUFF_EN_MASK,
				~AIE_TILE_CLOCK_CONTROL_CLK_BUFF_EN_MASK);
		}
	}
}

static XStatus AieCoreMemInit(const XPm_AieDomain *AieDomain)
{
	u16 StartCol = AieDomain->Array.StartCol;
	u16 EndCol = StartCol + AieDomain->Array.NumCols;
	u16 StartRow = AieDomain->Array.StartRow;
	u16 EndRow = StartRow + AieDomain->Array.NumRows;
	XStatus Status;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	for (u16 col = StartCol; col < EndCol; col++) {
		for (u16 row = StartRow; row < EndRow; row++) {
			PmDbg("---------- (%d, %d)----------\r\n", col, row);
			Status = ProgramCore(AieDomain, col, row,
					     &ProgramMem[0], ARRAY_SIZE(ProgramMem));
			if (XST_SUCCESS != Status) {
				DbgErr = XPM_INT_ERR_PRGRM_CORE;
				goto done;
			}

			AieCoreEnable(AieDomain, col, row);
		}
	}

	/**
	 * NOTE: In future, if contents of ProgramMem[] are changed due to an
	 * updated AIE elf generated by latest tools, then the below check for
	 * core DONE may not work. Latest tools use events instead of DONE bit.
	 */
	Status = AieWaitForCoreDone(AieDomain, (u32)EndCol - 1U, (u32)EndRow - 1U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_AIE_CORE_STATUS_TIMEOUT;
	}

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static void Aie2ClockInit(const XPm_AieDomain *AieDomain, u32 BaseAddress)
{
	u16 StartCol = AieDomain->Array.StartCol;
	u16 EndCol = StartCol + AieDomain->Array.NumCols;

	/* Enable privileged write access */
	XPm_RMW32(BaseAddress + AIE2_NPI_ME_PROT_REG_CTRL_OFFSET,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK);

	/* Enable all column clocks */
	for (u16 col = StartCol; col < EndCol; col++) {
	AieWrite64(AIE2_TILE_BADDR(AieDomain->Array.NocAddress, col, 0) +
				AIE2_PL_MODULE_COLUMN_CLK_CTRL_OFFSET, 1U);
	}

	/* Disable privileged write access */
	XPm_RMW32(BaseAddress + AIE2_NPI_ME_PROT_REG_CTRL_OFFSET,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK, 0U);
}

static void Aie2ClockGate(const XPm_AieDomain *AieDomain, u32 BaseAddress)
{
	u16 StartCol = AieDomain->Array.StartCol;
	u16 EndCol = StartCol + AieDomain->Array.NumCols;

	/* Enable privileged write access */
	XPm_RMW32(BaseAddress + AIE2_NPI_ME_PROT_REG_CTRL_OFFSET,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK);

	/* Disable all column clocks */
	for (u16 col = StartCol; col < EndCol; col++) {
		AieWrite64(AIE2_TILE_BADDR(AieDomain->Array.NocAddress, col, 0) +
				AIE2_PL_MODULE_COLUMN_CLK_CTRL_OFFSET, 0U);
	}

	/* Disable privileged write access */
	XPm_RMW32(BaseAddress + AIE2_NPI_ME_PROT_REG_CTRL_OFFSET,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK, 0U);

}

static XStatus AieInitStart(XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	u32 DisableMask;
	XPm_AieDomain *AieDomain = (XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/* Use AIE NoC Address if available */
	if (2U <= NumOfArgs) {
		AieDomain->Array.NocAddress = ((u64)Args[1] << 32U) | (Args[0]);
		PmDbg("AIE: NoC Address: 0x%x%08x\r\n",
				(u32)(AieDomain->Array.NocAddress >> 32U),
				(u32)(AieDomain->Array.NocAddress));
	}

	/* Check for ME Power Status */
	if( (XPm_In32(BaseAddress + NPI_PCSR_STATUS_OFFSET) &
			 ME_NPI_REG_PCSR_STATUS_ME_PWR_SUPPLY_MASK) !=
			 ME_NPI_REG_PCSR_STATUS_ME_PWR_SUPPLY_MASK) {
		DbgErr = XPM_INT_ERR_POWER_SUPPLY;
		goto done;
	}

	/* Unlock ME PCSR */
	XPmAieDomain_UnlockPcsr(BaseAddress);

	/* Relelase IPOR */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_IPOR_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_RST_RELEASE;
		goto fail;
	}

	/**
	 * Configure ME_TOP_ROW:
	 *	- ROW_OFFSET = 0
	 *	- ME_TOP_ROW = Total number of rows in the array
	 */
	PmOut32((BaseAddress + ME_NPI_ME_TOP_ROW_OFFSET), AieDomain->Array.NumRows);

	/* Get houseclean disable mask */
	DisableMask = XPm_In32(PM_HOUSECLEAN_DISABLE_REG_2) >> HOUSECLEAN_AIE_SHIFT;

	/* Set Houseclean Mask */
	PwrDomain->HcDisableMask |= DisableMask;

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock ME PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus Aie2InitStart(XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	u32 DisableMask;
	XPm_AieDomain *AieDomain = (XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const Aie2Dev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == Aie2Dev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = Aie2Dev->Node.BaseAddress;

	/* Use AIE NoC Address if available */
	if (2U <= NumOfArgs) {
		AieDomain->Array.NocAddress = ((u64)Args[1] << 32U) | (Args[0]);
		PmDbg("AIE: NoC Address: 0x%x%08x\r\n",
				(u32)(AieDomain->Array.NocAddress >> 32U),
				(u32)(AieDomain->Array.NocAddress));
	}

	/* Check for AIE2 Power Status */
	if ((XPm_In32(BaseAddress + NPI_PCSR_STATUS_OFFSET) &
			ME_NPI_REG_PCSR_STATUS_ME_PWR_SUPPLY_MASK) !=
			ME_NPI_REG_PCSR_STATUS_ME_PWR_SUPPLY_MASK) {
		DbgErr = XPM_INT_ERR_POWER_SUPPLY;
		goto done;
	}

	/* Unlock AIE PCSR */
	XPmAieDomain_UnlockPcsr(BaseAddress);

	/* Release IPOR */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_IPOR_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_RST_RELEASE;
		goto fail;
	}

	/**
	 * Configure ME_TOP_ROW:
	 *	- ROW_OFFSET = 0
	 *	- ME_TOP_ROW = Total number of rows in the array
	 */
	PmOut32((BaseAddress + ME_NPI_ME_TOP_ROW_OFFSET), AieDomain->Array.NumRows);

	/* Change from AIE to AIE2. AIE handles in CDO */
	/* De-assert INIT_STATE */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_INITSTATE_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_AIE_INITSTATE_RELEASE;
		goto fail;
	}

	/* Change from AIE to AIE2. AIE handles in CDO */
	/* De-assert AIE2 array reset */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_ARRAY_RESET_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_ARRAY_RESET_RELEASE;
		goto fail;
	}

	/* Get houseclean disable mask */
	DisableMask = XPm_In32(PM_HOUSECLEAN_DISABLE_REG_2) >> HOUSECLEAN_AIE_SHIFT;

	/* Set Houseclean Mask */
	PwrDomain->HcDisableMask |= DisableMask;

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock AIE PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus AieInitFinish(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/* Set PCOMPLETE bit */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_PCOMPLETE_MASK,
				 ME_NPI_REG_PCSR_MASK_PCOMPLETE_MASK);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_AIE_PCOMPLETE;
		goto done;
	}

	/* Clock gate ME Array column-wise (except SHIM array) */
	AieClkGateByCol(AieDomain);

done:
	if (0U != BaseAddress) {
		/* Lock ME PCSR */
		XPmAieDomain_LockPcsr(BaseAddress);
	}

	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus Aie2InitFinish(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/* Change from AIE to AIE2. */
	/* Clock gate for each column */
	Aie2ClockGate(AieDomain, BaseAddress);

	/* Change from AIE to AIE2. PCOMPLETE should be set at the end of the
	 * sequence */
	/* Set PCOMPLETE bit */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_PCOMPLETE_MASK,
			ME_NPI_REG_PCSR_MASK_PCOMPLETE_MASK);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_AIE_PCOMPLETE;
	}

done:
	if (0U != BaseAddress) {
		/* Lock AIE PCSR */
		XPmAieDomain_LockPcsr(BaseAddress);
	}

	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus AieScanClear(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/* De-assert ODISABLE[1] */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ODISABLE_1_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_ODISABLE_1_RELEASE;
		goto fail;
	}

	if (HOUSECLEAN_DISABLE_SCAN_CLEAR_MASK != (PwrDomain->HcDisableMask &
				HOUSECLEAN_DISABLE_SCAN_CLEAR_MASK)) {
		PmInfo("Triggering ScanClear for power node 0x%x\r\n", PwrDomain->Power.Node.Id);

		/* Trigger Scan Clear */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_SCAN_CLEAR_TRIGGER_MASK,
						ME_NPI_REG_PCSR_MASK_SCAN_CLEAR_TRIGGER_MASK);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_SCAN_CLEAR_TRIGGER;
			goto fail;
		}

		XPlmi_Printf(DEBUG_INFO, "INFO: %s : Wait for AIE Scan Clear complete...", __func__);

		/* Wait for Scan Clear DONE */
		Status = XPm_PollForMask(BaseAddress + NPI_PCSR_STATUS_OFFSET,
					 ME_NPI_REG_PCSR_STATUS_SCAN_CLEAR_DONE_MASK,
					 AIE_POLL_TIMEOUT);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_SCAN_CLEAR_TIMEOUT;
			XPlmi_Printf(DEBUG_INFO, "ERROR\r\n");
			goto fail;
		}
		else {
			XPlmi_Printf(DEBUG_INFO, "DONE\r\n");
		}

		/* Check Scan Clear PASS */
		if( (XPm_In32(BaseAddress + NPI_PCSR_STATUS_OFFSET) &
		     ME_NPI_REG_PCSR_STATUS_SCAN_CLEAR_PASS_MASK) !=
		    ME_NPI_REG_PCSR_STATUS_SCAN_CLEAR_PASS_MASK) {
			DbgErr = XPM_INT_ERR_SCAN_CLEAR_PASS;
			XPlmi_Printf(DEBUG_GENERAL, "ERROR: %s: AIE Scan Clear FAILED\r\n", __func__);
			Status = XST_FAILURE;
			goto fail;
		}

		/* Unwrite trigger bits */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_SCAN_CLEAR_TRIGGER_MASK, 0);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_SCAN_CLEAR_TRIGGER_UNSET;
			goto fail;
		}
	} else {
		/* ScanClear is skipped */
		PmInfo("Skipping ScanClear for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
	}

	/* De-assert ODISABLE[0] */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ODISABLE_0_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_ODISABLE_0_RELEASE;
		goto fail;
	}

	/* De-assert GATEREG */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_GATEREG_MASK, 0U);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_GATEREG_UNSET;
		goto fail;
	}

	/**
	 * Call post scan clear hook for AIE power domain, if available.
	 * A couple things to note:
	 *	- NPI space must already be unlocked before calling the hook (which it is)
	 *	- If failure occurs within the hook, NPI space must be locked in caller
	 */
	if (NULL != AieDomain->Hooks.PostScanClearHook) {
		Status = AieDomain->Hooks.PostScanClearHook(AieDomain, BaseAddress);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_AIE_POST_SCAN_CLEAR_HOOK;
			goto fail;
		}
	}

	 /*
	  * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	  * space shall remain unlocked for entire housecleaning sequence unless
	  * failure occurs.
	  */
	goto done;

fail:
	/* Lock ME PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus AiePostScanClearHook(const XPm_AieDomain *AieDomain, u32 BaseAddress)
{
	XStatus Status = XST_FAILURE;

	(void)AieDomain;
	(void)BaseAddress;

	/* De-assert INIT_STATE */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_INITSTATE_MASK, 0U);

	return Status;
}

static XStatus AiePreBisrHook(const XPm_AieDomain *AieDomain, u32 BaseAddress)
{
	XStatus Status = XST_FAILURE;

	(void)AieDomain;

	/* Config AIE SMID: ME_SMID_REG.ME_SMID[4:0]=0x1f */
	PmOut32((BaseAddress + ME_NPI_ME_SMID_REG), 0x1FU);

	/* Make AIE block non-secure: ME_SECURE_REG.ME_SECURE[0]=0x0 */
	PmOut32((BaseAddress + ME_NPI_ME_SECURE_REG), 0U);

	/* De-assert AIE array reset */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_ARRAY_RESET_MASK, 0U);

	return Status;
}

static XStatus AieBisr(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/**
	 * Call pre bisr hook for AIE power domain, if available.
	 * A couple things to note:
	 *	- NPI space must already be unlocked before calling the hook (which it is)
	 *	- If failure occurs within the hook, NPI space must be locked in caller
	 */
	if (NULL != AieDomain->Hooks.PreBisrHook) {
		Status = AieDomain->Hooks.PreBisrHook(AieDomain, BaseAddress);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_AIE_PRE_BISR_HOOK;
			goto fail;
		}
	}

	/* Remove PMC-NoC domain isolation */
	Status = XPmDomainIso_Control((u32)XPM_NODEIDX_ISO_PMC_SOC, FALSE_VALUE);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_PMC_SOC_ISO;
		goto fail;
	}

	if (HOUSECLEAN_DISABLE_BISR_MASK != (PwrDomain->HcDisableMask &
				HOUSECLEAN_DISABLE_BISR_MASK)) {
		PmInfo("Triggering BISR for power node 0x%x\r\n", PwrDomain->Power.Node.Id);

		Status = XPmBisr_Repair(MEA_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEA_BISR_REPAIR;
			goto fail;
		}

		Status = XPmBisr_Repair(MEB_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEB_BISR_REPAIR;
			goto fail;
		}

		Status = XPmBisr_Repair(MEC_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEC_BISR_REPAIR;
			goto fail;
		}
	} else {
		/* BISR is skipped */
		PmInfo("Skipping BISR for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
	}

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock ME PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus Aie2PreBisrHook(const XPm_AieDomain *AieDomain, u32 BaseAddress)
{
	XStatus Status = XST_FAILURE;

	(void)AieDomain;

	/* Assert AIE2 Shim Reset */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_ME_SHIM_RESET_MASK,
				ME_NPI_REG_PCSR_MASK_ME_SHIM_RESET_MASK);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	/* Config AIE SMID: ME_SMID_REG.ME_SMID[4:0]=0x1f */
	PmOut32((BaseAddress + ME_NPI_ME_SMID_REG), 0x1FU);

	/* Make AIE block non-secure: ME_SECURE_REG.ME_SECURE[0]=0x0 */
	PmOut32((BaseAddress + ME_NPI_ME_SECURE_REG), 0U);

done:
	return Status;
}

static XStatus Aie2Bisr(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/**
	 * Call pre bisr hook for AIE power domain, if available.
	 * A couple things to note:
	 *	- NPI space must already be unlocked before calling the hook (which it is)
	 *	- If failure occurs within the hook, NPI space must be locked in caller
	 */
	if (NULL != AieDomain->Hooks.PreBisrHook) {
		Status = AieDomain->Hooks.PreBisrHook(AieDomain, BaseAddress);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_AIE_PRE_BISR_HOOK;
			goto fail;
		}
	}

	/* Change from AIE to AIE2. AIE has clocks enabled by default whereas AIE2
	 * has then disabled by default. Clocks must be up from this point to
	 * continue the sequence */
	/* Enable all column clocks */
	Aie2ClockInit(AieDomain, BaseAddress);

	/* Remove PMC-NoC domain isolation */
	Status = XPmDomainIso_Control((u32)XPM_NODEIDX_ISO_PMC_SOC, FALSE_VALUE);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_PMC_SOC_ISO;
		goto fail;
	}

	if (HOUSECLEAN_DISABLE_BISR_MASK != (PwrDomain->HcDisableMask &
				HOUSECLEAN_DISABLE_BISR_MASK)) {
		PmInfo("Triggering BISR for power node 0x%x\r\n", PwrDomain->Power.Node.Id);

		Status = XPmBisr_Repair(MEA_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEA_BISR_REPAIR;
			goto fail;
		}

		Status = XPmBisr_Repair(MEB_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEB_BISR_REPAIR;
			goto fail;
		}

		Status = XPmBisr_Repair(MEC_TAG_ID);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEC_BISR_REPAIR;
			goto fail;
		}
	} else {
		/* BISR is skipped */
		PmInfo("Skipping BISR for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
	}

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock ME PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus TriggerMemClear(u16 *DbgErr)
{
	XStatus Status = XST_FAILURE;

	/* Clear MEM_CLEAR_EN_ALL to minimize power during mem clear */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_EN_ALL_MASK, 0U);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_MEM_CLEAR_EN;
		goto done;
	}

	/* Set OD_MBIST_ASYNC_RESET_N bit */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK,
			      ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_MBIST_RESET;
		goto done;
	}

	/* Assert OD_BIST_SETUP_1 */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK,
			      ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_BIST_RESET;
		goto done;
	}

	/* Assert MEM_CLEAR_TRIGGER */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK,
			      ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_MEM_CLEAR_TRIGGER;
	}

done:
	return Status;
}

static XStatus IsMemClearDone(const u32 BaseAddress, u16 *DbgErr)
{
	XStatus Status = XST_FAILURE;

	/* Wait for Mem Clear DONE */
	Status = XPm_PollForMask(BaseAddress + NPI_PCSR_STATUS_OFFSET,
				 ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_DONE_MASK,
				 AIE_POLL_TIMEOUT);
	if (Status != XST_SUCCESS) {
		*DbgErr = XPM_INT_ERR_MEM_CLEAR_DONE_TIMEOUT;
		XPlmi_Printf(DEBUG_INFO, "ERROR\r\n");
		goto done;
	}
	else {
		XPlmi_Printf(DEBUG_INFO, "DONE\r\n");
	}

	/* Check Mem Clear PASS */
	if ((XPm_In32(BaseAddress + NPI_PCSR_STATUS_OFFSET) &
		      ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_PASS_MASK) !=
		      ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_PASS_MASK) {
		XPlmi_Printf(DEBUG_GENERAL, "ERROR: %s: AIE Mem Clear FAILED\r\n", __func__);
		*DbgErr = XPM_INT_ERR_MEM_CLEAR_PASS;
		Status = XST_FAILURE;
	}

done:
	return Status;
}

static XStatus CleanupMemClear(u16 *DbgErr)
{
	XStatus Status = XST_FAILURE;

	/* Clear OD_MBIST_ASYNC_RESET_N bit */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK, 0U);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_MBIST_RESET_RELEASE;
		goto done;
	}

	/* De-assert OD_BIST_SETUP_1 */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK, 0U);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_BIST_RESET_RELEASE;
		goto done;
	}

	/* De-assert MEM_CLEAR_TRIGGER */
	Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK, 0U);
	if (XST_SUCCESS != Status) {
		*DbgErr = XPM_INT_ERR_MEM_CLEAR_TRIGGER_UNSET;
	}
done:
	return Status;
}

static XStatus AieMbistClear(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	if (HOUSECLEAN_DISABLE_MBIST_CLEAR_MASK != (PwrDomain->HcDisableMask &
				HOUSECLEAN_DISABLE_MBIST_CLEAR_MASK)) {
		PmInfo("Triggering MBIST for power node 0x%x\r\n", PwrDomain->Power.Node.Id);

		Status = TriggerMemClear(&DbgErr);
		if (XST_SUCCESS != Status) {
			goto fail;
		}

		XPlmi_Printf(DEBUG_INFO, "INFO: %s : Wait for AIE Mem Clear complete...", __func__);

		Status = IsMemClearDone(BaseAddress, &DbgErr);
		if (XST_SUCCESS != Status) {
			goto fail;
		}

		Status = CleanupMemClear(&DbgErr);
		if (XST_SUCCESS != Status) {
			goto fail;
		}
	} else {
		/* MBIST is skipped */
		PmInfo("Skipping MBIST for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
		Status = XST_SUCCESS;
	}

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock ME PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus Aie2MbistClear(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	if (HOUSECLEAN_DISABLE_MBIST_CLEAR_MASK != (PwrDomain->HcDisableMask &
				HOUSECLEAN_DISABLE_MBIST_CLEAR_MASK)) {
		PmInfo("Triggering MBIST for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
		/* Change from AIE to AIE2. */
		/* TODO: In AIE this is set to low power mode to avoid failures. Need
		 * confirmation that for AIE2 low power mode is not required. */
		/* Assert MEM_CLEAR_EN_ALL */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_EN_ALL_MASK,
				ME_NPI_REG_PCSR_MASK_MEM_CLEAR_EN_ALL_MASK);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEM_CLEAR_EN;
			goto fail;
		}

		/* Set OD_MBIST_ASYNC_RESET_N bit */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK,
				ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MBIST_RESET;
			goto fail;
		}

		/* Assert OD_BIST_SETUP_1 */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK,
				ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_BIST_RESET;
			goto fail;
		}

		/* Assert MEM_CLEAR_TRIGGER */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK,
				ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEM_CLEAR_TRIGGER;
			goto fail;
		}

		XPlmi_Printf(DEBUG_INFO, "INFO: %s : Wait for AIE Mem Clear complete...", __func__);

		/* Wait for Mem Clear DONE */
		Status = XPm_PollForMask(BaseAddress + NPI_PCSR_STATUS_OFFSET,
				ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_DONE_MASK,
				AIE_POLL_TIMEOUT);
		if (Status != XST_SUCCESS) {
			DbgErr = XPM_INT_ERR_MEM_CLEAR_DONE_TIMEOUT;
			XPlmi_Printf(DEBUG_INFO, "ERROR\r\n");
			goto fail;
		}
		else {
			XPlmi_Printf(DEBUG_INFO, "DONE\r\n");
		}

		/* Check Mem Clear PASS */
		if ((XPm_In32(BaseAddress + NPI_PCSR_STATUS_OFFSET) &
					ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_PASS_MASK) !=
				ME_NPI_REG_PCSR_STATUS_MEM_CLEAR_PASS_MASK) {
			XPlmi_Printf(DEBUG_GENERAL, "ERROR: %s: AIE Mem Clear FAILED\r\n", __func__);
			DbgErr = XPM_INT_ERR_MEM_CLEAR_PASS;
			Status = XST_FAILURE;
			goto fail;
		}

		/* Clear OD_MBIST_ASYNC_RESET_N bit */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_MBIST_ASYNC_RESET_N_MASK, 0U);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MBIST_RESET_RELEASE;
			goto fail;
		}

		/* De-assert OD_BIST_SETUP_1 */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_OD_BIST_SETUP_1_MASK, 0U);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_BIST_RESET_RELEASE;
			goto fail;
		}

		/* De-assert MEM_CLEAR_TRIGGER */
		Status = AiePcsrWrite(ME_NPI_REG_PCSR_MASK_MEM_CLEAR_TRIGGER_MASK, 0U);
		if (XST_SUCCESS != Status) {
			DbgErr = XPM_INT_ERR_MEM_CLEAR_TRIGGER_UNSET;
			goto fail;
		}
	} else {
		/* MBIST is skipped */
		PmInfo("Skipping MBIST for power node 0x%x\r\n", PwrDomain->Power.Node.Id);
		Status = XST_SUCCESS;
	}

	/*
	 * To maintain backwards compatibility, skip locking of AIE NPI space. NPI
	 * space shall remain unlocked for entire housecleaning sequence unless
	 * failure occurs.
	 */
	goto done;

fail:
	/* Lock AIE PCSR */
	XPmAieDomain_LockPcsr(BaseAddress);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus AieMemInit(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	u32 BaseAddress = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const XPm_AieDomain *AieDomain = (const XPm_AieDomain *)PwrDomain;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	PmDbg("---------- START ----------\r\n");

	/* Enable scrub, Scrub ECC protected memories */
	TriggerEccScrub(AieDomain, ECC_SCRUB_ENABLE);
	/* Wait for scrubbing to finish (1ms)*/
	AieWait(1000U);

	/* Disable scrub, Scrub ECC protected memories */
	TriggerEccScrub(AieDomain, ECC_SCRUB_DISABLE);

	/* Reset Array */
	Status = ArrayReset();
	if (XST_SUCCESS != Status) {
		PmErr("ERROR: Array reset failed\r\n");
	}
	/* Zeroize Data Memory */
	Status = AieCoreMemInit(AieDomain);
	if (Status != XST_SUCCESS) {
		PmInfo("ERROR: MemInit failed\r\n");
	}
	/* Reset Array */
	Status = ArrayReset();
	if (XST_SUCCESS != Status) {
		PmErr("ERROR: Array reset failed\r\n");
		/* Lock ME PCSR */
		XPmAieDomain_LockPcsr(BaseAddress);
		goto done;
	}
	PmDbg("---------- END ----------\r\n");

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static XStatus Aie2MemInit(const XPm_PowerDomain *PwrDomain, const u32 *Args,
		u32 NumOfArgs)
{
	XStatus Status = XST_FAILURE;
	XStatus CoreZeroStatus = XST_FAILURE;
	XStatus MemZeroStatus = XST_FAILURE;
	XStatus MemTileZeroStatus = XST_FAILURE;
	u32 AieZeroizationTime = 0U;
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	u32 BaseAddress, row, col, mrow;

	const XPm_AieArray *Array = &((const XPm_AieDomain *)PwrDomain)->Array;
	u16 StartCol = Array->StartCol;
	u16 EndCol = StartCol + Array->NumCols;
	u16 StartRow = Array->StartRow;
	u16 EndRow = StartRow + Array->NumRows;
	u16 StartTileRow = StartRow + Array->NumMemRows;

	/* This function does not use the args */
	(void)Args;
	(void)NumOfArgs;

	const XPm_Device * const AieDev = XPmDevice_GetById(PM_DEV_AIE);
	if (NULL == AieDev) {
		DbgErr = XPM_INT_ERR_INVALID_DEVICE;
		goto done;
	}

	BaseAddress = AieDev->Node.BaseAddress;

	/* Enable privileged write access */
	XPm_RMW32(BaseAddress + AIE2_NPI_ME_PROT_REG_CTRL_OFFSET,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK,
			ME_PROT_REG_CTRL_PROTECTED_REG_EN_MASK);

	/* Enable memory zeroization for mem tiles; stop before tile row begins */
	for (col = StartCol; col < EndCol; col++) {
		for (row = StartRow; row < StartTileRow; row++) {
			u64 MemTileBaseAddress = AIE2_TILE_BADDR(Array->NocAddress, col, row);
			AieRMW64(MemTileBaseAddress + AIE2_MEM_TILE_MODULE_MEM_CTRL_OFFSET,
					AIE2_MEM_TILE_MODULE_MEM_CTRL_MEM_ZEROISATION_MASK,
					AIE2_MEM_TILE_MODULE_MEM_CTRL_MEM_ZEROISATION_MASK);
		}
	}

	/* Enable memory zeroization for all AIE2 tiles.
	 * Enable for core and memory modules. */
	for (col = StartCol; col < EndCol; col++) {
		for (row = StartTileRow; row < EndRow; row++) {
			u64 TileBaseAddress = AIE2_TILE_BADDR(Array->NocAddress, col, row);
			AieWrite64(TileBaseAddress + AIE2_CORE_MODULE_MEM_CTRL_OFFSET,
					AIE2_CORE_MODULE_MEM_CTRL_MEM_ZEROISATION_MASK);
			AieWrite64(TileBaseAddress + AIE2_MEM_MODULE_MEM_CTRL_OFFSET,
					AIE2_MEM_MODULE_MEM_CTRL_MEM_ZEROISATION_MASK);
		}
	}

	col = (u32)(Array->StartCol + Array->NumCols - 1U);
	row = (u32)(Array->StartRow + Array->NumRows - 1U);
	mrow = (u32)(Array->StartRow + Array->NumMemRows - 1U);

	/* Poll the last cell for each tile type for memory zeroization complete */
	while ((XST_SUCCESS != MemTileZeroStatus) ||
	       (XST_SUCCESS != CoreZeroStatus) ||
	       (XST_SUCCESS != MemZeroStatus)) {

		if (0U == AieRead64(AIE2_TILE_BADDR(Array->NocAddress, col, mrow) +
				AIE2_MEM_TILE_MODULE_MEM_CTRL_OFFSET)) {
			MemTileZeroStatus = XST_SUCCESS;
		}
		if (0U == AieRead64(AIE2_TILE_BADDR(Array->NocAddress, col, row) +
				AIE2_CORE_MODULE_MEM_CTRL_OFFSET)) {
			CoreZeroStatus = XST_SUCCESS;
		}
		if (0U == AieRead64(AIE2_TILE_BADDR(Array->NocAddress, col, row) +
				AIE2_MEM_MODULE_MEM_CTRL_OFFSET)) {
			MemZeroStatus = XST_SUCCESS;
		}

		AieZeroizationTime++;
		if (AieZeroizationTime > XPLMI_TIME_OUT_DEFAULT) {
			/* Lock ME PCSR */
			XPmAieDomain_LockPcsr(BaseAddress);
			DbgErr = XPM_INT_ERR_AIE_MEMORY_ZEROISATION;
			Status = XST_FAILURE;
			goto done;
		}
	}

	Status = XST_SUCCESS;

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}

static struct XPm_PowerDomainOps AieOps[XPM_AIE_OPS_MAX] = {
	[XPM_AIE_OPS] = {
		.InitStart = AieInitStart,
		.InitFinish = AieInitFinish,
		.ScanClear = AieScanClear,
		.Bisr = AieBisr,
		.Mbist = AieMbistClear,
		.MemInit = AieMemInit,
		/* Mask to indicate which Ops are present */
		.InitMask = (BIT16(FUNC_INIT_START) |
				BIT16(FUNC_INIT_FINISH) |
				BIT16(FUNC_SCAN_CLEAR) |
				BIT16(FUNC_BISR) |
				BIT16(FUNC_MBIST_CLEAR) |
				BIT16(FUNC_MEM_INIT))
	},
	[XPM_AIE2_OPS] = {
		.InitStart = Aie2InitStart,
		.InitFinish = Aie2InitFinish,
		.ScanClear = AieScanClear,
		.Bisr = Aie2Bisr,
		.Mbist = Aie2MbistClear,
		.MemInit = Aie2MemInit,
		/* Mask to indicate which Ops are present */
		.InitMask = (BIT16(FUNC_INIT_START) |
				BIT16(FUNC_INIT_FINISH) |
				BIT16(FUNC_SCAN_CLEAR) |
				BIT16(FUNC_BISR) |
				BIT16(FUNC_MBIST_CLEAR) |
				BIT16(FUNC_MEM_INIT))
	},
};

XStatus XPmAieDomain_Init(XPm_AieDomain *AieDomain, u32 Id, u32 BaseAddress,
			  XPm_Power *Parent, const u32 *Args, u32 NumArgs)
{
	XStatus Status = XST_FAILURE;
	u32 Platform = XPm_GetPlatform();
	u32 IdCode = XPm_GetIdCode();
	u16 DbgErr = XPM_INT_ERR_UNDEFINED;
	const struct XPm_PowerDomainOps *Ops = NULL;
	XPm_AieArray *Array = &AieDomain->Array;
	XPm_AieDomainOpHooks *Hooks = &AieDomain->Hooks;

	(void)Args;
	(void)NumArgs;

	/* Set HC Ops based on AIE version */
	if (PM_POWER_ME == Id) {
		/* AIE1: Ops */
		Ops = &AieOps[XPM_AIE_OPS];
		/* AIE1 hooks for Ops */
		Hooks->PostScanClearHook = AiePostScanClearHook;
		Hooks->PreBisrHook = AiePreBisrHook;
	} else if (PM_POWER_ME2 == Id) {
		/* AIE2: Ops */
		Ops = &AieOps[XPM_AIE2_OPS];
		/* AIE2: Hooks for Ops */
		Hooks->PostScanClearHook = NULL;
		Hooks->PreBisrHook = Aie2PreBisrHook;
	} else {
		DbgErr = XPM_INT_ERR_INVALID_PWR_DOMAIN;
		Status = XPM_INVALID_PWRDOMAIN;
		goto done;
	}

	/**
	 * NOTE: This hardcoded AIE NoC Address will later be replaced;
	 * _if_ it is passed from pm_init_start for AIE PD command.
	 */
	Array->NocAddress = (u64)VIVADO_ME_BASEADDR;

	/* Read AIE array geometry info from topology if available */
	if (3U <= NumArgs) {
		Array->GenVersion = ARR_GENV(Args[0]);
		Array->NumRows = ARR_ROWS(Args[1]);
		Array->NumCols = ARR_COLS(Args[1]);
		Array->NumAieRows = ARR_AIEROWS(Args[2]);
		Array->NumMemRows = ARR_MEMROWS(Args[2]);
		Array->NumShimRows = ARR_SHMROWS(Args[2]);
		Array->StartCol = 0U;			/**< always start from first column */
		Array->StartRow = Array->NumShimRows;	/**< always start after shim row */
	} else {
		/* TODO: Remove this block when topology CDO changes are present */

		/* AIE 1 */
		if (PM_POWER_ME == Id) {
			/* Use defaults for AIE1 */
			Array->GenVersion = AIE_GENV1;

			if (Platform != PLATFORM_VERSION_SILICON) {
				/* Non-Silicon defaults for SPP/EMU */
				Array->NumCols = 7U;
				Array->NumRows = 5U;
				Array->StartCol = 6U;
				Array->StartRow = 1U;
				Array->NumShimRows = 1U;
				Array->NumAieRows = Array->NumRows - Array->NumMemRows;
			} else {
				/* Silicon defaults for AIE1 */
				Array->NumCols = 50U;
				Array->NumRows = 8U;
				Array->StartCol = 0U;
				Array->StartRow = 1U;
				Array->NumShimRows = 1U;
				Array->NumAieRows = Array->NumRows - Array->NumMemRows;
			}

			/* AIE Instance for VC1702 */
			if ((PMC_TAP_IDCODE_DEV_SBFMLY_VC1702 == (IdCode & PMC_TAP_IDCODE_DEV_SBFMLY_MASK)) ||
			    (PMC_TAP_IDCODE_DEV_SBFMLY_VE1752 == (IdCode & PMC_TAP_IDCODE_DEV_SBFMLY_MASK))) {
				Array->NumCols = 38U;
				Array->NumRows = 8U;
				Array->StartCol = 0U;
				Array->StartRow = 1U;
				Array->NumShimRows = 1U;
				Array->NumAieRows = Array->NumRows - Array->NumMemRows;
			}
		} else {
		/* AIE 2 */
			/* Use defaults for AIE2 */
			Array->GenVersion = AIE_GENV2;

			/* NOTE: "StartTileRow" is not copied in "Array" and computed at runtime later */

			Array->NumCols = 38U;
			Array->NumRows = 10U;
			Array->NumMemRows = 2U;
			Array->NumShimRows = 1U;
			Array->NumAieRows = Array->NumRows - Array->NumMemRows;
			Array->StartCol = 0U;
			Array->StartRow = 1U;

			/* AIE2 Instance for VE2302 */
			if (PMC_TAP_IDCODE_DEV_SBFMLY_VE2302 == (IdCode & PMC_TAP_IDCODE_DEV_SBFMLY_MASK)) {
				Array->NumCols = 17U;
				Array->NumRows = 3U;
				Array->NumMemRows = 1U;
				Array->NumShimRows = 1U;
				Array->NumAieRows = Array->NumRows - Array->NumMemRows;
				Array->StartCol = 0U;
				Array->StartRow = 1U;
			}
		}
	}

	/* NOP for HC on QEMU */
	if (Platform == PLATFORM_VERSION_QEMU) {
		Ops = NULL;
	}

	Status = XPmPowerDomain_Init(&AieDomain->Domain, Id, BaseAddress,
			Parent, Ops);
	if (XST_SUCCESS != Status) {
		DbgErr = XPM_INT_ERR_POWER_DOMAIN_INIT;
	}

	/* Clear AIE section of PMC RAM register reserved for houseclean disable */
	XPm_RMW32(PM_HOUSECLEAN_DISABLE_REG_2, PM_HOUSECLEAN_DISABLE_AIE_MASK, 0U);

done:
	XPm_PrintDbgErr(Status, DbgErr);
	return Status;
}
