#include "tile_cache.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <curl/curl.h>
#include <thread>
#include <chrono>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace fs = std::filesystem;

std::map<std::string, TextureData> g_TileCache;
std::queue<TileJob> g_JobQueue;
std::mutex g_JobMutex;
std::mutex g_CacheMutex;

std::string GetTilePath(int zoom, int x, int y) {
    std::stringstream ss;
    ss << "tile_cache/" << zoom << "/" << x;
    fs::create_directories(ss.str());
    ss << "/" << y << ".png";
    return ss.str();
}

bool LoadTileFromDisk(int zoom, int x, int y, std::vector<uint8_t>& out_data) {
    std::string path = GetTilePath(zoom, x, y);
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);
    out_data.resize(size);
    file.read(reinterpret_cast<char*>(out_data.data()), size);
    return file.good();
}

void SaveTileToDisk(int zoom, int x, int y, const std::vector<uint8_t>& data) {
    std::string path = GetTilePath(zoom, x, y);
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<const char*>(data.data()), data.size());
        file.close();
    }
}

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    auto& buffer = *static_cast<std::vector<uint8_t>*>(userp);
    const auto* dataptr = static_cast<uint8_t*>(contents);
    buffer.insert(buffer.end(), dataptr, dataptr + realsize);
    return realsize;
}

void FetchWorker() {
    
    curl_global_init(CURL_GLOBAL_ALL);
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[TileCache] Failed to init CURL!" << std::endl;
        return;
    }

    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: NetworkAnalyzer/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    std::cout << "[TileCache] FetchWorker started." << std::endl;

    while (true) {
        TileJob job;
        bool foundJob = false;

        {
            std::lock_guard<std::mutex> lock(g_JobMutex);
            if (!g_JobQueue.empty()) {
                job = g_JobQueue.front();
                g_JobQueue.pop();
                foundJob = true;
            }
        }

        if (!foundJob) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::vector<uint8_t> rawBlob;
        bool loaded = false;

        
        if (LoadTileFromDisk(job.zoom, job.x, job.y, rawBlob)) {
            std::cout << "[Cache] Loaded from disk: " << job.id << std::endl;
            loaded = true;
        } else {
            
            std::string url = "https://a.tile.openstreetmap.org/" +
                              std::to_string(job.zoom) + "/" +
                              std::to_string(job.x) + "/" +
                              std::to_string(job.y) + ".png";
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &rawBlob);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                std::cout << "[Download] Loaded from web: " << job.id << std::endl;
                SaveTileToDisk(job.zoom, job.x, job.y, rawBlob);
                loaded = true;
            } else {
                std::cerr << "[Error] Failed to load tile: " << job.id 
                          << " | CURL: " << curl_easy_strerror(res) << std::endl;
            }
        }

        
        {
            std::lock_guard<std::mutex> lock(g_CacheMutex);
            if (loaded && !rawBlob.empty()) {
                int w, h, ch;
                unsigned char* data = stbi_load_from_memory(rawBlob.data(), (int)rawBlob.size(), &w, &h, &ch, STBI_rgb_alpha);
                if (data) {
                    auto& tex = g_TileCache[job.id];
                    tex.rgbaBlob.assign(data, data + (w * h * 4));
                    tex.width = w;
                    tex.height = h;
                    tex.isLoading = false;
                    tex.isLoaded = true;
                    stbi_image_free(data);
                } else {
                    g_TileCache[job.id].isLoading = false;
                }
            } else {
                g_TileCache[job.id].isLoading = false;
            }
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
}