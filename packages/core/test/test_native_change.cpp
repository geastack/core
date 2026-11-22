#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include "ui/virtual_keyboard.h"
#include <cassert>
#include <cstdio>
#include <cstring>
namespace gea::framework::graphics::generated{void ensureLinked(){}}
namespace gea::framework::app::generated{void drainMicrotasks(){}}
using namespace gea::embedded::ui;using namespace gea::framework::events;
static void key(const char*label){auto&t=Tree::instance();VirtualKeyboard::instance().sync();int id=-1;for(int i=0;i<t.nodeCount();++i)if(t.node(i).text==label)id=i;assert(id>=0);PointerEvent event{};event.type=PointerEventType::Click;event.targetId=id;t.dispatchEvent(event);}
// Portrait, so the compact keyboard is laid out: its keys are labelled with
// text ("return", "delete"); the landscape layout draws icons on those keys.
static NodeHandle setup(const char*tag="input"){auto&t=Tree::instance();auto&d=Document::instance();t.clear();auto root=d.createView();auto input=d.createElement(tag);root.appendChild(input);d.mount(root,480,640);return input;}
int main(){auto&t=Tree::instance();
 {auto input=setup();int edits=0,changes=0;t.setEventListener(input.id(),"input",[&](auto&){++edits;});t.setEventListener(input.id(),"change",[&](auto&e){assert(std::strcmp(e.typeName(),"change")==0&&!e.cancelable);++changes;});input.focus();key("q");assert(edits==1&&changes==0);assert(std::strcmp(input.getAttribute("value"),"q")==0);input.blur();fprintf(stderr,"actual virtual-keyboard edits=%d commits=%d\n",edits,changes);assert(changes==1);input.blur();assert(changes==1);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});input.focus();input.blur();assert(changes==0);input.focus();input.setAttribute("value","programmatic");input.blur();assert(changes==0);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});input.focus();key("q");assert(changes==0);key("return");assert(changes==1);key("return");input.blur();assert(changes==1);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});input.focus();key("q");key("delete");input.blur();assert(changes==0);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});input.focus();key("q");input.setDisabled(true);key("q");assert(std::strcmp(input.getAttribute("value"),"q")==0);input.blur();assert(changes==0);}
 {auto input=setup("textarea");int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});input.focus();key("q");key("return");assert(changes==0);input.blur();assert(changes==1);}
 {auto input=setup();int changes=0;input.focus();key("q");const int old=input.id();t.removeNode(old);auto replacement=Document::instance().createElement("input");assert(replacement.id()==old);t.setEventListener(replacement.id(),"change",[&](auto&){++changes;});t.setActiveInput(-1);assert(changes==0);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;t.clear();Document::instance().createElement("input");});input.focus();key("q");input.blur();assert(changes==1);}
 {auto input=setup();int changes=0;t.setEventListener(input.id(),"change",[&](auto&){++changes;});t.setEventListener(input.id(),"input",[&](auto&){input.setDisabled(true);input.blur();});input.focus();key("q");assert(changes==0);}
 t.clear();puts("PASS genuine virtual-keyboard input versus commits, unchanged/programmatic/reverted values, Return/textarea, disabled/stale/reentrant authority; CPU native text path only");}
