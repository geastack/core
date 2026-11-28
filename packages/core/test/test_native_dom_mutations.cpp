// Real Core CPU-only DOM mutation regressions. No renderer/window/input/device APIs.
#include "ui/document.h"
#include "ui/tree_internal.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <stdexcept>
namespace gea::framework::graphics::generated { void ensureLinked() {} }
template<class N> void removeChildContract(N parent, N child) {
  if constexpr (requires { parent.removeChild(child); }) {
    const auto removed = parent.removeChild(child);
    assert(removed.valid() && removed.identity() == child.identity());
    assert(child.valid() && !child.parentHandle().valid() && !child.isConnected());
    assert(std::string(child.getAttribute("data-retained")) == "yes");
    parent.appendChild(child);
    assert(child.valid() && child.isConnected() && child.parentHandle().identity() == parent.identity());
  } else {
    assert(false && "actual NodeHandle removeChild detach/reinsert API is absent");
  }
}
int main(int argc, char **argv) {
  using namespace gea::embedded::ui;
  assert(argc == 2);
  auto &tree = Tree::instance();
  auto &document = Document::instance();
  tree.clear();
  auto root = document.querySelector("#app");
  assert(root.valid());
  auto parent = document.createElement("div"), other = document.createElement("div");
  root.appendChild(parent); root.appendChild(other);
  document.mount(root,320,240);
  const std::string mode=argv[1];
  if (mode == "namespace-creation") {
    constexpr auto svg = "http://www.w3.org/2000/svg";
    constexpr auto math = "http://www.w3.org/1998/Math/MathML";
    const auto gradient = document.createElementNS(svg, "svg:linearGradient");
    const auto formula = document.createElementNS(math, "math");
    const auto noNamespace = document.createElementNS("", "item");
    assert(gradient.valid() && formula.valid() && noNamespace.valid());
    assert(gradient.domNodeName() == "svg:linearGradient");
    const auto clone = gradient.cloneNode(false);
    assert(clone.domNodeName() == gradient.domNodeName());
    assert(*clone.namespaceURI() == svg);
    assert(formula.domNodeName() == "math");
    assert(parent.domNodeName() == "DIV");
    const auto text = document.createText("text");
    const auto comment = document.createComment("comment");
    assert(text.domNodeName() == "#text");
    assert(comment.domNodeName() == "#comment");
    assert(text.characterData() && *text.characterData() == "text");
    assert(comment.characterData() && *comment.characterData() == "comment");
    comment.setCharacterData("updated");
    assert(*comment.characterData() == "updated");
    comment.setCharacterData(nullptr);
    assert(comment.characterData() && comment.characterData()->empty());
    parent.setCharacterData("ignored");
    assert(tree.node(parent.id()).text.empty());
    assert(parent.characterData() == nullptr);
    assert(NodeHandle{}.characterData() == nullptr);
    assert(document.createDocumentFragment().domNodeName() == "#document-fragment");
    assert(*gradient.namespaceURI() == svg);
    assert(*gradient.namespacePrefix() == "svg");
    assert(std::string(gradient.tagName()) == "linearGradient");
    assert(*formula.namespaceURI() == math && formula.namespacePrefix() == nullptr);
    assert(noNamespace.namespaceURI() == nullptr);
    assert(*parent.namespaceURI() == "http://www.w3.org/1999/xhtml");
    parent.appendChild(gradient);
    assert(parent.removeChild(gradient).identity() == gradient.identity());
    assert(*gradient.namespaceURI() == svg);
    for (const auto *name : {"", "a b", "xml:item", "xmlns"}) {
      bool rejected = false;
      try {
        document.createElementNS(svg, name);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      assert(rejected);
    }
  } else if (mode == "named-style-write") {
    const auto style = parent.style();
    const auto fontSize = [&] { return tree.node(parent.id()).computedStyle().font_size; };
    const auto originalSize = fontSize();
    assert(style.setNamedProperty("fontSize", "31px"));
    assert(fontSize() == 31);
    assert(style.setNamedProperty("fontSize", ""));
    assert(fontSize() == originalSize);
    assert(!style.setNamedProperty("notAStyleProperty", "value"));
  } else if(mode=="document-position") {
    const auto child = document.createElement("span");
    parent.appendChild(child);
    assert(parent.compareDocumentPosition(parent) == 0);
    assert(parent.compareDocumentPosition(child) == 20);
    assert(child.compareDocumentPosition(parent) == 10);
    assert(child.compareDocumentPosition(other) == 4);
    assert(other.compareDocumentPosition(child) == 2);
    root.insertBefore(other, parent);
    assert(child.compareDocumentPosition(other) == 2);
    root.removeChild(other);
    const int forward = child.compareDocumentPosition(other);
    const int backward = other.compareDocumentPosition(child);
    assert((forward == 35 && backward == 37) || (forward == 37 && backward == 35));
  } else if(mode=="replace-retained") {
    auto a=document.createElement("div"),b=document.createElement("span");
    parent.appendChild(a);parent.appendChild(b);
    const auto aIdentity=a.identity(),bIdentity=b.identity();
    parent.replaceChildren({b,a});
    assert(a.valid() && b.valid() && "replaceChildren must not destroy retained child allocations");
    assert(a.identity()==aIdentity && b.identity()==bIdentity);
    assert(parent.firstChildHandle().identity()==bIdentity && parent.childAt(1).identity()==aIdentity);
    assert(!b.previousSiblingHandle().valid() && a.previousSiblingHandle().identity()==bIdentity);
    parent.removeChild(a);
    assert(!a.previousSiblingHandle().valid() && !b.nextSiblingHandle().valid());
  } else if(mode=="foreign-reference") {
    auto child=document.createElement("div"),reference=document.createElement("span");
    other.appendChild(child);other.appendChild(reference);
    bool refused=false;
    try {parent.insertBefore(child,reference);} catch(...) {refused=true;}
    assert(refused && "foreign insertBefore reference must refuse, not silently append");
    assert(child.valid() && child.parentHandle().identity()==other.identity());
    assert(!parent.firstChildHandle().valid());
  } else if(mode=="remove-detach") {
    auto child=document.createElement("button");child.setAttribute("data-retained","yes");parent.appendChild(child);
    removeChildContract(parent,child);
  } else if(mode=="focus-detach") {
    auto child=document.createElement("div"),input=document.createElement("input");
    child.appendChild(input);parent.appendChild(child);
    int outs=0;input.addEventListener("focusout",[&](auto &){++outs;assert(child.isConnected());});
    input.focus();assert(tree.activeInputId()==input.id());
    child.setAttribute("data-retained","yes");removeChildContract(parent,child);
    assert(outs==1 && tree.activeInputId()<0);
    input.focus();assert(tree.activeInputId()==input.id());input.blur();assert(outs==2);
  } else if(mode=="focus-retained") {
    auto a=document.createElement("input"),b=document.createElement("div");parent.appendChild(a);parent.appendChild(b);
    int outs=0;a.addEventListener("focusout",[&](auto &){++outs;});a.focus();
    parent.replaceChildren({b,a});
    assert(tree.activeInputId()==a.id() && outs==0 && a.isConnected());
  } else if(mode=="focus-reentrant") {
    auto child=document.createElement("input"),sibling=document.createElement("div");parent.appendChild(child);
    child.addEventListener("focusout",[&](auto &){other.appendChild(child);parent.appendChild(sibling);});
    child.focus();bool refused=false;
    try {parent.removeChild(child);}catch(...){refused=true;}
    assert(refused && child.valid() && child.parentHandle().identity()==other.identity());
    assert(parent.firstChildHandle().identity()==sibling.identity() && !sibling.nextSiblingHandle().valid());
  } else if(mode=="focus-throw") {
    auto child=document.createElement("input");parent.appendChild(child);
    child.addEventListener("focusout",[](auto &){throw std::runtime_error("authored focus callback");});
    child.focus();bool refused=false;try {parent.removeChild(child);}catch(...){refused=true;}
    assert(refused && child.valid() && child.parentHandle().identity()==parent.identity());
    assert(parent.firstChildHandle().identity()==child.identity() && tree.activeInputId()<0);
  } else if(mode=="focus-refocus") {
    auto child=document.createElement("input");parent.appendChild(child);
    child.addEventListener("focusout",[&](auto &){child.focus();});child.focus();
    bool refused=false;try {parent.removeChild(child);}catch(...){refused=true;}
    assert(refused && child.isConnected() && child.parentHandle().identity()==parent.identity() && tree.activeInputId()==child.id());
    child.remove();
  } else if(mode=="replace-atomic") {
    auto child=document.createElement("div"),otherChild=document.createElement("span");parent.appendChild(child);other.appendChild(otherChild);
    bool refused=false;try {parent.replaceChildren({otherChild,root});}catch(...){refused=true;}
    assert(refused && parent.firstChildHandle().identity()==child.identity() && other.firstChildHandle().identity()==otherChild.identity());
  } else if(mode=="reference-aba") {
    auto input=document.createElement("input"),target=document.createElement("div");parent.appendChild(input);
    auto first=document.createElement("a"),reference=document.createElement("b"),last=document.createElement("i");
    target.appendChild(first);target.appendChild(reference);target.appendChild(last);
    const int oldId=reference.id();NodeHandle replacement;
    input.addEventListener("focusout",[&](auto &){reference.remove();replacement=document.createElement("b");assert(replacement.id()==oldId);target.insertBefore(replacement,last);});
    input.focus();bool refused=false;try {target.insertBefore(input,reference);}catch(...){refused=true;}
    assert(refused && input.parentHandle().identity()==parent.identity() && !reference.valid());
    assert(target.childAt(0).identity()==first.identity() && target.childAt(1).identity()==replacement.identity() && target.childAt(2).identity()==last.identity());
  } else if(mode=="fragment-direct-first") {
    auto fragment=document.createDocumentFragment(),a=document.createElement("a"),b=document.createElement("b");fragment.appendChild(a);fragment.appendChild(b);
    parent.replaceChildren({b,fragment});
    assert(parent.childAt(0).identity()==b.identity() && parent.childAt(1).identity()==a.identity());
    fragment.appendChild(a);fragment.appendChild(b);parent.replaceChildren({a,fragment,a});
    assert(parent.childAt(0).identity()==b.identity() && parent.childAt(1).identity()==a.identity());
    fragment.appendChild(a);fragment.appendChild(b);parent.replaceChildren({fragment,a,fragment});
    assert(parent.childAt(0).identity()==b.identity() && parent.childAt(1).identity()==a.identity());
  } else if(mode=="fragment-repeat") {
    auto fragment=document.createDocumentFragment(),a=document.createElement("a"),b=document.createElement("b"),x=document.createElement("i");
    fragment.appendChild(a);fragment.appendChild(b);
    parent.replaceChildren({fragment,x,fragment});
    assert(parent.childAt(0).identity()==a.identity() && parent.childAt(1).identity()==b.identity() && parent.childAt(2).identity()==x.identity());
    assert(!fragment.firstChildHandle().valid() && fragment.valid());
  } else if(mode=="move-focus") {
    auto input=document.createElement("input"),detached=document.createElement("div");parent.appendChild(input);
    int outs=0;input.addEventListener("focusout",[&](auto &){++outs;assert(input.isConnected());});input.focus();
    other.appendChild(input);assert(tree.activeInputId()==input.id() && outs==0);
    detached.appendChild(input);assert(tree.activeInputId()<0 && outs==1 && input.valid() && !input.isConnected());
  } else if(mode=="fragment-reference") {
    auto fragment=document.createDocumentFragment(),entry=document.createElement("span"),reference=document.createElement("div");
    fragment.appendChild(entry);other.appendChild(reference);bool refused=false;
    try {parent.insertBefore(fragment,reference);}catch(...){refused=true;}
    assert(refused && fragment.firstChildHandle().identity()==entry.identity() && !parent.firstChildHandle().valid());
  } else {assert(false && "unknown test mode");}
  tree.clear();puts("PASS actual native DOM mutation mode");
}
