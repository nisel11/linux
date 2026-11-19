/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the direct-MMIO MediaTek VENC H.264 encoder.
 *
 * The offsets and bit fields below were recovered from the vendor
 * libvcodecdrv.so H.264 encoder HAL (libvcodecdrv.so.c).  Registers marked
 * "unverified" could not be fully decoded from Ghidra output; they are
 * kept with the exact value the HAL writes so that the programming
 * sequence stays faithful to the vendor driver.
 */

#ifndef _VENC_DIRECT_REGS_H_
#define _VENC_DIRECT_REGS_H_

#include <linux/bits.h>

/* Register offsets within the VENC core window at 0x17020000. */
#define VENC_SYS_MODE_SEL		0x000
#define VENC_SYS_MODE_SEL_H264		0x200bf60

#define VENC_SYS_FRAME_CTRL		0x010
#define VENC_SYS_FRAME_CTRL_BASE	0x400
#define VENC_SYS_FRAME_CTRL_IDR		0x3000
#define VENC_SYS_FRAME_CTRL_GOP_ID	GENMASK(12, 9)
#define VENC_SYS_FRAME_CTRL_NONREF_P	BIT(18)
#define VENC_SYS_FRAME_CTRL_FRAME_NUM_GAP BIT(8)
#define VENC_SYS_FRAME_CTRL_QP_DELTA	0x100000
#define VENC_SYS_FRAME_CTRL_MODE_MAGIC	0x3000000
#define VENC_SYS_FRAME_CTRL_SINGLE_SLICE	0x8000

#define VENC_SYS_FRAME_COUNT		0x014
#define VENC_SYS_FRAME_NUM_CURR		0x018
#define VENC_SYS_FRAME_NUM_REF		0x01c
#define VENC_SYS_I_FRAME_SIZE		0x020

#define VENC_SYS_PIC_SIZE_0		0x024
#define VENC_SYS_PIC_HEIGHT_MB_SHIFT	16
#define VENC_SYS_PIC_HEIGHT_MB_MASK	GENMASK(31, 16)
#define VENC_SYS_PIC_WIDTH_MB_MASK	GENMASK(15, 0)

#define VENC_SYS_PIC_SIZE_1		0x028
#define VENC_SYS_PIC_BUF_WIDTH_MB_SHIFT	20
#define VENC_SYS_PIC_BUF_WIDTH_MB_MASK	GENMASK(28, 20)
#define VENC_SYS_PIC_UV_PITCH_SHIFT	11
#define VENC_SYS_PIC_UV_PITCH_MASK	GENMASK(19, 11)
#define VENC_SYS_PIC_HPAD_SHIFT		5
#define VENC_SYS_PIC_HPAD_MASK		GENMASK(9, 5)
#define VENC_SYS_PIC_VPAD_MASK		GENMASK(4, 0)
#define VENC_SYS_PIC_SCAN_TYPE_SHIFT	29
#define VENC_SYS_PIC_SCAN_TYPE_NV12	0

#define VENC_SYS_TOTAL_MBS		0x02c

/* profile bits in 0x30 (H264_GetProfile, HAL ~85724) */
#define VENC_SYS_CODEC_CFG		0x030
#define VENC_SYS_CODEC_PROFILE_BASELINE	0x42
#define VENC_SYS_CODEC_PROFILE_MAIN	0x4d
#define VENC_SYS_CODEC_PROFILE_HIGH	100
#define VENC_SYS_CODEC_PROFILE_MASK	GENMASK(31, 29)
#define VENC_SYS_CODEC_LEVEL_SHIFT	22
#define VENC_SYS_CODEC_LEVEL_MASK	GENMASK(25, 22)
#define VENC_SYS_CODEC_MBAFF_BIT	BIT(28)
#define VENC_SYS_CODEC_COLOR_BIT	BIT(21)
#define VENC_SYS_CODEC_FLAG_0		BIT(17)
#define VENC_SYS_CODEC_FLAG_1		BIT(9)
#define VENC_SYS_CODEC_FLAG_2		BIT(10)

#define VENC_SYS_MB_CFG			0x040

#define VENC_SYS_RATE_CTRL_0		0x048
#define VENC_SYS_RATE_CTRL_0_POC	0x20000

#define VENC_SYS_RATE_CTRL_1		0x04c
#define VENC_SYS_RATE_CTRL_1_FPS_SHIFT	16
#define VENC_SYS_RATE_CTRL_1_FPS_MASK	GENMASK(23, 16)

