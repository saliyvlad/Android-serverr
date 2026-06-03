#define _USE_MATH_DEFINES
#include <cmath>
#include "gui.h"
#include "data_structures.h"
#include "tile_cache.h"
#include "mercator.h"
#include "heatmap.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "imgui.h"
#include "implot.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include <iostream>
#include <fstream>
#include <string>
#include <mutex>
#include <algorithm>
#include <thread>
#include <set>
#include <vector>

extern bool running;
extern bool start_server;
extern bool get_location;
extern bool get_network;
extern int zoom;
extern std::mutex g_CacheMutex;
extern std::mutex g_JobMutex;

static GLuint g_LegendTexture = 0;
static bool g_LegendGenerated = false;
static GLuint g_HeatmapTextureId = 0;
static bool g_HeatmapUploaded = false;

void ColoredIndicator(const char* label, bool condition,
                      const char* true_text, const char* false_text) {
    ImGui::Text("%s: ", label);
    ImGui::SameLine();
    if (condition) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 255, 0, 255));
        ImGui::Text("* %s", true_text);
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
        ImGui::Text("* %s", false_text);
        ImGui::PopStyleColor();
    }
}


void EnsureLegendTexture() {
    if (g_LegendGenerated) return;

    const int W = 100;
    const int H = 20;
    std::vector<unsigned char> data(W * H * 4);

    for (int x = 0; x < W; ++x) {
        double t = x / static_cast<double>(W - 1); 
        
        
        unsigned char r, g, b;
        if (t < 0.33) {
            r = 255; g = static_cast<unsigned char>(t * 3 * 255); b = 0;
        } else if (t < 0.66) {
            r = static_cast<unsigned char>(255 - (t - 0.33) * 3 * 255); g = 255; b = 0;
        } else {
            r = 0; g = static_cast<unsigned char>(255 - (t - 0.66) * 3 * 255); b = 255;
        }

        for (int y = 0; y < H; ++y) {
            int idx = (y * W + x) * 4;
            data[idx + 0] = r;
            data[idx + 1] = g;
            data[idx + 2] = b;
            data[idx + 3] = 255; 
        }
    }

    glGenTextures(1, &g_LegendTexture);
    glBindTexture(GL_TEXTURE_2D, g_LegendTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, 
                 GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    
    g_LegendGenerated = true;
}


void LoadLogFile() {
    std::lock_guard<std::mutex> lock(mtx);
    offline_store.clear();
    std::ifstream file("log.json");
    if (!file.is_open()) {
        std::cerr << "[Error] Could not open log.json" << std::endl;
        return;
    }
    std::string line;
    int line_count = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            
            double lat = j.value("lat", 0.0);
            double lon = j.value("lon", 0.0);
            
            
            if (lat == 0.0 && lon == 0.0) continue;

            offline_store.lats.push_back(lat);
            offline_store.lons.push_back(lon);
            offline_store.times.push_back(j.value("time", 0.0));
            offline_store.indices.push_back(line_count);

            
            double rsrp = -145.0;
            int pci = -1;
            int earfcn = 0;

            if (j.contains("cell_data") && j["cell_data"].contains("cells") && 
                j["cell_data"]["cells"].is_array() && !j["cell_data"]["cells"].empty()) {
                
                const auto& cell = j["cell_data"]["cells"][0];
                
                
                if (cell.contains("signal") && cell["signal"].contains("rsrp")) {
                    rsrp = cell["signal"]["rsrp"].get<double>();
                }
                
                
                if (cell.contains("identity") && cell["identity"].contains("pci")) {
                    auto& pci_val = cell["identity"]["pci"];
                    if (pci_val.is_string()) {
                        try { 
                            pci = std::stoi(pci_val.get<std::string>()); 
                        } catch(...) { 
                            pci = -1; 
                        }
                    } else {
                        pci = pci_val.get<int>();
                    }
                }
            }
            
            offline_store.rsrps.push_back(rsrp);
            offline_store.pcis.push_back(pci);
            offline_store.earfcns.push_back(earfcn);

            line_count++;
        } catch (const std::exception& e) {
            
        }
    }
    offline_store.loaded = true;
    std::cout << "[Info] Loaded " << line_count << " records." << std::endl;
    
    
    g_LegendGenerated = false;
    g_HeatmapUploaded = false;
    if (g_LegendTexture != 0) {
        glDeleteTextures(1, &g_LegendTexture);
        g_LegendTexture = 0;
    }
    
    file.close();
}


