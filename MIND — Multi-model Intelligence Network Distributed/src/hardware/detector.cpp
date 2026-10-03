#include "detector.h"
#include <cstdio>
#include <cstdlib>
#include <string>

bool HardwareDetector::simulation_enabled = false;

void HardwareDetector::override_simulation(bool enable) {
    simulation_enabled = enable;
}

bool HardwareDetector::is_simulation_enabled() {
    if (simulation_enabled) return true;
    const char * env = getenv("NEXUS_SIMULATE_HARDWARE");
    if (env && std::string(env) == "1") return true;
    return false;
}

MemoryTopology HardwareDetector::detect() {
    if (is_simulation_enabled()) {
        MemoryTopology topo;
        topo.architecture = "aarch64";
        topo.device_model = "Snapdragon X Elite Compute Platform";
        topo.unified_memory = true;
        topo.accelerator = "qualcomm";
        topo.accelerator_memory = 0; // UMA
        topo.system_memory = 32ULL * 1024 * 1024 * 1024; // 32 GB fake RAM
        topo.storage_capacity = 1000ULL * 1024 * 1024 * 1024; // 1 TB fake SSD
        topo.storage_free = 500ULL * 1024 * 1024 * 1024;
        topo.profile_name = "accelerator";
        
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "adreno";
        topo.compute.has_npu = true;
        topo.compute.npu_backend = "hexagon";
        topo.compute.cpu_backend = "arm_neon";
        
        return topo;
    }

#if defined(__APPLE__)
    return detect_macos_topology();
#elif defined(_WIN32)
    return detect_windows_topology();
#elif defined(__linux__)
    return detect_linux_topology();
#else
    MemoryTopology fallback;
    fallback.architecture = "unknown";
    fallback.profile_name = "cpu_only";
    return fallback;
#endif
}

void HardwareDetector::log_topology(const MemoryTopology & topo) {
    fprintf(stderr, "===============================================================\n");
    fprintf(stderr, " NEXUS Topology-Aware Hardware Detection:\n");
    fprintf(stderr, "   - Architecture    : %s\n", topo.architecture.c_str());
    fprintf(stderr, "   - Device Model    : %s\n", topo.device_model.c_str());
    fprintf(stderr, "   - Profile         : %s\n", topo.profile_name.c_str());
    fprintf(stderr, "   - Unified Memory  : %s\n", topo.unified_memory ? "YES (Zero-Copy UMA)" : "NO (Discrete)");
    fprintf(stderr, "   - Accelerator     : %s (VRAM: %.2f GB)\n",
            topo.accelerator.c_str(),
            (double)topo.accelerator_memory / (1024.0 * 1024.0 * 1024.0));
    fprintf(stderr, "   - System Memory   : %.2f GB\n",
            (double)topo.system_memory / (1024.0 * 1024.0 * 1024.0));
    fprintf(stderr, "   - Fast Storage    : %.2f GB total (%.2f GB free)\n",
            (double)topo.storage_capacity / (1024.0 * 1024.0 * 1024.0),
            (double)topo.storage_free / (1024.0 * 1024.0 * 1024.0));
    if (topo.compute.has_gpu || topo.compute.has_npu) {
        std::string npu_str = topo.compute.has_npu ? ", NPU (" + topo.compute.npu_backend + ")" : "";
        fprintf(stderr, "   - Compute Units   : CPU (%s)%s%s%s%s\n",
                topo.compute.cpu_backend.c_str(),
                topo.compute.has_gpu ? ", GPU (" : "",
                topo.compute.has_gpu ? topo.compute.gpu_backend.c_str() : "",
                topo.compute.has_gpu ? ")" : "",
                npu_str.c_str());
    }
    fprintf(stderr, "===============================================================\n");
}
