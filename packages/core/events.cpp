// SPDX-License-Identifier: Apache-2.0
#include "events.h"

#include "ui/tree_internal.h"

#include <cstdio>

namespace gea::framework::events {

const char *EventTarget::getAttribute(const char *name) const
{
	if (!valid()) return "";
	return gea::embedded::ui::Tree::instance().getAttribute(nodeId_, name);
}

const char *EventTarget::dataset(const char *name) const
{
	if (!valid() || !name) return "";
	char attr[48];
	std::snprintf(attr, sizeof(attr), "data-%s", name);
	return getAttribute(attr);
}

int EventTarget::pressId() const
{
	if (!valid()) return -1;
	return gea::embedded::ui::Tree::instance().pressId(nodeId_);
}

int EventTarget::pressValue() const
{
	if (!valid()) return -1;
	return gea::embedded::ui::Tree::instance().pressValue(nodeId_);
}

const char *PointerEvent::typeName() const
{
	switch (type) {
	case PointerEventType::TouchStart:
		return "touchstart";
	case PointerEventType::TouchMove:
		return "touchmove";
	case PointerEventType::TouchEnd:
		return "touchend";
	case PointerEventType::Click:
		return "click";
	case PointerEventType::Input:
		return "input";
	case PointerEventType::KeyDown:
		return "keydown";
	case PointerEventType::Rotary:
		return "rotary";
	case PointerEventType::Scroll:
		return "scroll";
	}
	return "";
}

void PointerEvent::preventDefault()
{
	if (cancelable) defaultPrevented = true;
}

void PointerEvent::stopPropagation()
{
	propagationStopped = true;
}

}  // namespace gea::framework::events
