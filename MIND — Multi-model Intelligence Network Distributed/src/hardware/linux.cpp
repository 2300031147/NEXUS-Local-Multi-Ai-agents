#include "detector.h"
#include "hardware_profile.h"

#if defined(__linux__)

#include <unistd.h>
#include <sys/statvfs.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

static std::string read_first_line(const std::string & path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = line.find_last_not_of(" \t\r\n");
    return line.substr(first, last - first + 1);
}

MemoryTopology detect_linux_topology() {
    MemoryTopology topo;

#if defined(__aarch64__)
    topo.architecture = "aarch64";
#elif defined(__x86_64__)
    topo.architecture = "x86_64";
#elif defined(__arm__)
    topo.architecture = "arm";
#elif defined(__riscv)
    topo.architecture = "riscv";
#else
    topo.architecture = "unknown";
#endif

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        topo.host_page_size = (size_t)page_size;
    }

    // 1. Inspect /proc/meminfo for System RAM
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.compare(0, 9, "MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                size_t kb = 0;
                if (iss >> kb) topo.system_memory = kb * 1024;
                break;
            }
        }
    }

    // 2. Check for SoC identification (/sys/devices/soc0 or /proc/cpuinfo)
    std::string soc_family = read_first_line("/sys/devices/soc0/family");
    std::string soc_machine = read_first_line("/sys/devices/soc0/machine");
    std::string soc_soc_id = read_first_line("/sys/devices/soc0/soc_id");

    bool is_qualcomm_snapdragon = false;
    bool is_apple_silicon = false;
    bool is_tegra = false;

    if (!soc_family.empty() || !soc_machine.empty()) {
        std::string combined = soc_family + " " + soc_machine + " " + soc_soc_id;
        std::transform(combined.begin(), combined.end(), combined.begin(), [](unsigned char c) { return std::tolower(c); });
        if (combined.find("snapdragon") != std::string::npos ||
            combined.find("qualcomm") != std::string::npos ||
            combined.find("sm8") != std::string::npos ||
            combined.find("sc8") != std::string::npos ||
            combined.find("x elite") != std::string::npos) {
            is_qualcomm_snapdragon = true;
            topo.device_model = soc_machine.empty() ? "Qualcomm Snapdragon" : soc_machine;
        } else if (combined.find("tegra") != std::string::npos || combined.find("orin") != std::string::npos) {
            is_tegra = true;
            topo.device_model = soc_machine.empty() ? "NVIDIA Jetson / Tegra" : soc_machine;
        } else if (combined.find("apple") != std::string::npos) {
            is_apple_silicon = true;
            topo.device_model = soc_machine.empty() ? "Apple Silicon (Asahi Linux)" : soc_machine;
        }
    }

    // Also inspect /proc/cpuinfo
    if (!is_qualcomm_snapdragon && !is_apple_silicon && !is_tegra) {
        std::ifstream cpuinfo("/proc/cpuinfo");
        if (cpuinfo.is_open()) {
            std::string line;
            while (std::getline(cpuinfo, line)) {
                std::string lower = line;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                if (lower.find("qualcomm") != std::string::npos || lower.find("snapdragon") != std::string::npos) {
                    is_qualcomm_snapdragon = true;
                    topo.device_model = "Qualcomm Snapdragon";
                    break;
                } else if (lower.find("apple") != std::string::npos) {
                    is_apple_silicon = true;
                    topo.device_model = "Apple Silicon";
                    break;
                } else if (lower.find("tegra") != std::string::npos) {
                    is_tegra = true;
                    topo.device_model = "NVIDIA Tegra";
                    break;
                }
            }
        }
    }

    // Check DMI product/board info for Snapdragon laptops (e.g. Lenovo ThinkPad T14s, HP OmniBook X)
    if (!is_qualcomm_snapdragon && !is_apple_silicon && !is_tegra) {
        std::string dmi_product = read_first_line("/sys/class/dmi/id/product_name");
        std::string dmi_board = read_first_line("/sys/class/dmi/id/board_name");
        std::string dmi_sys = read_first_line("/sys/class/dmi/id/sys_vendor");
        std::string combined_dmi = dmi_sys + " " + dmi_product + " " + dmi_board;
        std::transform(combined_dmi.begin(), combined_dmi.end(), combined_dmi.begin(), [](unsigned char c) { return std::tolower(c); });
        if (combined_dmi.find("snapdragon") != std::string::npos ||
            combined_dmi.find("x elite") != std::string::npos ||
            combined_dmi.find("x plus") != std::string::npos ||
            combined_dmi.find("qualcomm") != std::string::npos) {
            is_qualcomm_snapdragon = true;
            topo.device_model = dmi_product.empty() ? "Qualcomm Snapdragon (DMI)" : dmi_product;
        }
    }

    // 3. Inspect DRM subsystem (/sys/class/drm) for GPUs and drivers
    bool has_nvidia_dgpu = false;
    bool has_amd_dgpu = false;
    bool has_msm_gpu = false;

    try {
        if (fs::exists("/sys/class/drm")) {
            for (const auto & entry : fs::directory_iterator("/sys/class/drm")) {
                std::string filename = entry.path().filename().string();
                if (filename.rfind("card", 0) == 0 && filename.find('-') == std::string::npos) {
                    // Check driver symlink
                    std::string driver_link = entry.path().string() + "/device/driver";
                    if (fs::exists(driver_link)) {
                        std::string target = fs::read_symlink(driver_link).filename().string();
                        if (target == "nvidia") {
                            has_nvidia_dgpu = true;
                        } else if (target == "amdgpu") {
                            // Check if APU or discrete
                            std::string vram_path = entry.path().string() + "/device/mem_info_vram_total";
                            bool is_apu = false;
                            if (fs::exists(vram_path)) {
                                size_t vram_bytes = 0;
                                std::ifstream vf(vram_path);
                                if (vf >> vram_bytes && vram_bytes > 0) {
                                    if (vram_bytes <= (size_t)3 * 1024 * 1024 * 1024ULL) {
                                        // <= 3GB VRAM carved out typically indicates an AMD APU
                                        topo.unified_memory = true;
                                        topo.accelerator = "rocm";
                                        is_apu = true;
                                    } else {
                                        topo.accelerator_memory = std::max(topo.accelerator_memory, vram_bytes);
                                    }
                                }
                            }
                            if (!is_apu) {
                                has_amd_dgpu = true;
                            }
                        } else if (target == "msm" || target == "kgsl") {
                            has_msm_gpu = true;
                        } else if (target == "panfrost" || target == "lima" || target == "asahi") {
                            // ARM Mali or Apple Asahi UMA
                            topo.unified_memory = true;
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Fallback gracefully on restricted filesystem access
    }

    // Check for NVIDIA driver via /proc/driver/nvidia
    if (fs::exists("/proc/driver/nvidia")) {
        has_nvidia_dgpu = true;
        topo.accelerator = "cuda";
    }

    // Determine Unified Memory vs Discrete GPU topology
    if (is_qualcomm_snapdragon || has_msm_gpu) {
        topo.unified_memory = true;
        topo.accelerator = "qualcomm";
        if (topo.device_model == "Generic") topo.device_model = "Qualcomm Snapdragon";
    } else if (is_apple_silicon) {
        topo.unified_memory = true;
        topo.accelerator = "metal";
    } else if (is_tegra) {
        topo.unified_memory = true;
        topo.accelerator = "cuda";
    } else if (has_nvidia_dgpu) {
        topo.unified_memory = false;
        topo.accelerator = "cuda";
        if (topo.accelerator_memory == 0) {
            // Attempt to query via nvidia-smi if available
            FILE* pipe = popen("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null", "r");
            if (pipe) {
                char buffer[128];
                if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                    try {
                        size_t mb = std::stoull(buffer);
                        topo.accelerator_memory = mb * 1024 * 1024;
                    } catch (...) {
                        // ignore parsing error
                    }
                }
                pclose(pipe);
            }
            if (topo.accelerator_memory == 0) {
                // Estimate default discrete GPU VRAM if query not available
                topo.accelerator_memory = (size_t)8 * 1024 * 1024 * 1024ULL; // 8GB typical fallback
            }
        }
    } else if (has_amd_dgpu) {
        topo.unified_memory = false;
        topo.accelerator = "rocm";
    } else if (topo.architecture == "aarch64") {
        // Most ARM64 Linux machines (RPi, Jetson, Radxa, OrangePi) have unified memory
        topo.unified_memory = true;
        topo.accelerator = "cpu";
    } else {
        topo.unified_memory = false;
        topo.accelerator = "cpu";
    }

    // 4. Detect NPU capabilities (FastRPC for Qualcomm Hexagon NPU, Linux accel subsystem)
    bool has_hexagon_npu = false;
    try {
        if (fs::exists("/dev/adsprpc-smd") || fs::exists("/dev/cdsprpc-smd")) {
            has_hexagon_npu = true;
        } else if (fs::exists("/sys/class/misc")) {
            for (const auto & entry : fs::directory_iterator("/sys/class/misc")) {
                std::string fname = entry.path().filename().string();
                if (fname.find("fastrpc") != std::string::npos) {
                    has_hexagon_npu = true;
                    break;
                }
            }
        }
        if (!has_hexagon_npu && is_qualcomm_snapdragon && fs::exists("/sys/class/accel") && !fs::is_empty("/sys/class/accel")) {
            has_hexagon_npu = true;
        }
    } catch (...) {}

    // Populate ComputeTopology
    topo.compute.has_cpu = true;
    topo.compute.cpu_backend = "cpu";
    topo.compute.cpu_arch = topo.architecture;

    if (is_qualcomm_snapdragon || has_msm_gpu) {
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "adreno";
        topo.compute.gpu_model = "Qualcomm Adreno GPU";
        topo.compute.has_npu = true;
        topo.compute.npu_backend = "hexagon";
        topo.compute.npu_model = "Qualcomm Hexagon NPU";
    } else if (is_apple_silicon) {
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "metal";
        topo.compute.gpu_model = "Apple M-Series GPU";
        topo.compute.has_npu = true;
        topo.compute.npu_backend = "coreml";
        topo.compute.npu_model = "Apple Neural Engine (ANE)";
    } else if (is_tegra) {
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "cuda";
        topo.compute.gpu_model = "NVIDIA Tegra / Orin GPU";
        topo.compute.has_npu = true;
        topo.compute.npu_backend = "nv_dla";
        topo.compute.npu_model = "NVIDIA DLA";
    } else if (has_nvidia_dgpu) {
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "cuda";
        topo.compute.gpu_model = "NVIDIA Discrete GPU";
    } else if (has_amd_dgpu) {
        topo.compute.has_gpu = true;
        topo.compute.gpu_backend = "rocm";
        topo.compute.gpu_model = "AMD Discrete GPU";
    }

    if (has_hexagon_npu) {
        topo.compute.has_npu = true;
        topo.compute.npu_backend = "hexagon";
        if (topo.compute.npu_model.empty()) {
            topo.compute.npu_model = "Qualcomm Hexagon NPU";
        }
    }

    // 5. Inspect NVMe / Fast Storage capacity
    try {
        if (fs::exists("/sys/block")) {
            for (const auto & entry : fs::directory_iterator("/sys/block")) {
                std::string disk = entry.path().filename().string();
                if (disk.rfind("nvme", 0) == 0) {
                    std::string size_str = read_first_line(entry.path().string() + "/size");
                    if (!size_str.empty()) {
                        uint64_t sectors = std::stoull(size_str);
                        uint64_t bytes = sectors * 512;
                        topo.storage_capacity += bytes;
                    }
                }
            }
        }
    } catch (...) {}

    // Free space on / or /tmp for swap container
    struct statvfs sv;
    if (statvfs("/tmp", &sv) == 0) {
        topo.storage_free = (size_t)sv.f_bavail * sv.f_frsize;
        if (topo.storage_capacity == 0) {
            topo.storage_capacity = (size_t)sv.f_blocks * sv.f_frsize;
        }
    }

    topo.profile_name = HardwareProfile::profile_to_string(HardwareProfile::determine_profile(topo));
    return topo;
}

#else

MemoryTopology detect_linux_topology() {
    return MemoryTopology{};
}

#endif
