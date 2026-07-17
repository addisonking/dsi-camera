#include "i2c_handler.h"

#include "aptina.h"
#include "aptina_i2c.h"

#include <nds.h>

// Management structure and stack space for PXI server thread
Thread s_i2cPxiThread;
alignas(8) u8 s_i2cPxiThreadStack[1024];

// Custom mic recorder: NDMA0 drains the MICEX FIFO into a ring of buffers in
// main RAM (see pxi_vars.h). The ARM9 polls the completion counter in the ring
// header, so no PXI traffic happens per buffer. If the MICEX FIFO overruns
// (e.g. while IRQs are masked during SD transactions) it is restarted, so
// audio degrades for a moment instead of dying for the rest of the recording.
static vu32 *s_micDone;
static u32 s_micAddr;
static int s_micAddrState; // 1 = next message is addr low half, 2 = high half
static u8 s_micSlot;
static bool s_micActive;

static void micexStart(void) {
	REG_MICEX_CNT = MICEX_CNT_CLEAR_FIFO;
	REG_MICEX_CNT = MICEX_CNT_NO_R | MICEX_CNT_RATE_DIV(1) | MICEX_CNT_ENABLE; // div1 = 16364Hz
}

#define MIC_NDMA_CFG                                                                               \
	(NDMA_DST_MODE(NdmaMode_Increment) | NDMA_SRC_MODE(NdmaMode_Fixed) | NDMA_BLK_WORDS(8) |       \
	 NDMA_TIMING(NdmaTiming_MicData) | NDMA_TX_MODE(NdmaTxMode_Timing) | NDMA_IRQ_ENABLE |         \
	 NDMA_START)

// Fully (re)initializes NDMA channel ch to capture one mic buffer into `slot`.
// A wedged channel can't be revived with just |= NDMA_START, so always rebuild
// it from scratch.
static void micArmChannel(int ch, int slot) {
	REG_NDMAxCNT(ch)  = 0;
	REG_NDMAxSAD(ch)  = (u32)&REG_MICEX_DATA;
	REG_NDMAxDAD(ch)  = (u32)(s_micAddr + 32 + slot * MIC_BUF_BYTES);
	REG_NDMAxBCNT(ch) = 0;
	REG_NDMAxTCNT(ch) = MIC_BUF_BYTES / 4;
	REG_NDMAxWCNT(ch) = 8;
	REG_NDMAxCNT(ch)  = MIC_NDMA_CFG;
}

// Channels 0 and 1 ping-pong: while one drains the FIFO the other is already
// armed behind it, so a completion hands over in hardware with no IRQ-latency
// window (the old single-channel design overran the FIFO permanently whenever
// SD writes delayed the re-arm ISR by more than ~1ms).
static void micProcessCompletions(void) {
	for(int ch = 0; ch < 2; ch++) {
		if(REG_NDMAxCNT(ch) & NDMA_START)
			continue; // still armed/busy
		(*s_micDone)++;
		micArmChannel(ch, s_micSlot);
		s_micSlot = (s_micSlot + 1) % MIC_RING_BUFS;
	}
}

static void micNdmaIsr(void) {
	if(!s_micActive)
		return;
	micProcessCompletions();
	if(REG_MICEX_CNT & MICEX_CNT_FIFO_BORKED)
		micexStart(); // recover from overrun
}

// Belt and braces: tick-driven watchdog recovers even if both channels wedge.
static TickTask s_micWatchdog;

static void micWatchdogTick(TickTask *t) {
	if(!s_micActive)
		return;
	// Debug telemetry for the ARM9: heartbeat + raw MICEX/NDMA state in the
	// unused part of the ring header.
	vu32 *hdr = (vu32 *)s_micAddr;
	hdr[1]++;
	hdr[2] = REG_MICEX_CNT;
	hdr[3] = REG_NDMAxCNT(0);
	if(REG_MICEX_CNT & MICEX_CNT_FIFO_BORKED) {
		micProcessCompletions();
		micexStart();
	}
}

