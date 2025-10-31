/*
 * NR-U RSSI/LBT Integration for OpenAirInterface
 * ------------------------------------------------
 * Optimized RSSI measurement and LBT implementation for OAI with USRP
 * Compatible with existing nru_lbt.c and OAI MAC scheduler
 * 
 * Features:
 * - Direct USRP streaming for independent sensing
 * - Thread-safe sample buffer management
 * - Fast energy detection with caching
 * - ETSI EN 301 893 compliant LBT
 * 
 * Author: Integration for OAI NR-U
 * Date: 2025
 */

#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread.hpp>
#include <uhd/types/metadata.hpp>
#include <uhd/types/sensors.hpp>
#include <iostream>
#include <mutex>
#include <atomic>
#include <deque>
#include <complex>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <array>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
// ----------------------------------------------------------------------
// [NRU integration] External scheduler linkage to OAI gNB
// ----------------------------------------------------------------------
extern "C" {
    // Declared in gNB_scheduler.c
    void nr_schedule_response(void *gNB, int frame, int slot);
    extern void *global_gNB_ptr;
}

extern "C" {

/* ============================================
 *  CONFIGURATION CONSTANTS
 * ============================================ */

// Buffer size optimized for 15.36 MSPS (OAI standard rate)
// 65536 samples = ~4.3ms buffer at 15.36 MSPS
static const size_t MAX_BUFFER_SIZE = 65536;

// Cache validity for ultra-fast LBT checks
static const uint64_t CACHE_VALIDITY_US = 500;  // 0.5ms

// Default calibration offset (adjust based on hardware)
static constexpr float DEFAULT_CALIBRATION_OFFSET_DB = .0f;

// LBT timing constants (ETSI EN 301 893 compliance)
static const int DEFAULT_FBE_SENSING_US = 25;   // Frame-Based Equipment
static const int DEFAULT_LBE_SENSING_US = 100;  // Load-Based Equipment

/* ============================================
 *  GLOBAL STATE
 * ============================================ */

static uhd::usrp::multi_usrp::sptr global_usrp = nullptr;

// Thread-safe sample buffer
static std::deque<std::complex<float>> sample_buffer;
static std::mutex buffer_mutex;

// Energy detection state
static std::atomic<float> cached_energy_dbm{-90.0f};
static std::atomic<uint64_t> last_measurement_time_us{0};

// Configuration
float noise_floor_dbm = -90.0f;
float nru_config_ed_threshold_dbm = -82.0f;
bool noise_calibrated = false;
static float calibration_offset_db = DEFAULT_CALIBRATION_OFFSET_DB;

// Performance monitoring
static std::atomic<uint64_t> total_samples_received{0};
static std::atomic<uint64_t> total_samples_dropped{0};
static std::atomic<uint64_t> buffer_overflow_count{0};
static std::atomic<uint64_t> lbt_checks_performed{0};
static std::atomic<uint64_t> channel_busy_count{0};

// Direct streaming control
static std::atomic<bool> sensing_thread_running{false};
static std::thread sensing_thread;

/* ============================================
 *  TIME HELPERS
 * ============================================ */

static inline uint64_t get_time_us() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
}

/* ============================================
 *  SAMPLE BUFFER MANAGEMENT
 * ============================================ */

/**
 * Feed samples from external source
 * Non-blocking to prevent thread stalls
 */
void nru_feed_samples(const std::complex<float>* samples, size_t count) {
    if (!samples || count == 0) return;
    
    total_samples_received.fetch_add(count, std::memory_order_relaxed);
    
    // Non-blocking lock - if busy, skip this batch
    std::unique_lock<std::mutex> lock(buffer_mutex, std::defer_lock);
    if (!lock.try_lock()) {
        total_samples_dropped.fetch_add(count, std::memory_order_relaxed);
        return;
    }
    
    // Make room if needed (FIFO buffer)
    if (sample_buffer.size() + count > MAX_BUFFER_SIZE) {
        size_t to_remove = sample_buffer.size() + count - MAX_BUFFER_SIZE;
        sample_buffer.erase(sample_buffer.begin(), 
                           sample_buffer.begin() + to_remove);
        buffer_overflow_count.fetch_add(1, std::memory_order_relaxed);
    }
    
    // Add new samples
    sample_buffer.insert(sample_buffer.end(), samples, samples + count);
}

