#ifndef _DRVI2C_CXD6801_ITE_H_
#define _DRVI2C_CXD6801_ITE_H_

#include "sony_cxd6801_i2c.h"
#include "IT9300.h"


typedef struct drvi2c_cxd6801_ite_t
{
    Endeavour*		endeavour_sony;
	uint8_t			i2cbus;
} drvi2c_cxd6801_ite_t;

void drvi2c_cxd6801_ite_Initialize(drvi2c_cxd6801_ite_t* pDrvI2c, Endeavour* endeavour, uint8_t i2cbus);
void drvi2c_cxd6801_ite_CreateI2c(sony_cxd6801_i2c_t* pI2c, drvi2c_cxd6801_ite_t* pDrvI2c);


#endif