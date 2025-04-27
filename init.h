

#ifndef __INIT_H__
#define __INIT_H__

#ifdef __cplusplus
extern "C" {
#endif

const unsigned int SC320AT_CFG[][2] = {
    {0x3034,0x0b}, //0x03 //Enable E2 bridge
    {0x3641,0x92}, //0x12 //Enable E2 bridge

    //GPIO5- low 
    {0x300c, 0x22}, //0x02
    {0x3017, 0x20}, //0x00
    {0x3016, 0x00},	//1 high, 0 low
};

#ifdef __cplusplus
}
#endif

#endif