/**
 * Alternative: Feed from int16_t samples (OAI standard format)
 * Converts from 12-bit shifted format to normalized float
 */
void nru_feed_samples_int16(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return;
    
    // Convert to complex float (I/Q interleaved)
    std::vector<std::complex<float>> float_samples(count / 2);
    for (size_t i = 0; i < count / 2; i++) {
        float i_val = static_cast<float>(samples[2*i]) / 32768.0f;
        float q_val = static_cast<float>(samples[2*i + 1]) / 32768.0f;
        float_samples[i] = std::complex<float>(i_val, q_val);
    }
    
    nru_feed_samples(float_samples.data(), float_samples.size());
}
/**
 * Feed samples from OAI's main RX path
 * Decimates to avoid overwhelming the buffer
 * Called from nr-ru.c rx_rf() function
 */
void nru_feed_from_main_rx(const void* samples, size_t count, bool is_int16) {
    // Decimate - only process every 32nd batch to reduce load
    static std::atomic<int> sample_counter{0};
    const int DECIMATION_FACTOR = 8;
    
    int current_count = sample_counter.fetch_add(1, std::memory_order_relaxed);
    if ((current_count % DECIMATION_FACTOR) != 0) {
        return;  // Skip this batch
    }
    
    if (!samples || count == 0) return;
    
    if (is_int16) {
        // OAI uses int16_t I/Q samples
        nru_feed_samples_int16(static_cast<const int16_t*>(samples), count);
    } else {
        // Already complex float
        nru_feed_samples(static_cast<const std::complex<float>*>(samples), count);
    }
}
/* ============================================
 *  ENERGY CALCULATION
 * ============================================ */

/**
 * Fast software energy calculation
 * Uses last 500 samples for ~32μs measurement at 15.36 MSPS
 */
static float calculate_energy_from_samples_fast() {
    std::unique_lock<std::mutex> lock(buffer_mutex, std::defer_lock);
    
    // Non-blocking - return cached if busy
    if (!lock.try_lock()) {
        return cached_energy_dbm.load(std::memory_order_relaxed);
    }
    
    if (sample_buffer.size() < 100) {
        return noise_floor_dbm;
    }
    
    // Use last 500 samples for speed
    size_t n = std::min(sample_buffer.size(), static_cast<size_t>(500));
    size_t start = sample_buffer.size() - n;
    
    double sum_power = 0.0;
    for (size_t i = start; i < sample_buffer.size(); ++i) {
        sum_power += std::norm(sample_buffer[i]);
    }
    
    double mean_power = sum_power / n;
    double dbfs = 10.0 * std::log10(std::max(mean_power, 1e-12));
    float energy_dbm = static_cast<float>(dbfs + calibration_offset_db);
    
    return std::isfinite(energy_dbm) ? energy_dbm : noise_floor_dbm;
}

/**
 * Accurate energy calculation (more samples)
 * Uses up to 2000 samples for better accuracy
 */
static float calculate_energy_from_samples_accurate() {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    
    if (sample_buffer.size() < 100) {
        return noise_floor_dbm;
    }
    
    // Use more samples for accuracy
    size_t n = std::min(sample_buffer.size(), static_cast<size_t>(2000));
    size_t start = sample_buffer.size() - n;
    
    double sum_power = 0.0;
    for (size_t i = start; i < sample_buffer.size(); ++i) {
        sum_power += std::norm(sample_buffer[i]);
    }
    
    double mean_power = sum_power / n;
    double dbfs = 10.0 * std::log10(std::max(mean_power, 1e-12));
    float energy_dbm = static_cast<float>(dbfs + calibration_offset_db);
    
    return std::isfinite(energy_dbm) ? energy_dbm : noise_floor_dbm;
}

/* ============================================
 *  RSSI MEASUREMENT API
 * ============================================ */

/**
 * Get current energy with caching (ultra-fast)
 * Returns cached value if fresh enough
 */