static void micStart(void) {
	s_micDone   = (vu32 *)s_micAddr;
	*s_micDone  = 0;
	s_micActive = true;

	REG_MICEX_CNT = 0;
	micArmChannel(0, 0);
	micArmChannel(1, 1);
	s_micSlot = 2; // next slot to arm on completion
	irqSet(IRQ_NDMA0 | IRQ_NDMA1, micNdmaIsr);
	irqEnable(IRQ_NDMA0 | IRQ_NDMA1);
	micexStart();
	tickTaskStart(&s_micWatchdog, micWatchdogTick, ticksFromHz(20), ticksFromHz(20));
}

static void micStop(void) {
	s_micActive = false;
	tickTaskStop(&s_micWatchdog);
	REG_IE &= ~(IRQ_NDMA0 | IRQ_NDMA1);
	REG_MICEX_CNT   = 0;
	REG_NDMAxCNT(0) = 0;
	REG_NDMAxCNT(1) = 0;
}

//---------------------------------------------------------------------------------
int i2cPxiThreadMain(void *arg) {
	//---------------------------------------------------------------------------------
	// Set up PXI mailbox, used to receive PXI command words
	Mailbox mb;
	u32 mb_slots[4];
	mailboxPrepare(&mb, mb_slots, sizeof(mb_slots) / 4);
	pxiSetMailbox(PxiChannel_User0, &mb);

	// Main PXI message loop
	for(;;) {
		// Receive a message
		u32 msg    = mailboxRecv(&mb);
		u32 retval = 0;

		switch(msg) {
			case CAM_INIT:
				init(I2C_CAM0);
				init(I2C_CAM1);
				retval = aptReadRegister(I2C_CAM0, 0);
				break;
			case CAM0_ACTIVATE:
				activate(I2C_CAM0);
				retval = CAM0_ACTIVATE;
				break;
			case CAM0_DEACTIVATE:
				deactivate(I2C_CAM0);
				retval = CAM0_DEACTIVATE;
				break;
			case CAM1_ACTIVATE:
				activate(I2C_CAM1);
				retval = CAM1_ACTIVATE;
				break;
			case CAM1_DEACTIVATE:
				deactivate(I2C_CAM1);
				retval = CAM1_DEACTIVATE;
				break;
			case CAM_SET_MODE_PREVIEW:
				setMode(CAPTURE_MODE_PREVIEW);
				retval = CAPTURE_MODE_PREVIEW;
				break;
			case CAM_SET_MODE_CAPTURE:
				setMode(CAPTURE_MODE_CAPTURE);
				retval = CAPTURE_MODE_CAPTURE;
				break;
			case CAM_GET_KEY0:
			case CAM_GET_KEY1:
			case CAM_GET_KEY2:
			case CAM_GET_KEY3:
			case CAM_GET_KEY4:
			case CAM_GET_KEY5:
			case CAM_GET_KEY6:
			case CAM_GET_KEY7:
				// 16-bit halfword to stay within PXI's 26-bit immediate.
				retval = ((u16 *)g_cameraKey)[msg - CAM_GET_KEY0];
				break;
			case CAM_MIC_ADDR_LO:
				s_micAddrState = 1;
				retval         = CAM_MIC_ADDR_LO;
				break;
			case CAM_MIC_ADDR_HI:
				s_micAddrState = 2;
				retval         = CAM_MIC_ADDR_HI;
				break;
			case CAM_MIC_START:
				micStart();
				retval = CAM_MIC_START;
				break;
			case CAM_MIC_STOP:
				micStop();
				retval = CAM_MIC_STOP;
				break;
			default:
				// Halfword following CAM_MIC_ADDR_LO/HI: the ring address.
				if(s_micAddrState == 1) {
					s_micAddr      = (s_micAddr & 0xFFFF0000) | (msg & 0xFFFF);
					s_micAddrState = 0;
					retval         = msg;
				} else if(s_micAddrState == 2) {
					s_micAddr      = (s_micAddr & 0xFFFF) | ((msg & 0xFFFF) << 16);
					s_micAddrState = 0;
					retval         = msg;
				}
				break;
		}

		// Send a reply back to the ARM9
		pxiReply(PxiChannel_User0, retval);
	}

	return 0;
}
