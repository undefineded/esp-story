#include "../src/transfer_frame.h"
#include <cassert>
int main() {
  uint8_t vector[] = {0x53,0x42,0x58,0x34,2,0,3,0,0x88,0x40,0x47,0x58,0x85,0x59,0x98,6,0x7b,0x7d,0,10,255};
  uint8_t encoded[sizeof(vector)] = {};
  memcpy(encoded+16,vector+16,5);
  TransferFrame::header(encoded,2,3);
  assert(memcmp(encoded,vector,sizeof(vector))==0);
  static uint8_t frame[TransferFrame::Capacity];
  const char* json="{\"v\":4,\"id\":7}";
  const size_t length=strlen(json);
  memcpy(frame+16,json,length);
  for(size_t i=0;i<4096;i++)frame[16+length+i]=uint8_t(i);
  TransferFrame::header(frame,length,4096);
  const size_t total=16+length+4096;
  static TransferFrame::Decoder decoder;
  for(int pass=0;pass<2;pass++) {
    for(size_t i=0;i<total;i++)assert(decoder.feed(frame[i])==(i==total-1));
    assert(memcmp(decoder.bytes,frame,total)==0);decoder.reset();
  }
  frame[20]^=1;
  for(size_t i=0;i<total;i++)assert(!decoder.feed(frame[i]));
  assert(decoder.used==0);
}
