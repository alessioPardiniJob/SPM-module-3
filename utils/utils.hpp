#pragma once

#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <iostream>
#include <iomanip>

// ------------------------------------------------------------
// Timing accumulator per fasi
// ------------------------------------------------------------
struct PhaseTiming {
    struct PartitionPhase {
        double histogram = 0.0;
        double prefix = 0.0;
        double scatter = 0.0;
        double end = 0.0; 
        double teardown = 0.0; 
        double total = 0.0;
    };

    PartitionPhase R;
    PartitionPhase S;

    double build = 0.0;
    double probe = 0.0;
    double join_loop = 0.0;
    double teardown = 0.0;
    double total = 0.0;
};

// ------------------------------------------------------------
// Scoped timer RAII
// ------------------------------------------------------------
struct ScopedTimer {
    std::chrono::steady_clock::time_point start;
    double& acc;

    explicit ScopedTimer(double& acc_)
        : start(std::chrono::steady_clock::now()), acc(acc_) {}

    ~ScopedTimer() {
        auto end = std::chrono::steady_clock::now();
        acc += std::chrono::duration<double>(end - start).count();
    }
};

// ------------------------------------------------------------
// Statistiche base
// ------------------------------------------------------------
struct Stats {
    double mean;
    double median;
    double std;
};

inline Stats compute_stats(std::vector<double> v) {
    std::sort(v.begin(), v.end());

    const size_t n = v.size();
    if (n == 0) return {0, 0, 0};

    double sum = 0.0;
    for (double x : v) sum += x;

    double mean = sum / n;

    double median;
    if (n % 2 == 0)
        median = 0.5 * (v[n / 2 - 1] + v[n / 2]);
    else
        median = v[n / 2];

    double var = 0.0;
    for (double x : v) {
        double d = x - mean;
        var += d * d;
    }
    var /= n;

    return {mean, median, std::sqrt(var)};
}

static void print_stats(const std::string& name, const std::vector<double>& v) {
    Stats s = compute_stats(v);

    constexpr double MS = 1e3; // seconds -> milliseconds

    std::cout << std::left << std::setw(15) << name
              << " | median: " << std::setw(10) << (s.median * MS) << " ms"
              << " | mean: "   << std::setw(10) << (s.mean * MS)   << " ms"
              << " | std: "    << std::setw(10) << (s.std * MS)    << " ms"
              << "\n";
}

static void print_group(const std::string& name) {
    std::cout << "\n--- " << name << " ---\n";
}