void RunGUI() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return;
    SDL_Window* window = SDL_CreateWindow("Network Analyzer", SDL_WINDOWPOS_CENTERED,
    SDL_WINDOWPOS_CENTERED, 1100, 700,
    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    glewInit();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 330");

    
    g_HeatmapTextureId = 0;
    g_HeatmapUploaded = false;
    g_LegendGenerated = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        
        ImGui::Begin("Heatmap Settings");
        ImGui::Checkbox("Enable Heatmap", &g_HeatmapConfig.enabled);
        
        if (g_HeatmapConfig.enabled) {
            ImGui::SliderFloat("Radius (m)", &g_HeatmapConfig.search_radius_meters, 10.0f, 40.0f);
            ImGui::SliderFloat("Opacity", &g_HeatmapConfig.opacity, 0.1f, 1.0f);
            
            
            if (offline_store.loaded && !offline_store.pcis.empty()) {
                
                static std::vector<int> unique_pcis;
                static std::vector<std::string> labels_storage;
                static bool needs_update = true;

                if (needs_update) {
                    unique_pcis.clear();
                    labels_storage.clear();
                    std::set<int> seen;
                    
                    for (int p : offline_store.pcis) {
                        if (p != -1 && seen.find(p) == seen.end()) {
                            seen.insert(p);
                            unique_pcis.push_back(p);
                        }
                    }
                    std::sort(unique_pcis.begin(), unique_pcis.end());
                    
                    
                    labels_storage.push_back("All");
                    for (int p : unique_pcis) {
                        labels_storage.push_back("PCI: " + std::to_string(p));
                    }
                    needs_update = false;
                }

                
                std::vector<const char*> items;
                for (auto& s : labels_storage) {
                    items.push_back(s.c_str());
                }

                
                int current_idx = 0;
                if (g_HeatmapConfig.target_pci == -1) {
                    current_idx = 0;
                } else {
                    auto it = std::find(unique_pcis.begin(), unique_pcis.end(), g_HeatmapConfig.target_pci);
                    if (it != unique_pcis.end()) {
                        current_idx = static_cast<int>(it - unique_pcis.begin()) + 1;
                    }
                }

                if (ImGui::Combo("Filter by PCI", &current_idx, items.data(), static_cast<int>(items.size()))) {
                    if (current_idx == 0) {
                        g_HeatmapConfig.target_pci = -1; // All
                    } else {
                        g_HeatmapConfig.target_pci = unique_pcis[current_idx - 1];
                    }
                    
                    g_HeatmapData.ready = false;
                    g_HeatmapUploaded = false;
                }
            }

            if (!g_HeatmapConfig.isGenerating) {
                if (ImGui::Button("Generate Heatmap")) {
                    std::thread t(GenerateHeatmapWorker);
                    t.detach();
                }
            } else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Generating...");
            }
        }
        ImGui::End();

        
        ImGui::Begin("Current stats");
        ImGui::Text("Lat: %s", data_store.lat.c_str());
        ImGui::Text("Lon: %s", data_store.lon.c_str());
        ImGui::Text("RSRP: %.1f dBm", data_store.current_rsrp);
        ImGui::End();

        ImGui::Begin("Log Control");
        if (ImGui::Button("Load log.json", ImVec2(-1, 40))) {
            LoadLogFile();
        }
        if (offline_store.loaded) {
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Total: %d records", 
                              static_cast<int>(offline_store.lats.size()));
        }
        ImGui::End();

        
        ImGui::Begin("Movement Route");
        
        
        EnsureLegendTexture();
        if (g_LegendGenerated && g_LegendTexture != 0) {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImVec2 size = ImGui::GetWindowSize();
            ImVec2 legendPos = ImVec2(pos.x + size.x - 120, pos.y + 10);
            
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddImage((ImTextureID)(uintptr_t)g_LegendTexture, 
                                legendPos, 
                                ImVec2(legendPos.x + 100, legendPos.y + 20));
            
                                
            draw_list->AddText(ImVec2(legendPos.x, legendPos.y + 22), 
                              IM_COL32(255, 255, 255, 255), "-80");
            draw_list->AddText(ImVec2(legendPos.x + 40, legendPos.y + 22), 
                              IM_COL32(255, 255, 255, 255), "-95");
            draw_list->AddText(ImVec2(legendPos.x + 80, legendPos.y + 22), 
                              IM_COL32(255, 255, 255, 255), "-110");
            
            draw_list->AddText(ImVec2(legendPos.x, legendPos.y - 15), 
                              IM_COL32(255, 255, 255, 255), "RSRP Legend (dBm)");
        }

        if (offline_store.loaded && !offline_store.lats.empty()) {
            if (ImPlot::BeginPlot("Map", ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Longitude", "Latitude");
                ImPlot::SetupAxisLimits(ImAxis_X1, 82.900, 82.960, ImGuiCond_Once);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 55.000, 55.040, ImGuiCond_Once);
                
                ImPlotRect limits = ImPlot::GetPlotLimits();
                
                if (limits.X.Max > limits.X.Min && limits.Y.Max > limits.Y.Min) {
                    double mercator_left = limits.X.Min;
                    double mercator_right = limits.X.Max;
                    double mercator_bottom = LatToMercatorY(limits.Y.Min);
                    double mercator_top = LatToMercatorY(limits.Y.Max);
                    zoom = CalculateZoom(mercator_left, mercator_right);
                    
                    int minX = static_cast<int>(floor(MercatorXToTileX(mercator_left, zoom)));
                    int minY = static_cast<int>(floor(MercatorYToTileY(mercator_top, zoom)));
                    int maxX = static_cast<int>(floor(MercatorXToTileX(mercator_right, zoom)));
                    int maxY = static_cast<int>(floor(MercatorYToTileY(mercator_bottom, zoom)));
                    int maxTileCount = (1 << zoom) - 1;
                    
                    minX = std::max(0, std::min(minX, maxTileCount));
                    maxX = std::max(0, std::min(maxX, maxTileCount));
                    minY = std::max(0, std::min(minY, maxTileCount));
                    maxY = std::max(0, std::min(maxY, maxTileCount));

                    
                    for (int x = minX; x <= maxX; ++x) {
                        for (int y = minY; y <= maxY; ++y) {
                            std::string tileId = std::to_string(zoom) + "/" + 
                                                std::to_string(x) + "/" + 
                                                std::to_string(y);
                            bool needLoad = false;
                            {
                                std::lock_guard<std::mutex> lock(g_CacheMutex);
                                if (g_TileCache.find(tileId) == g_TileCache.end()) {
                                    TextureData newTex;
                                    newTex.isLoading = true;
                                    g_TileCache[tileId] = newTex;
                                    needLoad = true;
                                }
                            }
                            if (needLoad) {
                                std::lock_guard<std::mutex> lock(g_JobMutex);
                                TileJob job{tileId, zoom, x, y};
                                g_JobQueue.push(job);
                            }
                        }
                    }

                    
                    for (int x = minX; x <= maxX; ++x) {
                        for (int y = minY; y <= maxY; ++y) {
                            std::string tileId = std::to_string(zoom) + "/" + 
                                                std::to_string(x) + "/" + 
                                                std::to_string(y);
                            GLuint gpuId = 0;
                            {
                                std::lock_guard<std::mutex> lock(g_CacheMutex);
                                auto it = g_TileCache.find(tileId);
                                if (it != g_TileCache.end()) {
                                    auto& tex = it->second;
                                    if (!tex.rgbaBlob.empty() && tex.id == 0) {
                                        glGenTextures(1, &tex.id);
                                        glBindTexture(GL_TEXTURE_2D, tex.id);
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 
                                                     tex.width, tex.height, 0,
                                                     GL_RGBA, GL_UNSIGNED_BYTE, 
                                                     tex.rgbaBlob.data());
                                        tex.rgbaBlob.clear();
                                    }
                                    gpuId = tex.id;
                                }
                            }
                            if (gpuId != 0) {
                                double left = TileXToMercatorX(x, zoom);
                                double right = TileXToMercatorX(x + 1, zoom);
                                double top_lat = MercatorYToLat(TileYToMercatorY(y, zoom));
                                double bottom_lat = MercatorYToLat(TileYToMercatorY(y + 1, zoom));
                                ImPlotPoint minPoint{left, bottom_lat};
                                ImPlotPoint maxPoint{right, top_lat};
                                
                                ImPlot::PlotImage(("##tile_" + tileId).c_str(),
                                                  (ImTextureID)(uintptr_t)gpuId, 
                                                  minPoint, maxPoint);
                            }
                        }
                    }

                    
                    if (g_HeatmapData.ready && !g_HeatmapUploaded) {
                        if (g_HeatmapTextureId != 0) {
                            glDeleteTextures(1, &g_HeatmapTextureId);
                        }
                        glGenTextures(1, &g_HeatmapTextureId);
                        glBindTexture(GL_TEXTURE_2D, g_HeatmapTextureId);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 
                                     g_HeatmapData.width, g_HeatmapData.height, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, 
                                     g_HeatmapData.pixels.data());
                        g_HeatmapUploaded = true;
                        std::cout << "[GUI] ✓ Heatmap texture uploaded to GPU!" << std::endl;
                    }

                    
                    if (g_HeatmapConfig.enabled && g_HeatmapData.ready && 
                        g_HeatmapUploaded && g_HeatmapTextureId != 0) {
                        ImPlotPoint b_min{g_HeatmapData.minLon, g_HeatmapData.minLat};
                        ImPlotPoint b_max{g_HeatmapData.maxLon, g_HeatmapData.maxLat};
                        
                        ImPlot::PlotImage("##heatmap_overlay",
                                          (ImTextureID)(uintptr_t)g_HeatmapTextureId,
                                          b_min, b_max);
                    }

                    
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.0f);
                    ImPlot::PlotScatter("route", offline_store.lons.data(),
                                       offline_store.lats.data(), 
                                       static_cast<int>(offline_store.lats.size()));
                    ImPlot::EndPlot();
                }
            }
        } else {
            ImGui::Text("Load data first.");
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, 1100, 700);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    
    if (g_HeatmapTextureId != 0) {
        glDeleteTextures(1, &g_HeatmapTextureId);
    }
    if (g_LegendTexture != 0) {
        glDeleteTextures(1, &g_LegendTexture);
    }
    
    ImPlot::DestroyContext();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_Quit();
}