#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Allocatore ArduinoJson che preferisce la PSRAM (8MB sulla T-ETH-Lite ESP32S3):
// lo stato completo di un impianto con decine di canali non deve mai competere
// con la RAM interna usata da lwIP e dal web server.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  void deallocate(void *pointer) override { heap_caps_free(pointer); }

  void *reallocate(void *pointer, size_t size) override {
    return heap_caps_realloc_prefer(pointer, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }

  static SpiRamAllocator &instance() {
    static SpiRamAllocator allocator;
    return allocator;
  }
};

inline JsonDocument makeJsonDocument() { return JsonDocument(&SpiRamAllocator::instance()); }
