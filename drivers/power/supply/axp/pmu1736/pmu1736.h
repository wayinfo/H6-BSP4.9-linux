/*
 * drivers/power/supply/axp/pmu1736/pmu1736.h
 * (C) Copyright 2010-2016
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 * Pannan <pannan@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */
#ifndef PMU1736_H_
#define PMU1736_H_

#define RSB_RTSADDR_PMU1736       (0x2D)

#define PMU1736_POWER_SOURCE      (0x00)
#define PMU1736_IC_TYPE           (0x03)
#define PMU1736_DATA_BUFFER1      (0x04)
#define PMU1736_DATA_BUFFER2      (0x05)
#define PMU1736_DATA_BUFFER3      (0x06)
#define PMU1736_DATA_BUFFER4      (0x07)
#define PMU1736_ON_OFF_CTL1       (0x10)
#define PMU1736_ON_OFF_CTL2       (0x11)
#define PMU1736_ON_OFF_CTL3       (0x12)
#define PMU1736_DC1OUT_VOL        (0x13)
#define PMU1736_DC2OUT_VOL        (0x14)
#define PMU1736_DC3OUT_VOL        (0x15)
#define PMU1736_DC4OUT_VOL        (0x16)
#define PMU1736_DC5OUT_VOL        (0x17)
#define PMU1736_DC6OUT_VOL        (0x18)
#define PMU1736_ALDO1OUT_VOL      (0x19)
#define PMU1736_DCDC_MODE_CTL1    (0x1A)
#define PMU1736_DCDC_MODE_CTL2    (0x1B)
#define PMU1736_DCDC_MODE_CTL3    (0x1C)
#define PMU1736_DCDC_FREQ_SET     (0x1D)
#define PMU1736_OUT_MONITOR_CTL   (0x1E)
#define PMU1736_IRQ_PWROK_VOFF    (0x1F)
#define PMU1736_ALDO2OUT_VOL      (0x20)
#define PMU1736_ALDO3OUT_VOL      (0x21)
#define PMU1736_ALDO4OUT_VOL      (0x22)
#define PMU1736_ALDO5OUT_VOL      (0x23)
#define PMU1736_BLDO1OUT_VOL      (0x24)
#define PMU1736_BLDO2OUT_VOL      (0x25)
#define PMU1736_BLDO3OUT_VOL      (0x26)
#define PMU1736_BLDO4OUT_VOL      (0x27)
#define PMU1736_BLDO5OUT_VOL      (0x28)
#define PMU1736_CLDO1OUT_VOL      (0x29)
#define PMU1736_CLDO2OUT_VOL      (0x2A)
#define PMU1736_CLDO3OUT_VOL      (0x2B)
#define PMU1736_CLDO4_GPIO2_CTL   (0x2C)
#define PMU1736_CLDO4OUT_VOL      (0x2D)
#define PMU1736_CPUSOUT_VOL       (0x2E)
#define PMU1736_WAKEUP_CTL_OCIRQ  (0x31)
#define PMU1736_PWR_DISABLE_DOWN  (0x32)
#define PMU1736_POK_SET           (0x36)
#define PMU1736_INT_MODE_SELECT   (0x3E)
#define PMU1736_INTEN1            (0x40)
#define PMU1736_INTEN2            (0x41)
#define PMU1736_INTSTS1           (0x48)
#define PMU1736_INTSTS2           (0x49)

/* bit definitions for AXP events ,irq event */
#define PMU1736_IRQ_LOWN1         (0)
#define PMU1736_IRQ_LOWN2         (1)
#define PMU1736_IRQ_DC1UN         (2)
#define PMU1736_IRQ_DC2UN         (3)
#define PMU1736_IRQ_DC3UN         (4)
#define PMU1736_IRQ_DC4UN         (5)
#define PMU1736_IRQ_DC5UN         (6)
#define PMU1736_IRQ_DC6UN         (7)
#define PMU1736_IRQ_PEKL          (8)
#define PMU1736_IRQ_PEKS          (9)
#define PMU1736_IRQ_PEKO          (10)
#define PMU1736_IRQ_PEKFE         (11)
#define PMU1736_IRQ_PEKRE         (12)
#define PMU1736_IRQ_ALDOIN_VOFF   (13)
#define PMU1736_IRQ_DC2OV         (14)
#define PMU1736_IRQ_DC3OV         (15)

extern s32 axp_debug;
extern struct axp_config_info pmu1736_config;

#endif /* PMU1736_H_ */