float nru_get_current_energy_dbm(void) {
    uint64_t now = get_time_us();
    uint64_t cache_age = now - last_measurement_time_us.load(std::memory_order_relaxed);
    
    // Return cached value if still valid
    if (cache_age < CACHE_VALIDITY_US) {
        return cached_energy_dbm.load(std::memory_order_relaxed);
    }
    
    // Compute fresh measurement
    float energy = calculate_energy_from_samples_fast();
    std::cout << "[NRU][DEBUG] Energy reading = " << energy 
          << " dBm | Buffer size = " << sample_buffer.size() << std::endl;

    // Update cache
    cached_energy_dbm.store(energy, std::memory_order_relaxed);
    last_measurement_time_us.store(now, std::memory_order_relaxed);
    
    return energy;
}

/**
 * Force fresh measurement (bypass cache)
 * Used for calibration and critical measurements
 */
float nru_get_current_energy_dbm_no_cache(void) {
    last_measurement_time_us.store(0, std::memory_order_relaxed);
    return calculate_energy_from_samples_accurate();
}

/* ============================================
 *  LBT IMPLEMENTATION (ETSI EN 301 893)
 * ============================================ */

/**
 * Generic LBT check with configurable sensing time
 * 
 * @param sensing_time_us: Sensing duration in microseconds
 * @return: 1 if channel FREE, 0 if BUSY, -1 on error
 */
int nru_lbt_check_timed(int sensing_time_us) {
    if (sensing_time_us <= 0) {
        sensing_time_us = DEFAULT_FBE_SENSING_US;
    }
    
    lbt_checks_performed.fetch_add(1, std::memory_order_relaxed);
    
    uint64_t start_time = get_time_us();
    float max_energy = noise_floor_dbm;
    int measurements = 0;
    
    // Calculate measurement interval
    int measurement_interval_us = (sensing_time_us < 50) ? 
                                   (sensing_time_us / 4) : 10;
    
    // Sensing loop
    while (true) {
        uint64_t elapsed = get_time_us() - start_time;
        if (elapsed >= static_cast<uint64_t>(sensing_time_us)) {
            break;
        }
        
        // Measure energy
        float energy = nru_get_current_energy_dbm();
        if (energy > max_energy) {
            max_energy = energy;
        }
        measurements++;
        
        // Early exit if clearly busy
        if (max_energy >= nru_config_ed_threshold_dbm) {
            channel_busy_count.fetch_add(1, std::memory_order_relaxed);
            return 0;  // BUSY
        }
        
        // Sleep to avoid CPU starvation
        int remaining_us = sensing_time_us - elapsed;
        if (remaining_us > measurement_interval_us) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(measurement_interval_us / 2)
            );
        }
    }
    
    // Final decision
    bool channel_free = (max_energy < nru_config_ed_threshold_dbm);
    if (!channel_free) {
        channel_busy_count.fetch_add(1, std::memory_order_relaxed);
    }
    
    return channel_free ? 1 : 0;
}

/**
 * Standard FBE LBT check (25μs sensing)
 * ETSI EN 301 893 v2.1.1 Section 4.9.2.4
 */
int nru_lbt_check(void) {
    return nru_lbt_check_timed(DEFAULT_FBE_SENSING_US);
}

/**
 * Quick single-shot check (uses cached value)
 * Fastest option for time-critical checks
 */
int nru_lbt_check_fast(void) {
    lbt_checks_performed.fetch_add(1, std::memory_order_relaxed);
    
    float energy = nru_get_current_energy_dbm();
    bool channel_free = (energy < nru_config_ed_threshold_dbm);
    
    if (!channel_free) {
        channel_busy_count.fetch_add(1, std::memory_order_relaxed);
    }
    
    return channel_free ? 1 : 0;
}

/**
 * LBE mode check (100μs sensing)
 * ETSI EN 301 893 v2.1.1 Section 4.9.2.5
 */
int nru_lbt_check_lbe(void) {
    return nru_lbt_check_timed(DEFAULT_LBE_SENSING_US);
}

/**
 * Extended Clear Channel Assessment with backoff
 * Used for initial channel access in LBE mode
 */
int nru_lbt_extended_cca(int defer_duration_us, int max_attempts) {
    if (defer_duration_us <= 0) defer_duration_us = 34;  // ETSI default
    if (max_attempts <= 0) max_attempts = 10;
    
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        // Initial defer period
        std::this_thread::sleep_for(
            std::chrono::microseconds(defer_duration_us)
        );
        
        // Check channel
        int result = nru_lbt_check_lbe();
        if (result == 1) {
            return 1;  // Channel free
        }
        
        // Random backoff (exponential)
        int backoff_us = (rand() % (1 << std::min(attempt, 5))) * 9;  // 9μs slots
        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
    }
    
    return 0;  // Channel remained busy
}

