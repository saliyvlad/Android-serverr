#pragma once
#include <vector>
#include <string>
#include <mutex>
#include <cstdint>

enum class HeatmapCriterion { RSRP, RSRQ, RSSI, Altitude };

struct HeatmapConfig {
    bool enabled = false;
    HeatmapCriterion criterion = HeatmapCriterion::RSRP;
    float search_radius_meters = 20.0f;
    int target_earfcn = -1;
    int target_pci = -1;     
    float opacity = 0.6f;
    bool isGenerating = false;
};

struct HeatmapData {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    double minLat = 0, maxLat = 0;
    double minLon = 0, maxLon = 0;
    bool ready = false;
};

extern HeatmapConfig g_HeatmapConfig;
extern HeatmapData g_HeatmapData;
extern std::mutex g_HeatmapMutex;

void GenerateHeatmapWorker();