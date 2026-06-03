#pragma once
#include <vector>
#include <string>
#include <mutex>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;


struct SignalHistory {
    std::vector<int> x;
    std::map<int, std::vector<double>> streams_y;
    int current_step = 0;
    const int max_points = 100;
    void add_points(const json& cells_array);
};


struct OfflineData {
    std::vector<double> lats;
    std::vector<double> lons;
    std::vector<double> rsrps;
    std::vector<int> pcis;       
    std::vector<int> earfcns;    
    std::vector<double> altitudes;
    
    std::vector<double> times;
    std::vector<double> indices;
    bool loaded = false;
    
    
    void clear() {
        lats.clear(); lons.clear(); rsrps.clear();
        pcis.clear(); earfcns.clear(); altitudes.clear();
        times.clear(); indices.clear();
        loaded = false;
    }
};


struct Telemetry {
    std::string lat = "0", lon = "0", alt = "0", acc = "0", type = "N/A";
    float current_rsrp = -140.0f;
    SignalHistory history;
    std::vector<std::string> pending_records;
};


extern OfflineData offline_store;
extern Telemetry data_store;
extern std::mutex mtx;
extern std::vector<std::string> log_messages;
extern int session_data_counter;