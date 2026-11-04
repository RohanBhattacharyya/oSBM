#include "StarTouchInput.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

static unsigned allocations = 0;
void* operator new(std::size_t size) {
  ++allocations;
  if (auto p=std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p, std::size_t) noexcept {std::free(p);}

using namespace Star::TouchInput;
static unsigned checks = 0;
static void check(bool value, char const* message) {
  ++checks;
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
  for (int degrees = 0; degrees < 360; ++degrees) {
    float angle = degrees * 0.01745329252f;
    for (float length : {0.0f, 0.14f, 0.15f, 0.3f, 0.8f, 1.5f}) {
      auto value = stick(std::cos(angle) * length, std::sin(angle) * length, .15f, 1.4f);
      check(std::isfinite(value[0]) && std::isfinite(value[1]), "finite stick");
      check(std::hypot(value[0], value[1]) <= 1.00001f, "radial clamp");
      if (length < .15f) check(value[0] == 0 && value[1] == 0, "neutral drift rejected");
      if (length > .15f) check(std::abs(value[0] * std::sin(angle) - value[1] * std::cos(angle)) < .0001f, "angle preserved");
    }
  }
  Movement movement;
  float previous = 0;
  for (int i=0;i<=1000;++i) {
    float value=stick(i/1000.0f,0,.15f,1.4f)[0];
    check(value>=previous && value<=1,"monotonic fine aim curve");
    previous=value;
  }
  float travel60=0,travel90=0;
  for(int i=0;i<60;++i) travel60+=aimTravel(56,1,1.0f/60);
  for(int i=0;i<90;++i) travel90+=aimTravel(56,1,1.0f/90);
  check(std::abs(travel60-travel90)<.01f,"precise aim independent of 60/90 Hz");
  check(aimTravel(56,1,1)==aimTravel(56,1,.05f),"stall does not jump cursor");
  movement.update(.31f, 0); check(movement.right, "direction starts");
  for (float x : {.29f, .25f, .21f, .28f}) { movement.update(x, 0); check(movement.right, "hysteresis no flicker"); }
  movement.update(.20f, 0); check(!movement.right, "release at off threshold");
  movement.update(.8f, .31f); check(movement.right && !movement.down, "horizontal axis lock");
  movement.update(.31f, .8f); check(!movement.right && movement.down, "vertical axis lock");
  movement.update(.8f, .8f); check(movement.right && movement.down, "deliberate diagonal");
  movement.update(-.8f, -.8f); check(movement.left && movement.up && !movement.right && !movement.down, "return across origin");
  Swipe swipe;
  check(swipe.move(-25) == 0 && swipe.move(-2) == 1, "left swipe step");
  check(swipe.move(53) == -2 && swipe.accumulated == 0, "reverse swipe remainder");
  check(wheel(0, -30, 30)[1] == 1 && wheel(0, 30, 30)[1] == -1, "wheel Y up");
  check(wheel(0, 0, 0)[0] == 0, "zero wheel reach");
  check(edge(0, 50, 100, 100) && !edge(50, 50, 100, 100), "palm edge");
  check(idleFade(0) == 1 && idleFade(3500) == 1 && std::abs(idleFade(4300) - .45f) < .0001f, "idle fade bounds");
  check(minimumTarget(3) == 144 && tapThreshold(3) == 30, "density independent targets");
  AimAttack attack;
  check(!attack.update(AttackMode::AimOnly,true) && !attack.release(AttackMode::AimOnly,false),"default aim does not attack");
  Contact moveFinger, aimFinger;
  check(moveFinger.claim(1) && aimFinger.claim(2), "independent two thumbs");
  check(!moveFinger.claim(2) && moveFinger.owns(1), "second finger cannot steal stick");
  for (int i = 0; i < 500; ++i) {
    movement.update(i % 2 ? .8f : -.8f, 0);
    check(attack.update(AttackMode::Continuous, true), "continuous mining survives movement");
  }
  check(!attack.release(AttackMode::Continuous, false) && !attack.held, "continuous releases");
  attack.update(AttackMode::Release, true);
  check(attack.release(AttackMode::Release, false), "release attack once");
  check(!attack.release(AttackMode::Release, false), "no phantom release attack");
  attack.update(AttackMode::Release, true);
  check(!attack.release(AttackMode::Release, true), "focus loss cannot fire");
  attack.update(AttackMode::Release,true);attack.update(AttackMode::Release,false);
  check(!attack.release(AttackMode::Release,false),"return to deadzone cancels release attack");
  for (int reason = 0; reason < 5; ++reason) {
    moveFinger.clear(); aimFinger.clear(); attack.release(AttackMode::Continuous, true); movement = {};
    check(!moveFinger.active && !aimFinger.active && !attack.held && !movement.left, "lifecycle reset");
    check(moveFinger.claim(1), "new contact after reset");
  }
  Gesture gesture;
  gesture.down(50, 50, 0); check(!gesture.longPress(.44) && gesture.longPress(.45), "long press threshold");
  check(!gesture.tap(), "long press suppresses tap");
  gesture.down(50, 50, 1); check(!gesture.move(55, 55, 10) && gesture.move(70, 50, 10), "drag threshold");
  check(!gesture.tap() && !gesture.longPress(2), "drag cannot tap or long press");
  gesture.cancel(); check(!gesture.move(0, 0, 1) && !gesture.tap(), "cancelled contact inert");
  Inertia inertia; inertia.velocity = 200;
  float distance = 0;
  for (int i = 0; i < 200; ++i) distance += inertia.step(1.0f / 60);
  check(inertia.velocity == 0 && distance > 19 && distance < 21, "bounded inertia decay");
  struct Element { std::string id; Point position; float size; int action; bool enabled; };
  std::vector<Element> saved{{"move",{.2f,.7f},1,42,false},{"custom",{.8f,.2f},1.8f,77,true}};
  std::vector<Element> defaults{{"move",{.1f,.8f},1,1,true},{"singleBlock",{.7f,.3f},.7f,2,true}};
  mergeLayout(saved,defaults,[](auto& old,auto const& value){old.position=value.position;old.size=value.size;});
  check(saved.size()==3 && saved[1].id=="custom" && saved[1].action==77, "preset preserves custom element");
  check(saved[0].action==42 && !saved[0].enabled, "preset preserves remapping and disabled state");
  check(saved[2].id=="singleBlock", "new default merged into old layout");
  std::vector<Element> profile{{"move",{.3f,.5f},1.3f,5,true}};
  mergeLayout(saved,profile,[](auto& old,auto const& value){old=value;});
  check(saved.size()==3 && saved[0].action==5 && saved[1].action==77, "profile replaces named element without dropping others");
  Point point{.2f,.8f};float size=1;
  preset(point,size,3);check(std::abs(point[0]-.8f)<.0001f && point[1]==.8f,"left handed mirror");
  preset(point,size,3);check(std::abs(point[0]-.2f)<.0001f,"mirror reversible");
  preset(point,size,1);check(size==.8f,"compact preset");
  preset(point,size,2);check(size==1,"large preset");
  check(overlaps(.3f,.1f,.05f,.05f,.24f,0,.78f,.14f),"hotbar overlap detected");
  check(!overlaps(.9f,.8f,.03f,.03f,.24f,0,.78f,.14f),"free layout position accepted");
  // Exercise the production layout policy at landscape sizes, including the
  // safe canvas of the user's screenshot, also with optional Lock/Next. Circles must stay in
  // bounds, below the top HUD, and visually separate from each other.
  check(controlRadius(540, 1)==56 && controlRadius(720, 1)==56 && controlRadius(1080, 2)==168,
      "editor and runtime use the same minimum radius and global size scaling");
  char const* ids[] = {"joystick", "aimJoystick", "leftHand", "rightHand", "jump", "interact",
      "pause", "chat", "tech", "inventoryWheel", "actionWheel", "singleBlock", "swapAimHand", "targetLock", "targetNext"};
  for (Point canvas : {Point{960,540}, Point{1230,562}, Point{1280,720}, Point{1920,1080}, Point{2340,1080}}) {
    std::array<Point, 15> centers{};
    std::array<float, 15> radii{};
    for (unsigned i=0; i<15; ++i) {
      Point p{}; float scale=0;
      check(comfortablePreset(ids[i], p, scale), "comfortable layout covers every enabled built-in control");
      centers[i] = {p[0]*canvas[0], p[1]*canvas[1]};
      radii[i] = controlRadius(canvas[1], 1) * scale * (i < 2 ? 1.0f : .55f);
      check(centers[i][0]-radii[i]>=0 && centers[i][0]+radii[i]<=canvas[0]
          && centers[i][1]-radii[i]>=.16f*canvas[1] && centers[i][1]+radii[i]<=canvas[1],
          "comfortable controls fit safe canvas and clear top HUD at default global size");
      for (unsigned j=0; j<i; ++j)
        check(std::hypot(centers[i][0]-centers[j][0], centers[i][1]-centers[j][1]) > radii[i]+radii[j],
            "comfortable visible control circles do not overlap");
    }
  }
  Point unchanged{.3f,.4f}; float unchangedSize=1.7f;
  check(!comfortablePreset("custom",unchanged,unchangedSize) && unchanged==Point{.3f,.4f} && unchangedSize==1.7f,
      "comfortable layout leaves custom geometry untouched");
  std::vector<Element> remapped{{"jump",{.2f,.3f},.7f,99,false},{"custom",{.4f,.3f},1.5f,77,true}};
  auto incoming=remapped;
  for (auto& element : incoming) comfortablePreset(element.id,element.position,element.size);
  mergeLayout(remapped,incoming,[](auto& old,auto const& value){old.position=value.position;old.size=value.size;});
  check(remapped[0].action==99 && !remapped[0].enabled && remapped[0].size==1.4f,
      "comfortable layout keeps disabled state and remapped jump action");
  check(remapped[1].position==Point{.4f,.3f} && remapped[1].action==77,
      "comfortable layout keeps custom control position and action");
  unsigned before=allocations;
  float checksum=0;
  for(int frame=0;frame<20000;++frame) {
    auto value=stick(.5f,.3f,.15f,1.4f);
    movement.update(value[0],value[1]);
    checksum+=wheel(value[0],value[1],1)[0]+idleFade((float)frame);
    attack.update(AttackMode::Continuous,frame%4!=0);
    swipe.move(value[0]);inertia.step(1.0f/90);
    gesture.down(0,0,frame);gesture.move(.1f,.2f,10);gesture.cancel();
  }
  check(allocations==before && checksum>0,"steady pure input makes zero allocations");
  std::printf("Pure input allocation audit: 20000 frames, %u allocations\n",allocations-before);
  std::printf("Passed %u touch input checks\n", checks);
}
