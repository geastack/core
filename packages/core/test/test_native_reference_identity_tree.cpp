#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#ifndef GEA_TEST_RUNTIME_HEADER
#error Actual shared compiler runtime header required
#endif
#include GEA_TEST_RUNTIME_HEADER
#include "ui/tree_internal.h"
#include <cassert>
namespace gea::framework::app::generated { void drainMicrotasks() {} }
namespace gea::framework::graphics::generated { void ensureLinked() {} }
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
 const int second=tree.createButton();
 assert(second==first);
 gea::embedded::ui::NodeHandle next(second), fromOld(retained);
 assert(next.identity()!=old.identity());
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
 tree.clear();
 const int third=tree.createButton();
 assert(third==first);
 assert(gea::embedded::ui::NodeHandle(third).identity()!=next.identity());
 assert(gea::weakCollectionIdentity(gea::embedded::ui::NodeHandle(third))!=gea::weakCollectionIdentity(next));
 // A program holds the one document through Document carrier copies; each
 // copy must name that document as a weak key, and no node may share it.
 auto &document=gea::embedded::ui::Document::instance();
 const gea::embedded::ui::Document documentCopy=document;
 assert(gea::weakCollectionIdentity(document)==gea::weakCollectionIdentity(documentCopy));
 assert(gea::weakCollectionIdentity(document)!=gea::weakCollectionIdentity(gea::embedded::ui::NodeHandle(third)));
 gea::WeakMap<gea::embedded::ui::Document,int> perDocument;
 perDocument.set(documentCopy,7);
 assert(perDocument.has(document));
 perDocument.set(document,8);
 assert(*perDocument.get(documentCopy)==8);
 tree.clear();
 assert(perDocument.has(gea::embedded::ui::Document::instance())); // clearing the tree keeps the document
}
