// m11q display-only ordering. Partition flags and wipe implementations stay intact.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

inline unsigned M11qWipeRank(const std::string& point) {
    const char* const order[] = {"DALVIK", "/cache", "/data", "INTERNAL", "/external_sd", "/usb-otg"};
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
        if (point == order[i]) return i;
    return sizeof(order) / sizeof(order[0]);
}

template <typename Entry>
void M11qOrderWipeList(std::vector<Entry>& entries) {
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.Mount_Point == "/metadata";
    }), entries.end());
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return M11qWipeRank(a.Mount_Point) < M11qWipeRank(b.Mount_Point);
    });
}
