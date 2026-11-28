// SPDX-License-Identifier: Apache-2.0
#include <string>

#include "geaos.h"
#include "gea/embedded-host.h"

namespace gea::framework::host {

class AppHost {
 public:
  static double launch(const std::string &app_id) {
    return gea::geaos::Launcher::launch(app_id) ? 1.0 : 0.0;
  }
};

}  // namespace gea::framework::host

double gea::host::Apps::launch(const std::string &app_id) const {
  return gea::framework::host::AppHost::launch(app_id);
}
