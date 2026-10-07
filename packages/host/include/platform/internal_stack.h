// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>

#ifdef ESP_PLATFORM
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace gea::platform {

// Flash-backed VFS and radio configuration can disable caches. Run these
// operations on an internal stack when a compiled application uses PSRAM.
inline void onInternalStack(const std::function<void()> &action, unsigned stackBytes = 6144) {
#ifdef ESP_PLATFORM
  int marker;
  if (esp_ptr_external_ram(&marker)) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);

    struct Worker {
      SemaphoreHandle_t ready = nullptr;
      SemaphoreHandle_t done = nullptr;
      const std::function<void()> *action = nullptr;
      std::exception_ptr error;
      TaskHandle_t task = nullptr;
    };

    static Worker worker;
    if (stackBytes > 10240) {
      throw std::runtime_error("Internal-stack action exceeds worker capacity");
    }
    if (!worker.task) {
      worker.ready = xSemaphoreCreateBinary();
      worker.done = xSemaphoreCreateBinary();
      if (!worker.ready || !worker.done) {
        if (worker.ready) {
          vSemaphoreDelete(worker.ready);
        }
        if (worker.done) {
          vSemaphoreDelete(worker.done);
        }
        worker.ready = worker.done = nullptr;
        throw std::runtime_error("Internal-stack semaphore allocation failed");
      }
      const auto run = [](void *argument) {
        auto &worker = *static_cast<Worker *>(argument);
        for (;;) {
          xSemaphoreTake(worker.ready, portMAX_DELAY);
          try {
            (*worker.action)();
          } catch (...) {
            worker.error = std::current_exception();
          }
          worker.action = nullptr;
          xSemaphoreGive(worker.done);
        }
      };
      if (xTaskCreateWithCaps(run, "gea_internal", 10240, &worker, 5, &worker.task,
                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(worker.ready);
        vSemaphoreDelete(worker.done);
        worker.ready = worker.done = nullptr;
        throw std::runtime_error("Internal-stack worker allocation failed");
      }
    }
    worker.error = {};
    worker.action = &action;
    xSemaphoreGive(worker.ready);
    xSemaphoreTake(worker.done, portMAX_DELAY);
    if (worker.error) {
      std::rethrow_exception(worker.error);
    }
    return;
  }
#else
  (void)stackBytes;
#endif
  action();
}

} // namespace gea::platform
