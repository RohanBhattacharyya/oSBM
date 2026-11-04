#include "StarTargetLock.hpp"
#include <cstdio>
#include <cstdlib>

static unsigned checks = 0;
static void check(bool ok) { ++checks; if (!ok) std::exit(1); }
int main() {
  using namespace Star::TargetLock;
  check(score(30,1,true,true) < score(1,1,false,true));
  check(score(30,1,true,false) > score(1,1,false,false));
  check(score(10,1,true,true) < score(20,1,true,true));
  check(score(10,1,false,true) < score(10,.6f,false,true));
  check(std::isinf(score(1,.49f,true,true)));
  check(std::isfinite(score(1,.5f,true,true)));
  check(std::isinf(score(0,1,true,true)));
  check(std::isinf(score(NAN,1,true,true)));
  check(std::isinf(score(1,NAN,true,true)));
  Candidate previous;
  for (int step=0; step<9; ++step) {
    Chooser chooser; chooser.previous=previous;
    chooser.consider(3,30); chooser.consider(1,10); chooser.consider(2,20);
    auto result=chooser.result();
    check(result.valid && result.id==(step%3+1)*10);
    previous=result;
  }
  Chooser tie; tie.consider(1,20); tie.consider(1,10);
  check(tie.result().id==10);
  check(score(30,1,false,false)<score(1,.8f,false,false));
  for (unsigned mask=0;mask<128;++mask) {
    bool valid=retain(mask&1,mask&2,mask&4,mask&8,mask&16,mask&32,mask&64);
    check(valid == (mask==63));
  }
  std::printf("Passed %u target lock checks\n",checks);
}
