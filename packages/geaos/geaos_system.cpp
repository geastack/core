// SPDX-License-Identifier: Apache-2.0
// GeaOS system actions that reach the runtime's event loop.
#include "geaos.h"

#include "event.h"
#include "services/frame_scheduler.h"

namespace gea::geaos {

bool queueSettingsToggle()
{
	gea::framework::events::Event event{};
	event.type = gea::framework::events::EventType::SettingsToggle;
	return gea::framework::services::FrameScheduler::sendEvent(event);
}

}  // namespace gea::geaos
