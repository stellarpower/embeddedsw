/******************************************************************************
* Copyright (c) 2018 - 2022 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

#ifndef XPM_REGS_H
#define XPM_REGS_H

#ifdef __cplusplus
extern "C" {
#endif
/* PMC_GLOBAL registers */
#ifndef PMC_GLOBAL_BASEADDR
#define PMC_GLOBAL_BASEADDR			(0xF1110000U)
#endif

#define PMC_GLOBAL_PMC_GSW_ERR_OFFSET		(0x00000064U)
#define PMC_GLOBAL_PMC_GSW_ERR_CR_FLAG_SHIFT	(30U)
/* PSM GLOBAL registers */
#define PSM_GLOBAL_REG_BASEADDR			(0xEBC90000U)

#define XPM_RPU_CLUSTER_LOCKSTEP_DISABLE (0x1U)
#define XPM_RPU_CLUSTER_LOCKSTEP_ENABLE (0x0U)
#define XIH_PH_ATTRB_CLUSTER_LOCKSTEP_DISABLED (0x0U)
#define XIH_PH_ATTRB_CLUSTER_LOCKSTEP_ENABLE (0x30U)
#define XPM_A78_CLUSTER_CONFIGURED 		(1U)
#define XPM_R52_CLUSTER_CONFIGURED 		(1U)

#define XPM_APU_CLUSTER_LOCKSTEP_DISABLE	(0U)
#define XPM_APU_CLUSTER_LOCKSTEP_ENABLE	(1U)

#define RPU_CLUSTER_CORE_CFG0_OFFSET		(0x0U)
#define RPU_CLUSTER_CORE_CFG0_REMAP_MASK	(0x00000020U)
#define RPU_CLUSTER_CORE_CFG0_CPU_HALT_MASK	(0x00000001U)

#define RPU_CLUSTER_CORE_VECTABLE_OFFSET	(0x10U)
#define RPU_CLUSTER_CORE_VECTABLE_MASK		(0xFFFFFFE0U)
#define RPU_CLUSTER_CFG_OFFSET			(0x00000800U)

#define PSX_CRL_BASEADDR		(0xEB5E0000U)
#define CRL_RST_RPU_ADDR		(PSX_CRL_BASEADDR + 0x310U)
#define CRL_PSM_RST_MODE_ADDR		(PSX_CRL_BASEADDR + 0x380U)
#define CRL_PSM_RST_WAKEUP_MASK		(0x4U)

#define RPU_A_TOPRESET_MASK		(0x00010000U)
#define RPU_A_DBGRST_MASK		(0x00100000U)
#define RPU_A_DCLS_TOPRESET_MASK	(0x00040000U)
#define RPU_CORE0A_POR_MASK		(0x00000100U)
#define RPU_CORE0A_RESET_MASK		(0x00000001U)

/* EFUSE_CACHE registers */
/* putting def guard because , xplmi_hw.h already defined this */
#ifndef EFUSE_CACHE_BASEADDR
#define EFUSE_CACHE_BASEADDR			(0xF1250000U)
#endif
#define EFUSE_CACHE_BISR_RSVD_0_OFFSET		(0x00000504U)
#define EFUSE_CACHE_TBITS1_BISR_RSVD_OFFSET	(0x00000500U)
#define EFUSE_CACHE_TBITS2_BISR_RSVD_OFFSET	(0x00000BFCU)

#define GetRpuRstMask(Mask, ClusterNum, CoreNum)  (Mask << ((2U * ClusterNum)\
						+ CoreNum))

#define XPM_RPU_CORE0	(0U)
#define XPM_RPU_CORE1	(1U)

/*Registers*/
/*
 * Definitions required for RPU_CLUSTER
 */
#define RPU_CLUSTER_OFFSET		(0x10000U)
#define RPU_CLUSTER_CORE_OFFSET		(0x100U)

#define GET_RPU_CLUSTER_CORE_REG(ClusterNum, CoreNum, Offset) \
		GET_REGISTER_ADDR(RPU_CLUSTER_BASEADDR, \
		(ClusterNum) * RPU_CLUSTER_OFFSET, Offset, \
		(CoreNum) * RPU_CLUSTER_CORE_OFFSET)

