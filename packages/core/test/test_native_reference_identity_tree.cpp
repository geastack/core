#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#ifndef GEA_TEST_RUNTIME_HEADER
#error Actual shared compiler runtime header required
#endif
#include GEA_TEST_RUNTIME_HEADER
#include "host/window.h"
#include "program.h"
#include "ui/tree_internal.h"
#include <cassert>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
namespace gea::framework::program { const Hooks *loaded() { return nullptr; } }
int main() {
 auto &tree=gea::embedded::ui::Tree::instance();
 tree.clear();
 const int first=tree.createButton();
 gea::embedded::ui::NodeHandle old(first), same(first);
 gea::framework::events::EventTarget retained(first);
 assert(old.identity()==same.identity());
 // This is the actual emitter's existing identity hook, not raw-id equality.
 assert(gea::weakCollectionIdentity(old)==gea::weakCollectionIdentity(same));
 auto nodeCopy=old;
 assert(gea::weakCollectionIdentity(old)==gea::weakCollectionIdentity(nodeCopy));
 std::weak_ptr<void> propertyOwner;
 {
   const auto key=gea::PropertyKey::string("owner");
   using Handle=gea::embedded::ui::NodeHandle;
   using Policy=gea::NativeFieldLeafPolicy;
   assert((!gea::host::element::ownNativeProperty<Handle, Policy>(old, key)));
   assert(gea::host::element::setOwnNativeProperty<Policy>(gea::makeRef<Handle>(old), key, old));
   propertyOwner=old.propertyStorage();
   const auto read=gea::host::element::ownNativeProperty<Handle, Policy>(nodeCopy, key);
   assert(read && read->identity()==old.identity());
 }
 assert(!propertyOwner.expired());
 auto targetCopy=retained;
 assert(gea::weakCollectionIdentity(retained)==gea::weakCollectionIdentity(targetCopy));
 const int distinct=tree.createButton();
 gea::embedded::ui::NodeHandle other(distinct);
 gea::framework::events::EventTarget otherTarget(distinct);
 assert(gea::weakCollectionIdentity(old)!=gea::weakCollectionIdentity(other));
 assert(gea::weakCollectionIdentity(retained)!=gea::weakCollectionIdentity(otherTarget));
 gea::framework::events::PointerEvent occurrence{},fresh{};
 auto eventCopy=occurrence;
 assert(gea::weakCollectionIdentity(occurrence)==gea::weakCollectionIdentity(eventCopy));
 assert(gea::weakCollectionIdentity(occurrence)!=gea::weakCollectionIdentity(fresh));
 fresh=occurrence;
 assert(gea::weakCollectionIdentity(occurrence)==gea::weakCollectionIdentity(fresh));
 tree.removeNode(first);
 assert(propertyOwner.expired());
 assert(!gea::host::element::propertyTable(old, true));
 const int second=tree.createButton();
 assert(second==first);
 gea::embedded::ui::NodeHandle next(second), fromOld(retained);
 assert(next.identity()!=old.identity());
 assert(!gea::host::element::propertyTable(next));
 assert(fromOld.identity()==old.identity());
 gea::framework::events::EventTarget replacementTarget(second);
 assert(gea::weakCollectionIdentity(next)!=gea::weakCollectionIdentity(old));
 assert(gea::weakCollectionIdentity(replacementTarget)!=gea::weakCollectionIdentity(retained));
 assert(gea::weakCollectionIdentity(fromOld)==gea::weakCollectionIdentity(old));
 assert(gea::weakCollectionIdentity(targetCopy)==gea::weakCollectionIdentity(retained));
 std::weak_ptr<const void> liveToken;
 const int live=tree.createButton();
 { gea::embedded::ui::NodeHandle temporary(live); liveToken=temporary.weakIdentityOwner(); }
 assert(!liveToken.expired()); // live Tree owns identity without a wrapper
 tree.removeNode(live);
 assert(liveToken.expired()); // collection keys must not keep removed nodes alive
 gea::host::element::propertyTable(next, true);
 std::weak_ptr<void> propertyBeforeClear=next.propertyStorage();
 assert(!propertyBeforeClear.expired());
 tree.clear();
 assert(propertyBeforeClear.expired());
 const int third=tree.createButton();
 assert(third==first);
 assert(gea::embedded::ui::NodeHandle(third).identity()!=next.identity());
 assert(gea::weakCollectionIdentity(gea::embedded::ui::NodeHandle(third))!=gea::weakCollectionIdentity(next));
 {
   using Policy = gea::NativeFieldLeafPolicy;
   const auto key = gea::PropertyKey::string("cache");
   const auto document = gea::host::documentValue;
   const auto alias = document;
   const auto documentRef = gea::makeRef<gea::host::DocumentFacade>(alias);
   assert((!gea::host::element::ownNativeProperty<int, Policy>(document, key)));
   assert(gea::host::element::setOwnNativeProperty<Policy>(document, key, 42));
   assert((gea::host::element::ownNativeProperty<int, Policy>(alias, key) == 42));
   const auto present = gea::host::element::getOwnNativeProperty<int, Policy>(documentRef, key);
   assert(present.has_value() && *present == 42);
   std::weak_ptr<void> propertyOwner = document.propertyStorage();
   assert(!propertyOwner.expired());
   gea::embedded::ui::Document::instance().clear();
   assert(propertyOwner.expired());
   assert((!gea::host::element::ownNativeProperty<int, Policy>(alias, key)));
   assert((!gea::host::element::getOwnNativeProperty<int, Policy>(documentRef, key).has_value()));
 }
}
