// Process UI-owner-thread input retirement before a topology commit.
#pragma once
#include "node.h"
#include "tree_internal.h"
#include <vector>
namespace gea::embedded::ui {
using InputRetirementRoots = std::vector<NodeHandle>;
inline bool inputOwnerLeaving(const NodeHandle &owner,const InputRetirementRoots &removed,const InputRetirementRoots &retained) {
 if(!owner.valid()) return false;
 auto &tree=Tree::instance();
 bool leaving=false;
 for(const auto &root:removed) if(root.valid()&&tree.containsNode(root.id(),owner.id())) {leaving=true;break;}
 if(!leaving)return false;
 for(const auto &root:retained) if(root.valid()&&tree.containsNode(root.id(),owner.id())) return false;
 return true;
}
void retirePointerInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &);
void cancelPrimaryPointerInput();
// A programmatic scroll of a `snap-page` scroller animates to its target
// (engine/ui/input.cpp); false when the node does not snap to pages.
bool animateSnapPageScroll(int node, bool horizontal, int target);
void retireTouchInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &);
// Weak Core default; actual native platform providers must override this.
void retirePlatformInputForSubtrees(const InputRetirementRoots &,const InputRetirementRoots &);
}
