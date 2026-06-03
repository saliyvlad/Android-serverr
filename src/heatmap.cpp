#include "heatmap.h"
#include "data_structures.h"
#include "mercator.h"
#include <cmath>
#include <algorithm>
#include <thread>
#include <vector>
#include <iostream>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

HeatmapConfig g_HeatmapConfig;
HeatmapData g_HeatmapData;
std::mutex g_HeatmapMutex;

double HaversineDistance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dLat/2) * sin(dLat/2) + 
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) * 
               sin(dLon/2) * sin(dLon/2);
    return R * 2 * atan2(sqrt(a), sqrt(1-a));
}


double CalculateIDW(double queryLat, double queryLon, float radiusMeters, int filterEarfcn, int filterPci) {
    double latDegRadius = radiusMeters / 111000.0;
    double lonDegRadius = radiusMeters / (111000.0 * cos(queryLat * M_PI / 180.0));
    
    double sumNum = 0.0;
    double sumDen = 0.0;
    const double p = 2.0;

    for (size_t i = 0; i < offline_store.lats.size(); ++i) {
        
        if (filterEarfcn != -1 && offline_store.earfcns[i] != filterEarfcn) continue;
        
        
        if (filterPci != -1 && offline_store.pcis[i] != filterPci) continue;

        double dLat = std::abs(offline_store.lats[i] - queryLat);
        double dLon = std::abs(offline_store.lons[i] - queryLon);
        if (dLat > latDegRadius || dLon > lonDegRadius) continue;
        
        double dist = HaversineDistance(queryLat, queryLon, 
                                       offline_store.lats[i], 
                                       offline_store.lons[i]);
        
        if (dist <= radiusMeters && dist > 0.001) {
            double w = 1.0 / pow(dist, p);
            double value = offline_store.rsrps[i];
            sumNum += w * value;
            sumDen += w;
        } else if (dist < 0.001) {
            return offline_store.rsrps[i];
        }
    }

    return (sumDen == 0.0) ? NAN : sumNum / sumDen;
}

std::vector<unsigned char> ValueToColor(double value) {
    if (std::isnan(value)) return {0, 0, 0, 0};
    if (g_HeatmapConfig.criterion == HeatmapCriterion::RSRP && value < -110.0) {
        return {0, 0, 0, 0};
    }

    double t = 0.0;
    if (g_HeatmapConfig.criterion == HeatmapCriterion::RSRP) {
        t = std::clamp((-value - 80.0) / 30.0, 0.0, 1.0);
    } else {
        t = 0.5;
    }
    t = std::clamp(t, 0.0, 1.0);

    unsigned char r, g, b;
    if (t < 0.33) { r = 255; g = static_cast<unsigned char>(t * 3 * 255); b = 0; }
    else if (t < 0.66) { r = static_cast<unsigned char>(255 - (t - 0.33) * 3 * 255); g = 255; b = 0; }
    else { r = 0; g = static_cast<unsigned char>(255 - (t - 0.66) * 3 * 255); b = 255; }

    unsigned char alpha = static_cast<unsigned char>(g_HeatmapConfig.opacity * 255);
    return {r, g, b, alpha};
}

void GenerateHeatmapWorker() {
    g_HeatmapConfig.isGenerating = true;
    std::cout << "[Heatmap] Starting PARALLEL generation..." << std::endl;
    
    if (offline_store.lats.empty()) {
        std::cerr << "[Heatmap] ERROR: No data!" << std::endl;
        g_HeatmapConfig.isGenerating = false;
        return;
    }

    double minLat = offline_store.lats[0], maxLat = offline_store.lats[0];
    double minLon = offline_store.lons[0], maxLon = offline_store.lons[0];
    
    for (size_t i = 0; i < offline_store.lats.size(); ++i) {
        minLat = std::min(minLat, offline_store.lats[i]);
        maxLat = std::max(maxLat, offline_store.lats[i]);
        minLon = std::min(minLon, offline_store.lons[i]);
        maxLon = std::max(maxLon, offline_store.lons[i]);
    }
    
    double padding = 0.0005;
    minLat -= padding; maxLat += padding;
    minLon -= padding; maxLon += padding;

    const int W = 512;
    const int H = 512;
    std::vector<uint8_t> image(W * H * 4, 0);

    unsigned int numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;
    numThreads = std::min(numThreads, static_cast<unsigned int>(H));
    
    std::cout << "[Heatmap] Using " << numThreads << " threads." << std::endl;

    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    auto processRows = [&](int startRow, int endRow, int threadId) {
        for (int y = startRow; y < endRow; ++y) {
            if (threadId == 0 && y % 64 == 0) {
                std::cout << "[Heatmap] Progress: " << (y * 100 / H) << "%" << std::endl;
            }
            
            for (int x = 0; x < W; ++x) {
                double lat = maxLat - (maxLat - minLat) * y / H;
                double lon = minLon + (maxLon - minLon) * x / W;

                
                double val = CalculateIDW(lat, lon, 
                                         g_HeatmapConfig.search_radius_meters, 
                                         g_HeatmapConfig.target_earfcn,
                                         g_HeatmapConfig.target_pci); 
                
                auto col = ValueToColor(val);

                int idx = (y * W + x) * 4;
                image[idx + 0] = col[0];
                image[idx + 1] = col[1];
                image[idx + 2] = col[2];
                image[idx + 3] = col[3];
            }
        }
    };

    int rowsPerThread = H / numThreads;
    for (unsigned int t = 0; t < numThreads; ++t) {
        int startRow = t * rowsPerThread;
        int endRow = (t == numThreads - 1) ? H : startRow + rowsPerThread;
        threads.emplace_back(processRows, startRow, endRow, t);
    }

    for (auto& thread : threads) {
        thread.join();
    }

    std::cout << "[Heatmap] Calculation complete." << std::endl;

    try {
        std::filesystem::create_directories("build");
        std::string path = "build/heatmap.png";
        stbi_write_png(path.c_str(), W, H, 4, image.data(), W * 4);
    } catch (const std::exception& e) {
        std::cerr << "[Heatmap] Error saving: " << e.what() << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(g_HeatmapMutex);
        g_HeatmapData.pixels = std::move(image);
        g_HeatmapData.width = W;
        g_HeatmapData.height = H;
        g_HeatmapData.minLat = minLat;
        g_HeatmapData.maxLat = maxLat;
        g_HeatmapData.minLon = minLon;
        g_HeatmapData.maxLon = maxLon;
        g_HeatmapData.ready = true;
    }

    std::cout << "[Heatmap] ✓ Data ready for GPU upload." << std::endl;
    g_HeatmapConfig.isGenerating = false;
}