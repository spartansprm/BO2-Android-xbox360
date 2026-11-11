// SPDX-License-Identifier: GPL-3.0-only
#include "../android/game/touch_look.h"
#include <cassert>
#include <iostream>
#include <limits>
using bo2zm::android::TouchLook;
int main(){
  int checks=0;auto check=[&](bool yes){++checks;assert(yes);};
  TouchLook look;look.Touch(0,0,0,0,1920,1080);look.Touch(1,1,.5f,.5f,1920,1080);
  auto a=look.Consume();check(a.x==0&&a.y==0);
  look.Touch(1,0,.5f,.5f,1920,1080);a=look.Consume();check(a.x==960&&a.y==540);
  a=look.Consume();check(a.x==0&&a.y==0);
  look.Touch(2,0,1,1,1920,1080);a=look.Consume();check(a.x==960&&a.y==540);
  look.Touch(1,0,0,0,1920,1080);a=look.Consume();check(a.x==0&&a.y==0);
  for(auto size: {std::pair{1280.f,720.f},std::pair{1920.f,1080.f},std::pair{2400.f,1080.f},std::pair{2560.f,1440.f}}){
    look.Cancel();look.Touch(0,3,0,0,size.first,size.second);look.Touch(1,3,size.second/size.first*.5f,.5f,size.first,size.second);
    a=look.Consume();check(std::abs(a.x-540)<=1&&a.y==540);
  }
  look.Cancel();look.Touch(0,0,0,0,1280,720);int total=0;
  for(int i=1;i<=100;++i){look.Touch(1,0,i/1280.f/10,0,1280,720);total+=look.Consume().x;}
  check(total>=14&&total<=15);
  look.Cancel();a=look.Consume();check(a.x==0&&a.y==0);
  look.Touch(0,0,0,0,1280,720);look.Touch(1,0,std::numeric_limits<float>::infinity(),0,1280,720);a=look.Consume();check(a.x==0&&a.y==0);
  look.Touch(1,0,1,0,1280,720);look.Cancel();a=look.Consume();check(a.x==0&&a.y==0);
  std::cout<<"Relative touch: "<<checks<<" checks passed\n";
}
