#include "b310e-dbopl.h"
#include "reference-dbopl.h"
#include <assert.h>
#include <stdio.h>
#include <chrono>
#include <algorithm>
int main() {
    using clock=std::chrono::steady_clock;
    DBOPL::Chip fast; DBOPLReference::Chip reference;
    auto t0=clock::now(); reference.Setup(22050); auto t1=clock::now();
    fast.Setup(22050);auto t2=clock::now();
    for(unsigned i=0;i<76;i++) {
        assert(fast.attackRates[i]==reference.attackRates[i]);
        assert(fast.linearRates[i]==reference.linearRates[i]);
    }
    /* Compare bulk output with the original one-sample path across key-on,
     * release, tremolo, percussion and fractional IMF event boundaries. */
    int energy=0;
    const unsigned regs[][2]={{1,32},{0x20,0xe1},{0x23,0xe1},{0x40,16},{0x43,0},
        {0x60,0xf3},{0x63,0xf3},{0x80,0x35},{0x83,0x35},{0xa0,0x98},{0xb0,0x31}};
    for(auto &reg:regs){fast.WriteReg(reg[0],reg[1]);reference.WriteReg(reg[0],reg[1]);}
    const unsigned operators[]={0,1,2,8,9,10,16,17,18};
    for(unsigned ch=1;ch<9;ch++) {
        for(unsigned op: {operators[ch],operators[ch]+3}) {
            const unsigned voice[][2]={{0x20+op,0xe1},{0x40+op,12},{0x60+op,0xf3},{0x80+op,0x35}};
            for(auto &reg:voice){fast.WriteReg(reg[0],reg[1]);reference.WriteReg(reg[0],reg[1]);}
        }
        fast.WriteReg(0xa0+ch,0x98+ch*7);reference.WriteReg(0xa0+ch,0x98+ch*7);
        fast.WriteReg(0xb0+ch,0x31);reference.WriteReg(0xb0+ch,0x31);
    }
    for(unsigned pos=0;pos<22050;) {
        unsigned n=std::min(22050-pos,(pos%3)?31u:32u);
        if(pos>5000 && pos<10000) {fast.WriteReg(0xbd,0xff);reference.WriteReg(0xbd,0xff);}
        if(pos>11025) {fast.WriteReg(0xb0,0x11);reference.WriteReg(0xb0,0x11);}
        int32_t output[32];fast.GenerateBlock2(n,output);
        for(unsigned i=0;i<n;i++) {
            int32_t sample;reference.GenerateBlock2(1,&sample);
            assert(sample==output[i]);energy+=sample<0?-sample:sample;
        }
        pos+=n;
    }
    assert(energy>10000);
    printf("PASS: OPL calibration and block PCM match original; host setup %.2f ms -> %.2f ms\n",
        std::chrono::duration<double,std::milli>(t1-t0).count(),
        std::chrono::duration<double,std::milli>(t2-t1).count());
}