#define VENC_SYS_RATE_CTRL_2		0x050
#define VENC_SYS_RATE_CTRL_2_MAGIC	0x3000000
#define VENC_SYS_RATE_CTRL_2_WFD	0x3080000
#define VENC_SYS_RATE_CTRL_2_SECURE	0x1e000000
#define VENC_SYS_RATE_CTRL_2_QP_SHIFT	8
#define VENC_SYS_RATE_CTRL_2_QP_MASK	GENMASK(15, 8)
#define VENC_SYS_RATE_CTRL_2_QP_MIN	GENMASK(7, 0)

#define VENC_SYS_RATE_CTRL_3		0x054
#define VENC_SYS_RATE_CTRL_3_DEFAULT	0x808

/* Trigger / unit type (HAL H264_EncodeFrameEnc_TriggerHW ~90665) */
#define VENC_SYS_TRIGGER_UNIT		0x058
#define VENC_SYS_TRIGGER_UNIT_FRAME	4
#define VENC_SYS_TRIGGER_UNIT_PPS	2
#define VENC_SYS_TRIGGER_UNIT_SPS	1

#define VENC_SYS_IRQ_STATUS		0x05c
#define VENC_SYS_IRQ_ACK		0x060
/* Frame done uses the same bit layout as the existing encoder IRQ handler. */
#define VENC_SYS_IRQ_DONE		BIT(2)
#define VENC_SYS_IRQ_BS_FULL		BIT(3)
#define VENC_SYS_IRQ_SPS		BIT(0)
#define VENC_SYS_IRQ_PPS		BIT(1)
#define VENC_SYS_IRQ_ALL		0x3f

#define VENC_SYS_BS_SIZE		0x064
#define VENC_SYS_BS_ADDR		0x068
#define VENC_SYS_BS_ADDR_SHIFT		7
#define VENC_SYS_CURR_Y_ADDR		0x06c
#define VENC_SYS_CURR_U_ADDR		0x070
#define VENC_SYS_CURR_V_ADDR		0x094
#define VENC_SYS_ADDR_SHIFT		4
#define VENC_SYS_ADDR_HIGH_SHIFT	28

#define VENC_SYS_REF_BASE		0x074
#define VENC_SYS_REF_NUM		6

#define VENC_SYS_RC_INFO_ADDR		0x08c
#define VENC_SYS_RC_CODE_ADDR		0x090

#define VENC_SYS_BITSTREAM_BYTE_CNT	0x098

#define VENC_SYS_IRQ_MASK		0x0a4
/* HAL H264_EncodeFrameEnc_TriggerHW() ~90678-90681: non-GCE path writes
 * 0x3f here (frame-done IRQ sources enabled); GCE path writes 0.
 */
#define VENC_SYS_IRQ_MASK_DEFAULT	0x3f

/* Software reset: write 0, read back, write 1, read back (HAL ~90001) */
#define VENC_SYS_SW_RESET		0x0a8

#define VENC_SYS_GOP_IDX		0x16c
#define VENC_SYS_REF_P_DIST		0x124
#define VENC_SYS_FRAME_CFG_128		0x128
#define VENC_SYS_CFG_12C		0x12c

#define VENC_SYS_CLOCK_GATE		0x1c0
#define VENC_SYS_CLOCK_GATE_DEFAULT	0x10000
#define VENC_SYS_CLOCK_GATE_ON		0x11
#define VENC_SYS_CLOCK_GATE_ON_SECURE	1
#define VENC_SYS_UFO_CFG_1C4		0x1c4

#define VENC_SYS_RGB_MODE		0x0e4

/* Second window (0x17020000, 0x10000): scratch / psy-rd */
#define VENC_CORE_SCRATCH_0		0x1040
#define VENC_CORE_SCRATCH_LUMA		0x1044
#define VENC_CORE_SCRATCH_LUMA_2	0x1048
#define VENC_CORE_SCRATCH_CHROMA	0x104c
#define VENC_CORE_BUFFER_MODE		0x1050
#define VENC_CORE_PSY_RD_LAMBDA		0x1078
#define VENC_CORE_GCE_TRIGGER		0x1084

/* Frame done flag and write-back control, same window as 0x58 */
#define VENC_SYS_ENABLE			0x0ec
#define VENC_SYS_STATUS			0x0bc

#endif /* _VENC_DIRECT_REGS_H_ */
