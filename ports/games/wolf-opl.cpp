/* Integer DOSBox OPL2 output for Wolf4SDL; GPL-2.0-or-later. */
#include "b310e-dbopl.h"
#include "../fpdoom/b310e-audio.h"
static DBOPL::Chip *chip;
extern "C" void b310e_opl_init(void) {
    if (!chip) chip=new DBOPL::Chip;
    chip->Setup(22050); chip->WriteReg(1,0x20);
}
extern "C" void b310e_opl_write(unsigned reg,unsigned value) {
    if(chip) chip->WriteReg(reg,value);
}
extern "C" void b310e_opl_block(int32_t *samples,unsigned frames) {
    if(chip) chip->GenerateBlock2(frames,samples);
    else for(unsigned i=0;i<frames;i++) samples[i]=0;
}
