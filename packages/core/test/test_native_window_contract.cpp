#include "ui/document.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include "host/window.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::graphics::generated {void ensureLinked(){}}
void __gea_top_level();
using namespace gea::embedded::ui;
using namespace gea::framework::events;
int main(){
 auto &tree=Tree::instance();tree.clear();
 auto &sheet=StyleSheet::instance();
 sheet.registerSelectorRule(".native-style-probe::before","content","\"probe\"");
 sheet.registerSelectorRule(".native-style-probe::before","padding-top","19px");
 auto root=Document::instance().createElement("main");auto button=Document::instance().createElement("button");
 tree.setParent(button.id(),root.id());tree.mount(root.id(),320,240);
 Document::setPreferredMountSize(320,240);
 __gea_top_level();
 PointerEvent first{};first.type=PointerEventType::TouchEnd;first.targetId=button.id();first.clientX=41;
 tree.dispatchEvent(first);assert(first.defaultPrevented);
 PointerEvent second{};second.type=PointerEventType::TouchEnd;second.targetId=button.id();second.clientX=41;
 tree.dispatchEvent(second);assert(!second.defaultPrevented); // Genuine callback identity removal + once.
 PointerEvent move{};move.type=PointerEventType::TouchMove;move.targetId=button.id();move.clientX=41;
 tree.dispatchEvent(move);assert(!move.defaultPrevented);
 assert(!dispatchWindowLifecycleEvent(PointerEventType::WindowBlur));
 Document::setPreferredMountSize(640,480);
 assert(dispatchWindowLifecycleEvent(PointerEventType::WindowResize));
 PointerEvent resized{};resized.type=PointerEventType::TouchStart;resized.targetId=button.id();resized.clientX=41;
 tree.dispatchEvent(resized);assert(resized.defaultPrevented);
 PointerEvent resizedAgain{};resizedAgain.type=PointerEventType::TouchStart;resizedAgain.targetId=button.id();resizedAgain.clientX=41;
 tree.dispatchEvent(resizedAgain);assert(!resizedAgain.defaultPrevented);
 tree.clear();assert(!dispatchWindowLifecycleEvent(PointerEventType::WindowResize));
#if GEA_CSS_SCROLLING
 auto scroller = Document::instance().createView();
 auto content = Document::instance().createView();
 scroller.style().setProperty("display", "block");
 scroller.style().setProperty("overflow-x", "scroll");
 scroller.style().setProperty("overflow-y", "scroll");
 scroller.style().width(200); scroller.style().height(100);
 content.style().width(640); content.style().height(600);
 scroller.appendChild(content);
 tree.mount(scroller.id(), 200, 100);
 tree.computeLayout(scroller.id(), 200, 100);
 gea::host::window.scrollTo(25, 50);
 assert(gea::host::window.scrollX() == 25 && gea::host::window.scrollY() == 50);
 gea::host::window.scrollTo(-25, -50);
 assert(gea::host::window.scrollX() == 0 && gea::host::window.scrollY() == 0);
#endif
 puts("PASS emitted typed native window callback identity/removal/once/options/shared event flags; CPU Tree events only, no OS window or global input");
}
