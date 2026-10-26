#ifndef TEST_NDS_H
#define TEST_NDS_H
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
#define I2C_CAM0 0x7A
#define I2C_CAM1 0x78
#define I2C_PM 0x4A
#define I2CREGPM_CAMLED 0x31
void swiWaitForVBlank(void);
u8 i2cWriteRegister(u8 device, u8 reg, u8 value);
u8 i2cReadRegister(u8 device, u8 reg);
#endif
