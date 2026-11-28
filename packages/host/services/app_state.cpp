// SPDX-License-Identifier: Apache-2.0
#include "services/app_state.h"

#include "geaos.h"
#include "services/mirror.h"

namespace gea::framework::services {

const char *AppState::currentAppId()
{
	return gea::geaos::Launcher::currentId();
}

void AppState::afterAppInit()
{
	MirrorService::clearDirty();
}

}  // namespace gea::framework::services