/* ============================================
 *  CALIBRATION
 * ============================================ */

/**
 * Calibrate noise floor
 * Should be done with no signal present
 */
void nru_calibrate_noise_floor(int samples) {
    if (samples <= 0) samples = 100;

    std::cout << "[NRU][UHD] Calibrating noise floor (" << samples
              << " measurements)...\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    double sum = 0.0;
    int valid_count = 0;
    last_measurement_time_us.store(0, std::memory_order_relaxed);

    for (int i = 0; i < samples; i++) {
        float energy = nru_get_current_energy_dbm_no_cache();
        if (std::isfinite(energy) && energy > -120.0f && energy < -50.0f) {
            sum += energy;
            valid_count++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (valid_count > samples / 2) {
        noise_floor_dbm = static_cast<float>(sum / valid_count);
        noise_calibrated = true;
        nru_config_ed_threshold_dbm = noise_floor_dbm + 8.0f;
        std::cout << "[NRU][UHD] ✅ Noise floor: " << noise_floor_dbm
                  << " dBm (from " << valid_count << " valid samples)\n";
        std::cout << "[NRU][UHD] ✅ ED threshold: "
                  << nru_config_ed_threshold_dbm << " dBm\n";
    } else {
        std::cerr << "[NRU][UHD] ⚠️  Calibration failed (only "
                  << valid_count << " valid samples)\n";
        return;
    }

    // ------------------------------------------------------------
    // 🔧 AUTO-CALIBRATION against laptop Wi-Fi RSSI
    // ------------------------------------------------------------
    FILE *pipe = popen(
        "iw dev wlp0s20f3 link | grep 'signal:' | awk '{print $2}'", "r");
    if (pipe) {
        float wifi_rssi_dbm = NAN;
        fscanf(pipe, "%f", &wifi_rssi_dbm);
        pclose(pipe);

        if (std::isfinite(wifi_rssi_dbm)) {
            // Measure current mean in dBFS (before offset)
            std::unique_lock<std::mutex> lock(buffer_mutex, std::defer_lock);
            if (lock.try_lock() && sample_buffer.size() > 500) {
                size_t n = std::min((size_t)500, sample_buffer.size());
                size_t start = sample_buffer.size() - n;
                double sum_power = 0.0;
                for (size_t j = start; j < sample_buffer.size(); ++j)
                    sum_power += std::norm(sample_buffer[j]);
                double dbfs = 10.0 * std::log10(std::max(sum_power / n, 1e-12));
                calibration_offset_db = wifi_rssi_dbm - dbfs;

                std::cout << "[NRU][CAL] ✅ Auto-calibration from Wi-Fi RSSI\n"
                          << "    Wi-Fi RSSI : " << wifi_rssi_dbm << " dBm\n"
                          << "    Measured   : " << dbfs << " dBFS\n"
                          << "    Offset     : " << calibration_offset_db
                          << " dB\n";
            }
        } else {
            std::cerr << "[NRU][CAL] ⚠️  Could not parse Wi-Fi RSSI\n";
        }
    } else {
        std::cerr << "[NRU][CAL] ⚠️  Failed to execute iw command\n";
    }

    std::cout << "[NRU][UHD] 🧩 Final calibration offset: "
              << calibration_offset_db << " dB\n";
}

/**
 * Set manual calibration offset
 */
void nru_set_calibration_offset(float offset_db) {
    calibration_offset_db = offset_db;
    std::cout << "[NRU][UHD] Calibration offset: " << offset_db << " dB\n";
}
int nru_read_samples(std::complex<float>* dst, int max_count) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    int n = std::min<int>(max_count, sample_buffer.size());
    for (int i = 0; i < n; ++i) {
        dst[i] = sample_buffer.front();
        sample_buffer.pop_front();
    }
    return n;
}

/**
 * Auto-calibrate using known reference signal
 */
void nru_auto_calibrate_offset(float known_power_dbm) {
    std::cout << "[NRU][UHD] Auto-calibrating with " << known_power_dbm 
              << " dBm reference...\n";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    double sum_dbfs = 0.0;
    int count = 0;
    
    for (int i = 0; i < 50; i++) {
        std::unique_lock<std::mutex> lock(buffer_mutex, std::defer_lock);
        if (!lock.try_lock()) continue;
        
        if (sample_buffer.size() < 500) continue;
        
        size_t n = std::min(static_cast<size_t>(500), sample_buffer.size());
        size_t start = sample_buffer.size() - n;
        
        double sum_power = 0.0;
        for (size_t j = start; j < sample_buffer.size(); ++j) {
            sum_power += std::norm(sample_buffer[j]);
        }
        
        double dbfs = 10.0 * std::log10(std::max(sum_power / n, 1e-12));
        sum_dbfs += dbfs;
        count++;
        
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    if (count > 0) {
        float measured_dbfs = static_cast<float>(sum_dbfs / count);
        calibration_offset_db = known_power_dbm - measured_dbfs;
        
        std::cout << "[NRU][UHD] ✅ Calibration offset: " 
                  << calibration_offset_db << " dB\n";
        std::cout << "[NRU][UHD] (Measured " << measured_dbfs 
                  << " dBFS for " << known_power_dbm << " dBm signal)\n";
    } else {
        std::cerr << "[NRU][UHD] ⚠️  Auto-calibration failed\n";
    }
}

/* ============================================
 *  CONFIGURATION
 * ============================================ */

void nru_set_ed_threshold(float threshold_dbm) {
    nru_config_ed_threshold_dbm = threshold_dbm;
    std::cout << "[NRU][UHD] ED threshold: " << threshold_dbm << " dBm\n";
}

float nru_get_ed_threshold(void) {
    return nru_config_ed_threshold_dbm;
}

float nru_get_noise_floor(void) {
    return noise_floor_dbm;
}

/* ============================================
 *  DIRECT RX STREAMING FOR SENSING
 * ============================================ */

/**
 * Background thread for continuous sample acquisition
 * Runs independently from OAI's main RX stream
 */
static void sensing_stream_worker() {
    if (!global_usrp) {
        std::cerr << "[NRU][STREAM] ❌ USRP not attached\n";
        return;
    }
    
    std::cout << "[NRU][STREAM] 🚀 Starting dedicated sensing stream...\n";
    
    try {
        // Create separate RX stream for sensing
        uhd::stream_args_t stream_args("fc32", "sc16");  // Complex float, wire format sc16
        stream_args.channels = {0};  // Use channel 0
        
        uhd::rx_streamer::sptr sensing_rx_stream = global_usrp->get_rx_stream(stream_args);
        
        // Buffer for receiving samples (optimized size)
        const size_t samps_per_buff = 1024;  // ~67μs at 15.36 MSPS
        std::vector<std::complex<float>> buff(samps_per_buff);
        
        // Metadata for stream control
        uhd::rx_metadata_t md;
        
        // Start streaming
        uhd::stream_cmd_t stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        stream_cmd.stream_now = true;
        sensing_rx_stream->issue_stream_cmd(stream_cmd);
        
        std::cout << "[NRU][STREAM] ✅ Sensing stream started\n";
        std::cout << "[NRU][STREAM] Sample rate: " 
                  << (global_usrp->get_rx_rate(0) / 1e6) << " MSps\n";
        std::cout << "[NRU][STREAM] Frequency: " 
                  << (global_usrp->get_rx_freq(0) / 1e6) << " MHz\n";
        
        uint64_t total_received = 0;
        uint64_t error_count = 0;
        auto last_report = std::chrono::steady_clock::now();
        
        // Main sensing loop
        while (sensing_thread_running.load(std::memory_order_relaxed)) {
            // Receive samples (timeout 0.1s)
            size_t num_rx_samps = sensing_rx_stream->recv(
                buff.data(), 
                samps_per_buff, 
                md, 
                0.1  // timeout
            );
            
            // Handle metadata errors
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) {
                continue;  // Normal timeout, keep trying
            }
            
            if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) {
                error_count++;
                if (error_count % 100 == 0) {
                    std::cerr << "[NRU][STREAM] ⚠️  Overflow detected (count: " 
                              << error_count << ")\n";
                }
                continue;
            }
            
            if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
                std::cerr << "[NRU][STREAM] ❌ Error: " << md.strerror() << "\n";
                continue;
            }
            
            // Feed samples to buffer
            if (num_rx_samps > 0) {
                nru_feed_samples(buff.data(), num_rx_samps);
                total_received += num_rx_samps;
            }
            
            // Periodic status report (every 10 seconds)
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count() >= 10) {
                float mbytes = (total_received * sizeof(std::complex<float>)) / (1024.0f * 1024.0f);
                std::cout << "[NRU][STREAM] 📊 Received " << total_received 
                          << " samples (" << std::fixed << std::setprecision(2) 
                          << mbytes << " MB) | Errors: " << error_count << "\n";
                last_report = now;
            }
        }
        
        // Stop streaming
        stream_cmd.stream_mode = uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;
        sensing_rx_stream->issue_stream_cmd(stream_cmd);
        
        std::cout << "[NRU][STREAM] 🛑 Sensing stream stopped\n";
        std::cout << "[NRU][STREAM] Total samples: " << total_received << "\n";
        
    } catch (const std::exception& e) {
        std::cerr << "[NRU][STREAM] ❌ Exception: " << e.what() << "\n";
        sensing_thread_running.store(false, std::memory_order_relaxed);
    }
}

