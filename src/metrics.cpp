#include "metrics.h"

#include <esp_heap_caps.h>
#include <esp_system.h>

namespace metrics {

namespace {

constexpr uint32_t WINDOW_MS = 1000;
constexpr size_t HISTORY = 60;  // un minuto di finestre da un secondo

uint32_t g_windowStartedAt = 0;
uint32_t g_windowBusyUs = 0;
uint32_t g_windowIterations = 0;
uint32_t g_windowMaxUs = 0;

float g_usage = 0.0f;
float g_history[HISTORY] = {0};
size_t g_historyCount = 0;
size_t g_historyIndex = 0;
float g_usagePeak = 0.0f;
uint32_t g_loopHz = 0;
uint32_t g_loopAvgUs = 0;
uint32_t g_loopMaxUs = 0;
uint32_t g_loopMaxUsEver = 0;

float historyAverage() {
  if (!g_historyCount) return g_usage;
  float total = 0.0f;
  for (size_t i = 0; i < g_historyCount; i++) total += g_history[i];
  return total / g_historyCount;
}

}  // namespace

void sample(uint32_t busyMicros) {
  const uint32_t now = millis();
  if (g_windowStartedAt == 0) g_windowStartedAt = now;

  g_windowBusyUs += busyMicros;
  g_windowIterations++;
  if (busyMicros > g_windowMaxUs) g_windowMaxUs = busyMicros;
  if (busyMicros > g_loopMaxUsEver) g_loopMaxUsEver = busyMicros;

  const uint32_t elapsed = now - g_windowStartedAt;
  if (elapsed < WINDOW_MS) return;

  const float usage = constrain((g_windowBusyUs / 1000.0f) / elapsed * 100.0f, 0.0f, 100.0f);
  g_usage = usage;
  if (usage > g_usagePeak) g_usagePeak = usage;
  g_loopHz = static_cast<uint32_t>((g_windowIterations * 1000UL) / elapsed);
  g_loopAvgUs = g_windowIterations ? g_windowBusyUs / g_windowIterations : 0;
  g_loopMaxUs = g_windowMaxUs;

  g_history[g_historyIndex] = usage;
  g_historyIndex = (g_historyIndex + 1) % HISTORY;
  if (g_historyCount < HISTORY) g_historyCount++;

  g_windowStartedAt = now;
  g_windowBusyUs = 0;
  g_windowIterations = 0;
  g_windowMaxUs = 0;
}

void statusJson(JsonObject out) {
  out["cpuFreqMhz"] = getCpuFrequencyMhz();
  out["cores"] = 2;
  out["loopUsagePercent"] = roundf(g_usage * 10.0f) / 10.0f;
  out["loopUsageAvgPercent"] = roundf(historyAverage() * 10.0f) / 10.0f;
  out["loopUsagePeakPercent"] = roundf(g_usagePeak * 10.0f) / 10.0f;
  out["loopHz"] = g_loopHz;
  out["loopAvgUs"] = g_loopAvgUs;
  out["loopMaxUs"] = g_loopMaxUs;
  out["loopMaxUsEver"] = g_loopMaxUsEver;
  out["taskStackFree"] = uxTaskGetStackHighWaterMark(nullptr);
  out["minFreeHeap"] = esp_get_minimum_free_heap_size();
  out["heapLargestBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  JsonArray history = out["history"].to<JsonArray>();
  for (size_t i = 0; i < g_historyCount; i++) {
    const size_t index = (g_historyIndex + HISTORY - g_historyCount + i) % HISTORY;
    history.add(roundf(g_history[index] * 10.0f) / 10.0f);
  }
}

}  // namespace metrics
