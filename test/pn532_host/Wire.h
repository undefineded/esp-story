#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
struct FakeWire {
    bool present=true,corrupt=false,ack=false;
    std::vector<uint8_t> command,rx;
    std::size_t position=0;
    void begin(int,int,uint32_t){}
    void end(){}
    void setClock(uint32_t){}
    void beginTransmission(uint8_t){command.clear();}
    std::size_t write(const uint8_t* data,std::size_t size){command.assign(data,data+size);return size;}
    int endTransmission(bool=true){if(!present)return 2;if(!command.empty())ack=true;return 0;}
    uint8_t requestFrom(uint8_t,uint8_t length){
        rx.clear();position=0;
        if(!present)return 0;
        rx.push_back(1);
        if(length==1)return 1;
        if(ack){
            const uint8_t bytes[]={0,0,255,0,255,0};
            rx.insert(rx.end(),bytes,bytes+6);ack=false;
        }else{
            const uint8_t cmd=command.at(6);
            std::vector<uint8_t> payload;
            if(cmd==2)payload={0x32,1,6,7};
            if(cmd==0x4a)payload={0};
            uint8_t n=payload.size()+2;
            const uint8_t header[]={0,0,255,n,uint8_t(-n),0xd5,uint8_t(cmd+1)};
            rx.insert(rx.end(),header,header+7);
            rx.insert(rx.end(),payload.begin(),payload.end());
            uint8_t sum=uint8_t(0xd5+cmd+1);for(auto value:payload)sum+=value;
            rx.push_back(uint8_t(-sum));rx.push_back(0);
            if(corrupt)rx[5]^=1;
        }
        rx.resize(length,0);return length;
    }
    bool available(){return position<rx.size();}
    int read(){return available()?rx[position++]:-1;}
};
extern FakeWire Wire;
