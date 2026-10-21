// Real TouchRuntime CPU contacts, no SDL/OS input, display or audio devices.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/input_retirement.h"
#include "touch.h"
#include "event.h"
#include "pixel.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <stdexcept>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
namespace gea::platform::touch { extern int gNativeTestMoveConsumes; }
using namespace gea::embedded::ui;
using namespace gea::framework::events;
static NodeHandle observedReplacement;
static unsigned int expectedReplacementBackground=0;
static bool observeRetirement=false,observedRetirement=false;
// Inert platform observation port: prove actual Core state before topology/style publication.
namespace gea::embedded::ui {void retirePlatformInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &){
 if(!observeRetirement)return;
 observedRetirement=true;assert(observedReplacement.valid());
 const auto &style=Tree::instance().node(observedReplacement.id()).computedStyle();
 assert(style.has_bg && style.bg_color==expectedReplacementBackground);
}}
static void contact(TouchPhase phase,int pointer=0,int x=20,int y=20){Event e{};e.type=EventType::Touch;e.touchPhase=phase;e.touching=phase!=TouchPhase::Up;e.x=x;e.y=y;e.pointerId=pointer;TouchRuntime::dispatchEvent(e);}
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];auto&doc=Document::instance();auto&tree=Tree::instance();tree.clear();
 auto root=doc.createView(),button=doc.createButton();button.style().width(120);button.style().height(40);root.appendChild(button);doc.mount(root,320,240);
 int down=0,up=0,cancel=0,click=0;
 button.addEventListener("pointerdown",[&](PointerEvent&){++down;});
 button.addEventListener("pointerup",[&](PointerEvent&){++up;});
 button.addEventListener("pointercancel",[&](PointerEvent&){++cancel;assert(button.isConnected());});
 button.addEventListener("click",[&](PointerEvent&){++click;});
 if(mode=="pressed-owner-aba"){
  auto wrapper=doc.createView();auto input=doc.createElement("input");button.style().backgroundColor(1234);root.appendChild(wrapper);wrapper.appendChild(button);wrapper.appendChild(input);doc.mount(root,320,240);
  input.focus();contact(TouchPhase::Down);const int oldId=button.id();
  const auto pressedBackground=tree.node(oldId).computedStyle().bg_color;assert(pressedBackground!=gea::framework::graphics::pixel::fromRgb565(1234));
  NodeHandle replacement;
  input.addEventListener("focusout",[&](auto&){button.remove();replacement=doc.createButton();assert(replacement.id()==oldId);replacement.style().backgroundColor(gea::framework::graphics::pixel::toRgb565(pressedBackground));wrapper.insertBefore(replacement,input);observedReplacement=replacement;expectedReplacementBackground=pressedBackground;observeRetirement=true;});
  root.removeChild(wrapper);assert(observedRetirement);
  assert(replacement.valid()&&tree.node(replacement.id()).computedStyle().has_bg&&tree.node(replacement.id()).computedStyle().bg_color==pressedBackground);
 }else if(mode=="retained-descendant"){
  auto wrapper=doc.createView();root.appendChild(wrapper);wrapper.appendChild(button);doc.mount(root,320,240);
  contact(TouchPhase::Down);root.replaceChildren({button});contact(TouchPhase::Up);
  assert(down==1&&cancel==0&&up==1&&click==1);
 }else if(mode=="unrelated"){
  auto other=doc.createButton();other.style().width(120);other.style().height(40);root.appendChild(other);doc.mount(root,320,240);
  int otherUp=0,otherClick=0,otherCancel=0;other.addEventListener("pointerup",[&](auto&){++otherUp;});other.addEventListener("click",[&](auto&){++otherClick;});other.addEventListener("pointercancel",[&](auto&){++otherCancel;});
  contact(TouchPhase::Down);contact(TouchPhase::Down,1,20,60);root.removeChild(button);root.appendChild(button);
  contact(TouchPhase::Up);contact(TouchPhase::Up,1,20,60);
  assert(cancel==1&&up==0&&click==0&&otherCancel==0&&otherUp==1&&otherClick==1);
 }else if(mode=="cancel-throw"){
  button.addEventListener("pointercancel",[&](auto&){if(cancel==1)throw std::runtime_error("authored cancellation");});
  contact(TouchPhase::Down);contact(TouchPhase::Down,1);bool refused=false;try{root.removeChild(button);}catch(...){refused=true;}
  assert(refused&&button.isConnected()&&cancel==2);
  contact(TouchPhase::Up);contact(TouchPhase::Up,1);assert(up==0&&click==0);
 }else if(mode=="cancel-reentrant"){
  auto other=doc.createView();root.appendChild(other);
  button.addEventListener("pointercancel",[&](auto&){other.appendChild(button);});
  contact(TouchPhase::Down);bool refused=false;try{root.removeChild(button);}catch(...){refused=true;}
  assert(refused&&button.parentHandle().identity()==other.identity()&&cancel==1);
  contact(TouchPhase::Up);assert(up==0&&click==0);
 }else if(mode=="down-detach"){
  button.addEventListener("pointerdown",[&](auto&){root.removeChild(button);root.appendChild(button);});
  contact(TouchPhase::Down);contact(TouchPhase::Move,0,21);contact(TouchPhase::Up,0,21);
  assert(down==1&&cancel==1&&up==0&&click==0);
 }else if(mode=="up-detach"){
  button.addEventListener("pointerup",[&](auto&){root.removeChild(button);root.appendChild(button);});
  contact(TouchPhase::Down);contact(TouchPhase::Up);
  assert(down==1&&up==1&&cancel==0&&click==0);
 }else{
  assert(mode=="primary"||mode=="secondary");const int pointer=mode=="secondary"?1:0;
  const auto oldBackground=tree.node(button.id()).computedStyle().bg_color;const auto oldHasBackground=tree.node(button.id()).computedStyle().has_bg;
  contact(TouchPhase::Down,pointer);assert(down==1);
  root.removeChild(button);root.appendChild(button);
  assert(tree.node(button.id()).computedStyle().bg_color==oldBackground&&tree.node(button.id()).computedStyle().has_bg==oldHasBackground);
  const int consumesBefore=gea::platform::touch::gNativeTestMoveConsumes;
  contact(TouchPhase::Move,pointer,21);contact(TouchPhase::Up,pointer,21);
  assert(cancel==1&&up==0&&click==0&&down==1);
  // The move that retires the detached gesture still drains the controller.
  if(pointer==0)assert(gea::platform::touch::gNativeTestMoveConsumes==consumesBefore+1);
  contact(TouchPhase::Down,pointer);contact(TouchPhase::Up,pointer);
  assert(down==2&&cancel==1&&up==1&&click==1);
 }
 tree.clear();puts("PASS actual TouchRuntime subtree detach cancellation");
}
