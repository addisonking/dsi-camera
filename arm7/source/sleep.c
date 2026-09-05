#include "pxi_vars.h"

#include <calico/arm/common.h>
#include <calico/nds/arm7/i2c.h>
#include <calico/nds/arm7/pmic.h>
#include <calico/nds/arm7/rtc.h>
#include <calico/nds/arm7/spi.h>
#include <calico/nds/bios.h>
#include <calico/nds/keypad.h>
#include <calico/nds/pm.h>
#include <calico/nds/pxi.h>
#include <calico/nds/system.h>
#include <calico/system/irq.h>

static volatile bool s_shoulderWake;

// Calico's sound server normally registers the PM handlers that call these.
// This override calls the same hooks because it replaces Calico's PM routine.
extern void _soundDisable(void);
extern void _soundEnable(void);

static void shoulderWakeIrq(void) { s_shoulderWake = true; }

// Calico normally wakes only for the hinge while asleep. Keep the same sleep
// sequence, but also wake on L+R and report that wake to the ARM9.
void pmEnterSleep(void) {
	_soundDisable();
	if(systemIsTwlMode())
		i2cLock();
	spiLock();

	ArmIrqState cpsrIf = armIrqLockByPsr();
	IrqState ime       = irqLock();
	u32 ie             = REG_IE;
	u32 ie2            = REG_IE2;
	u16 keycnt         = REG_KEYCNT;
	REG_IE             = IRQ_HINGE | IRQ_KEYPAD;
	REG_IE2            = 0;
	irqSet(IRQ_KEYPAD, shoulderWakeIrq);
	REG_KEYCNT = KEY_L | KEY_R | KEYCNT_IRQ_ENABLE | KEYCNT_IRQ_AND;

	u8 pmicCtl = pmicReadRegister(PmicReg_Control);
	pmicWriteRegister(PmicReg_Control, PMIC_CTRL_LED_BLINK_SLOW);

	irqUnlock(1);
	armIrqUnlockByPsr(0);
	svcSleep();

	armIrqLockByPsr();
	bool shoulderWake = s_shoulderWake;
	s_shoulderWake    = false;
	REG_KEYCNT        = keycnt;
	REG_IF            = IRQ_KEYPAD;
	irqSet(IRQ_KEYPAD, pmPrepareToReset);
	REG_IE  = ie;
	REG_IE2 = ie2;
	irqUnlock(ime);
	armIrqUnlockByPsr(cpsrIf);

	pmicWriteRegister(PmicReg_Control, pmicCtl);
	spiUnlock();
	if(systemIsTwlMode())
		i2cUnlock();
	rtcSyncTime();
	_soundEnable();

	if(shoulderWake)
		pxiSend(PxiChannel_User1, PXI_SLEEP_SHOULDER_WAKE);
	pxiSend(PxiChannel_Power, 2); // PxiPmMsg_Wakeup
}
