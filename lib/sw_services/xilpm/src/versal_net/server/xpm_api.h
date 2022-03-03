/******************************************************************************
* Copyright (c) 2018 - 2022 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/


#ifndef XPM_API_H_
#define XPM_API_H_

#include "xil_types.h"
#include "xstatus.h"
#include "xpm_defs.h"
#include "xpm_nodeid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent global general storage register base address */
#define PGGS_BASEADDR	(0xF1110050U)

#define MAX_BASEADDR_LEN	3

/* Extern Variable and Function */
extern u32 ResetReason;

XStatus XPm_Init(void (*const RequestCb)(const u32 SubsystemId, const XPmApiCbId_t EventId, u32 *Payload),
		 int (*const RestartCb)(u32 ImageId, u32 *FuncId));

XStatus XPm_AddSubsystem(u32 SubsystemId);

XStatus XPm_SystemShutdown(u32 SubsystemId, const u32 Type, const u32 SubType,
			   const u32 CmdType);

XStatus XPm_RequestWakeUp(u32 SubsystemId, const u32 DeviceId,
			const u32 SetAddress,
			const u64 Address,
			const u32 Ack,
			const u32 CmdType);

XStatus XPm_RequestDevice(const u32 SubsystemId, const u32 DeviceId,
			  const u32 Capabilities, const u32 QoS, const u32 Ack,
			  const u32 CmdType);

XStatus XPm_ReleaseDevice(const u32 SubsystemId, const u32 DeviceId,
			  const u32 CmdType);

XStatus XPm_GetDeviceStatus(const u32 SubsystemId,
			const u32 DeviceId,
			XPm_DeviceStatus *const DeviceStatus);

XStatus XPm_Query(const u32 Qid, const u32 Arg1, const u32 Arg2,
		  const u32 Arg3, u32 *const Output);


XStatus XPm_DevIoctl(const u32 SubsystemId, const u32 DeviceId,
                        const pm_ioctl_id IoctlId,
                        const u32 Arg1,
                        const u32 Arg2,u32 *const Response, const u32 CmdType);

XStatus XPm_InitNode(u32 NodeId, u32 Function, const u32 *Args, u32 NumArgs);
XStatus XPm_GicProxyWakeUp(const u32 PeriphIdx);
XStatus XPm_HookAfterPlmCdo(void);
u32 XPm_GetSubsystemId(u32 ImageId);
XStatus XPm_IsoControl(u32 NodeId, u32 Enable);
XStatus XPm_GetDeviceBaseAddr(u32 DeviceId, u32 *BaseAddr);
XStatus XPm_GetApiVersion(u32 *Version);

#ifdef __cplusplus
}
#endif

/** @} */
#endif /* XPM_API_H_ */