/**
 * Start dedicated sensing stream
 * Call this after nru_attach_usrp()
 */


void nru_start_sensing_stream(void) {
    if (!global_usrp) {
        std::cerr << "[NRU][STREAM] ❌ USRP not attached\n";
        return;
    }

    // DISABLED: No dedicated stream - using main OAI RX path
    std::cout << "[NRU][STREAM] ✅ Using main RX stream for energy detection\n";
    std::cout << "[NRU][STREAM] No dedicated sensing stream (prevents USB overflows)\n";
    
    sensing_thread_running.store(true, std::memory_order_relaxed);
    
    // Initialize with noise floor
    cached_energy_dbm.store(noise_floor_dbm, std::memory_order_relaxed);
    last_measurement_time_us.store(get_time_us(), std::memory_order_relaxed);
    
    std::cout << "[NRU][STREAM] ✅ Ready to receive samples from main RX path\n";
    
    // DO NOT start sensing_thread - no dedicated stream needed
}



/**
 * Stop dedicated sensing stream
 */
void nru_stop_sensing_stream(void) {
    // No dedicated stream to stop
    sensing_thread_running.store(false, std::memory_order_relaxed);
    std::cout << "[NRU][STREAM] LBT module deactivated\n";
}

/**
 * Check if sensing stream is running
 */
