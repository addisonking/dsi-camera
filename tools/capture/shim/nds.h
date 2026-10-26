#ifndef TEST_NDS_H
#define TEST_NDS_H
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
#define BIT(n) (1u << (n))
#define IRQ_NDMA1 BIT(29)
#define PxiChannel_User0 0
extern u32 REG_SCFG_CLK, REG_IF;
void swiDelay(unsigned delay);
void irqSet(u32 mask, void (*handler)(void));
void irqEnable(u32 mask);
void irqDisable(u32 mask);
void irqClear(u32 mask);
void armDCacheFlush(const void *address, unsigned size);
void armDCacheInvalidate(const void *address, unsigned size);
u32 pxiSendAndReceive(unsigned channel, u32 command);
#define I2C_CAM0 0x7A
#define I2C_CAM1 0x78
#define I2C_PM 0x4A
#define I2CREGPM_CAMLED 0x31
void swiWaitForVBlank(void);
u8 i2cWriteRegister(u8 device, u8 reg, u8 value);
u8 i2cReadRegister(u8 device, u8 reg);
#endif
