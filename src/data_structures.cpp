#include "data_structures.h"


OfflineData offline_store;
Telemetry data_store;
std::mutex mtx;
std::vector<std::string> log_messages;
int session_data_counter = 0;



void SignalHistory::add_points(const json& cells_array) {
    if (x.size() >= max_points) {
        x.erase(x.begin());
        for (auto& [pci, vec] : streams_y) {
            if (!vec.empty()) vec.erase(vec.begin());
        }
    }
    x.push_back(current_step++);

    for (const auto& cell : cells_array) {
        if (cell.contains("identity") && cell["identity"].contains("pci")) {
            int pci = cell["identity"]["pci"].get<int>();
            double rsrp = -145.0;
            if (cell.contains("signal") && cell["signal"].contains("rsrp")) {
                rsrp = cell["signal"]["rsrp"].get<double>();
            }
            
            if (streams_y.find(pci) == streams_y.end()) {
                streams_y[pci] = std::vector<double>(x.size() - 1, -145.0);
                streams_y[pci].push_back(rsrp);
            } else {
                streams_y[pci].push_back(rsrp);
            }
        }
    }
}