bool nru_is_sensing_active(void) {
    return sensing_thread_running.load(std::memory_order_relaxed);
}

/* ============================================
 *  INITIALIZATION & CLEANUP
 * ============================================ */

/**
 * Attach to existing USRP instance from OAI
 * Called after USRP initialization in device_init()
 */
void nru_attach_usrp(void *priv) {
    if (!priv) {
        std::cerr << "[NRU][UHD] ❌ Null USRP handle\n";
        return;
    }
    
    global_usrp = *static_cast<uhd::usrp::multi_usrp::sptr*>(priv);
    std::cout << "[NRU][UHD] ✅ Attached to USRP device\n";
    
    // Set thread priority for better real-time performance
    try {
        uhd::set_thread_priority_safe(0.9, true);
        std::cout << "[NRU][UHD] ✅ Thread priority elevated\n";
    } catch (const std::exception& e) {
        std::cerr << "[NRU][UHD] ⚠️  Thread priority: " << e.what() << "\n";
    }
    
    // Get RX gain for info
    try {
        float rx_gain = static_cast<float>(global_usrp->get_rx_gain(0));
        std::cout << "[NRU][UHD] RX gain: " << rx_gain << " dB\n";
    } catch (...) {}
    
    // Initialize buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        sample_buffer.clear();
    }
    
    // Reset state
    cached_energy_dbm.store(noise_floor_dbm, std::memory_order_relaxed);
    last_measurement_time_us.store(0, std::memory_order_relaxed);
    total_samples_received.store(0, std::memory_order_relaxed);
    total_samples_dropped.store(0, std::memory_order_relaxed);
    buffer_overflow_count.store(0, std::memory_order_relaxed);
    lbt_checks_performed.store(0, std::memory_order_relaxed);
    channel_busy_count.store(0, std::memory_order_relaxed);
    
    std::cout << "[NRU][UHD] ✅ Ready for software-based energy detection\n";
    
    // Auto-start sensing stream
    nru_start_sensing_stream();
}