#define GET_RPU_CLUSTER_REG(ClusterNum, Offset) \
		GET_REGISTER_ADDR(RPU_CLUSTER_BASEADDR, \
		(ClusterNum) * RPU_CLUSTER_OFFSET, Offset, 0U)

/*
 * Definitions required for FPX_SLCR
 */
#define FPX_SLCR_BASEADDR		(0xEC8C0000U)
#define FPX_SLCR_APU_CTRL		(FPX_SLCR_BASEADDR + 0x1000U)

/*
 * Definitions required for APU_CLUSTER
 */
#define APU_CLUSTER_BASEADDR		(0xECC00000U)
#define APU_CLUSTER_OFFSET		    (0x00100000U)
#define APU_CLUSTER2_OFFSET		    (0x00E00000U)

#define APU_CLUSTER_RVBARADDR0L_OFFSET	(0x00000040U)
#define APU_CLUSTER_RVBARADDR0H_OFFSET	(0x00000044U)

/*
 * Definitions required for APU_PCLI
 */
#define APU_PCLI_BASEADDR		    (0xECB10000U)
#define APU_PCLI_CORE_OFFSET		(0x00000030U)
#define APU_PCLI_CLUSTER_OFFSET		(0x00001000U)
#define APU_PCLI_CLUSTER_PREQ_OFFSET		(0x00008004U)
#define APU_PCLI_CLUSTER_PREQ_PREQ_MASK		(0x00000001U)
#define APU_PCLI_CLUSTER_PSTATE_OFFSET		(0x00008008U)
#define APU_PCLI_CLUSTER_PSTATE_PSTATE_MASK	(0x0000007FU)
#define APU_PCLI_CLUSTER_PACTIVE_OFFSET		(0x0000800CU)
#define APU_PCLI_CLUSTER_PACTIVE_PACCEPT_MASK	(0x01000000U)
#define APU_CLUSTER_PSTATE_FULL_ON_VAL		(0x00000048U)
#define APU_PCLI_CORE_PREQ_OFFSET		(0x00000004U)
#define APU_PCLI_CORE_PREQ_PREQ_MASK		(0x00000001U)
#define APU_PCLI_CORE_PSTATE_OFFSET		(0x00000008U)
#define APU_PCLI_CORE_PSTATE_PSTATE_MASK	(0x0000003FU)
#define APU_PCLI_CORE_PACTIVE_OFFSET		(0x0000000CU)
#define APU_PCLI_CORE_PACTIVE_PACCEPT_MASK	(0x01000000U)
#define APU_CORE_PSTATE_FULL_ON_VAL		(0x00000038U)

#define GET_APU_CLUSTER_REG(ClusterNum, Offset)		\
	GET_REGISTER_ADDR(APU_CLUSTER_BASEADDR,		\
	((ClusterNum / 2U) * APU_CLUSTER2_OFFSET) + 	\
	((ClusterNum % 2U) * APU_CLUSTER_OFFSET), Offset, 0U)

#define GET_APU_PCLI_CLUSTER_REG(CoreNum, Offset)		\
	GET_REGISTER_ADDR(APU_PCLI_BASEADDR, 0U, Offset, \
		(CoreNum) * APU_PCLI_CLUSTER_OFFSET)

#define GET_APU_PCLI_CORE_REG(CoreNum, Offset)		\
	GET_REGISTER_ADDR(APU_PCLI_BASEADDR, 0U, Offset, \
		(CoreNum) * APU_PCLI_CORE_OFFSET)

#define XPM_R52_0A_TCMA_BASE_ADDR	(0xEBA00000U)
#define XPM_R52_1A_TCMA_BASE_ADDR	(0xEBA40000U)
#define XPM_R52_0B_TCMA_BASE_ADDR	(0xEBA80000U)
#define XPM_R52_1B_TCMA_BASE_ADDR	(0xEBAC0000U)
#define XPM_R52_TCM_CLUSTER_OFFSET	(0x00080000U)

/*
 * TCM address for R52
 */
#define XPM_R52_TCMA_LOAD_ADDRESS	(0x0U)
#define XPM_R52_TCM_TOTAL_LENGTH	(0x30000U)


#define XPM_R52_0_TCMA_ECC_DONE     (0x00000001U)
#define XPM_R52_1_TCMA_ECC_DONE 	(0x00000002U)

#ifdef __cplusplus
}
#endif

#endif /* XPM_REGS_H */