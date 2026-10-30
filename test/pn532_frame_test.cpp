#include "../src/pn532_frame.h"
#include <cassert>
int main() {
    // PN532 GetFirmwareVersion response: IC=32, version=1.6, support=07.
    uint8_t firmware[]={0,0,0xff,6,0xfa,0xd5,3,0x32,1,6,7,0xe8,0};
    uint8_t output[16],size=0;
    assert(Pn532Frame::decode(firmware,sizeof(firmware),3,output,size,sizeof(output)));
    assert(size==4 && output[0]==0x32 && output[2]==6);
    assert(!Pn532Frame::decode(firmware,sizeof(firmware),4,output,size,sizeof(output)));
    assert(!Pn532Frame::decode(firmware,sizeof(firmware)-1,3,output,size,sizeof(output)));
    assert(!Pn532Frame::decode(firmware,sizeof(firmware),3,output,size,3));
    firmware[4]^=1;
    assert(!Pn532Frame::decode(firmware,sizeof(firmware),3,output,size,sizeof(output)));
    firmware[4]^=1;firmware[11]^=1;
    assert(!Pn532Frame::decode(firmware,sizeof(firmware),3,output,size,sizeof(output)));
    // SAMConfiguration has an empty response payload (LEN=2, not >=3).
    const uint8_t sam[]={0,0,0xff,2,0xfe,0xd5,0x15,0x16,0};
    assert(Pn532Frame::decode(sam,sizeof(sam),0x15,nullptr,size,0) && size==0);
}