/**
 * Cleanup resources
 */
void nru_cleanup(void) {
    std::cout << "[NRU][UHD] Cleaning up...\n";
    
    // Stop sensing stream first
    nru_stop_sensing_stream();
    
    // Clear buffer
    {
        std::lock_guard<std::mutex> lock(buffer_mutex);
        sample_buffer.clear();
    }
    
    global_usrp = nullptr;
    std::cout << "[NRU][UHD] ✅ Cleanup complete\n";
}

/* ============================================
 *  STREAM CONTROL (NO-OP FOR OAI)
 * ============================================ */

// These are intentionally no-ops because OAI manages the main RX stream
void nru_stop_rx_stream(void) {
    // Managed by OAI - do nothing
}

void nru_restart_rx_stream(void) {
    // Managed by OAI - do nothing
}

/* ============================================
 *  DEBUGGING & MONITORING
 * ============================================ */

void nru_print_stats(void) {
    float energy = nru_get_current_energy_dbm();
    bool is_free = (energy < nru_config_ed_threshold_dbm);
    uint64_t cache_age = get_time_us() - last_measurement_time_us.load();
    
    uint64_t received = total_samples_received.load();
    uint64_t dropped = total_samples_dropped.load();
    uint64_t overflows = buffer_overflow_count.load();
    uint64_t lbt_checks = lbt_checks_performed.load();
    uint64_t busy_count = channel_busy_count.load();
    
    float drop_rate = (received > 0) ? (100.0f * dropped / received) : 0.0f;
    float busy_rate = (lbt_checks > 0) ? (100.0f * busy_count / lbt_checks) : 0.0f;
    
    std::cout << "\n[NRU][STATS] ==========================================\n";
     std::cout << "[NRU][STATS] Energy: " << energy << " dBm"
              << " | Threshold: " << nru_config_ed_threshold_dbm
              << " dBm | State: " << (is_free ? "✅ FREE" : "❌ BUSY") << "\n";
    std::cout << "[NRU][STATS] Cache age: " << cache_age << " µs\n";
    std::cout << "[NRU][STATS] Samples received: " << received
              << " | Dropped: " << dropped
              << " | Overflows: " << overflows << "\n";
    std::cout << "[NRU][STATS] LBT checks: " << lbt_checks
              << " | Busy count: " << busy_count
              << " (" << busy_rate << "% busy)\n";
    std::cout << "[NRU][STATS] Drop rate: " << drop_rate << "%\n";
    std::cout << "[NRU][STATS] ==========================================\n\n";
}
   

/**
 * Reset all runtime counters without detaching the USRP
 */
void nru_reset_stats(void) {
    total_samples_received.store(0);
    total_samples_dropped.store(0);
    buffer_overflow_count.store(0);
    lbt_checks_performed.store(0);
    channel_busy_count.store(0);
    std::cout << "[NRU][UHD] ✅ Statistics counters reset\n";
}

/**
 * Force flush of buffered samples
 */
void nru_clear_buffer(void) {
    std::lock_guard<std::mutex> lock(buffer_mutex);
    sample_buffer.clear();
    std::cout << "[NRU][UHD] 🧹 Buffer cleared\n";
}

/**
 * Manual trigger for short energy test (debug)
 */
void nru_debug_energy_probe(int count) {
    if (count <= 0) count = 10;
    std::cout << "[NRU][DEBUG] Energy probe (" << count << " samples):\n";
    for (int i = 0; i < count; i++) {
        float e = nru_get_current_energy_dbm_no_cache();
        std::cout << "  [" << i << "] " << e << " dBm\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

/**
 * Export runtime status summary for external monitoring
 * Returns 1 if sensing active, 0 otherwise
 */
int nru_get_runtime_status(void) {
    bool active = nru_is_sensing_active();
    std::cout << "[NRU][STATUS] Sensing: " << (active ? "ON" : "OFF")
              << " | Energy: " << nru_get_current_energy_dbm()
              << " dBm | Threshold: " << nru_config_ed_threshold_dbm << " dBm\n";
    return active ? 1 : 0;
}

} // extern "C"
