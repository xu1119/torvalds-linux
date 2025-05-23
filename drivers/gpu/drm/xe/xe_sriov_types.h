/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef _XE_SRIOV_TYPES_H_
#define _XE_SRIOV_TYPES_H_

#include <linux/build_bug.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue_types.h>

/**
 * VFID - Virtual Function Identifier
 * @n: VF number
 *
 * Helper macro to represent Virtual Function (VF) Identifier.
 * VFID(0) is used as alias to the PFID that represents Physical Function.
 *
 * Note: According to PCI spec, SR-IOV VF's numbers are 1-based (VF1, VF2, ...).
 */
#define VFID(n)		(n)
#define PFID		VFID(0)

/**
 * enum xe_sriov_mode - SR-IOV mode
 * @XE_SRIOV_MODE_NONE: bare-metal mode (non-virtualized)
 * @XE_SRIOV_MODE_PF: SR-IOV Physical Function (PF) mode
 * @XE_SRIOV_MODE_VF: SR-IOV Virtual Function (VF) mode
 */
enum xe_sriov_mode {
	/*
	 * Note: We don't use default enum value 0 to allow catch any too early
	 * attempt of checking the SR-IOV mode prior to the actual mode probe.
	 */
	XE_SRIOV_MODE_NONE = 1,
	XE_SRIOV_MODE_PF,
	XE_SRIOV_MODE_VF,
};
static_assert(XE_SRIOV_MODE_NONE);

/**
 * struct xe_device_pf - Xe PF related data
 *
 * The data in this structure is valid only if driver is running in the
 * @XE_SRIOV_MODE_PF mode.
 */
struct xe_device_pf {
	/** @device_total_vfs: Maximum number of VFs supported by the device. */
	u16 device_total_vfs;

	/** @driver_max_vfs: Maximum number of VFs supported by the driver. */
	u16 driver_max_vfs;

	/** @master_lock: protects all VFs configurations across GTs */
	struct mutex master_lock;
};

/**
 * struct xe_device_vf - Xe Virtual Function related data
 *
 * The data in this structure is valid only if driver is running in the
 * @XE_SRIOV_MODE_VF mode.
 */
struct xe_device_vf {
	/** @migration: VF Migration state data */
	struct {
		/** @migration.worker: VF migration recovery worker */
		struct work_struct worker;
		/** @migration.gt_flags: Per-GT request flags for VF migration recovery */
		unsigned long gt_flags;
	} migration;
};

#endif
