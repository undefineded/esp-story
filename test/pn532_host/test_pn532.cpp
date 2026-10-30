#include "pn532.h"
#include <Wire.h>
#include <cassert>
FakeWire Wire;
int main(){
    assert(Pn532::begin());
    assert(Pn532::present() && Pn532::firmwareMajor()==1 && Pn532::firmwareMinor()==6);
    Pn532::Target target;
    assert(Pn532::listPassiveTarget(target) && !target.found);
    Wire.corrupt=true;
    assert(!Pn532::probe());
    Wire.corrupt=false;Wire.present=false;
    assert(!Pn532::begin() && !Pn532::present());
    Wire.present=true;assert(Pn532::begin());
}
