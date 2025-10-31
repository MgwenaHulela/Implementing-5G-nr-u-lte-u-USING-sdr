/*
 * NR-U LBT Header File
 * --------------------
 * Interface between OAI MAC and UHD RSSI measurement
 * 
 * Location: common/utils/nru_lbt.h
 */

#ifndef NRU_LBT_H
#define NRU_LBT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 *  CONFIGURATION STRUCTURES
 * ============================================ */

/**
 * LBT Mode enumeration
 */
typedef enum {
    LBT_MODE_FBE = 0,    // Frame-Based Equipment
    LBT_MODE_LBE = 1,    // Load-Based Equipment
    LBT_MODE_DISABLED = 2
} lbt_mode_t;

/**
 * FBE (Frame-Based Equipment) Configuration
 */
typedef struct {
    lbt_mode_t mode;                   // LBT mode
    uint64_t T_frame_us;               // Frame period in microseconds
    uint64_t T_on_us;                  // TX window duration
    uint64_t start_time_us;            // Reference start time
    uint64_t jitter_us;                // Timing jitter tolerance
    int gnb_id;                        // gNB identifier
    int log_level;                     // Logging verbosity
    struct {
        double max_duty;               // Maximum duty cycle (0.0 - 1.0)
    } duty;
} nru_fbe_cfg_t;

/**
 * Main NR-U LBT Configuration
 */
typedef struct {
    bool enabled;                      // Enable/disable LBT
    char mode[16];                     // "FBE" or "LBE"
    
    // Energy Detection Parameters
    int ed_threshold_dbm;              // Energy detection threshold (dBm)
    int ed_sensing_time_us;            // Sensing duration (microseconds)
    
    // Frame-Based Equipment (FBE) Parameters
    int frame_period_ms;               // Frame period (milliseconds)
    int tx_window_ms;                  // TX window size (milliseconds)
    double duty_cycle_percent;         // Duty cycle percentage
    nru_fbe_cfg_t fbe_cfg;            // FBE-specific configuration
    
    // Load-Based Equipment (LBE) Parameters
    int mcot_ms;                       // Maximum Channel Occupancy Time (ms)
    int defer_period_us;               // Defer period before sensing (μs)
    int backoff_slots;                 // Number of backoff slots
    int cw_min;                        // Contention window minimum
    int cw_max;                        // Contention window maximum
    
    // Logging
    bool log_lbt;                      // Enable LBT event logging
} nru_cfg_t;

/**
 * Global FBE configuration (for compatibility)
 */
extern nru_fbe_cfg_t global_fbe_cfg;

/* ============================================
 *  CORE LBT FUNCTIONS (nru_lbt.c)
 * ============================================ */

/**
 * Initialize NR-U LBT subsystem
 * @param cfg: Configuration structure
 * @return: 0 on success, -1 on error
 */
int nru_lbt_init(const nru_cfg_t *cfg);

/**
 * Perform channel sensing and acquire channel
 * @param gnb_id: gNB identifier
 * @param required_us: Required transmission duration (μs)
 * @return: 1 if channel acquired, 0 if busy
 */
int nru_lbt_sense_and_acquire(int gnb_id, int required_us);
// === ADD THESE NEW DECLARATIONS ===
// Channel stability checking (for UE access control)
int nru_lbt_is_stable_for_ue_access(void);
int nru_lbt_get_consecutive_free(void);
bool nru_lbt_is_channel_stable(void);
void nru_lbt_reset_stability(void);
/**
 * Called after transmission completes
 */
void nru_lbt_on_tx_complete(void);

/**
 * FBE heartbeat (periodic call for duty cycle management)
 */
void nru_fbe_heartbeat(void);

/**
 * Get current configuration
 */
const nru_cfg_t* nru_get_cfg(void);

/**
 * Update configuration dynamically
 */
int nru_lbt_update_cfg(const nru_cfg_t *cfg);

/**
 * Update ED threshold
 */
int nru_update_ed_threshold(float new_threshold_dbm);

/**
 * Process USRP samples (optional direct path)
 */
int nru_lbt_process_usrp_samples(void *samples, int len);

/**
 * Get current time in microseconds
 */
uint64_t nru_time_now_us(void);

/**
 * FBE-specific functions
 */
int nru_fbe_init(nru_fbe_cfg_t *cfg, int gnb_id);
bool nru_fbe_tx_allowed(nru_fbe_cfg_t *cfg, uint64_t now_us);

/* ============================================
 *  UHD HELPER FUNCTIONS (nru_uhd_rssi_oai.cpp)
 * ============================================ */

/**
 * Attach to existing USRP device
 * Called from device_init() in usrp_lib.cpp
 * @param priv: Pointer to uhd::usrp::multi_usrp::sptr
 */
void nru_attach_usrp(void *priv);

/**
 * Feed IQ samples to RSSI measurement buffer
 * Called from trx_usrp_read() after receiving samples
 * @param samples: Complex float samples
 * @param count: Number of samples
 */
void nru_feed_samples(const void *samples, size_t count);

/**
 * Feed int16_t samples (OAI standard format)
 * @param samples: Interleaved I/Q int16_t samples
 * @param count: Total number of int16_t values (I+Q)
 */
void nru_feed_samples_int16(const int16_t *samples, size_t count);

/**
 * Get current energy level
 * @return: Energy in dBm (uses cached value if recent)
 */
float nru_get_current_energy_dbm(void);

/**
 * Get fresh energy measurement (bypass cache)
 * @return: Energy in dBm
 */
float nru_get_current_energy_dbm_no_cache(void);

/**
 * Perform LBT check with timing
 * @param sensing_time_us: Sensing duration in microseconds
 * @return: 1 if FREE, 0 if BUSY, -1 on error
 */
int nru_lbt_check_timed(int sensing_time_us);

/**
 * Standard FBE LBT check (25μs sensing)
 * @return: 1 if FREE, 0 if BUSY
 */
int nru_lbt_check(void);

/**
 * Fast LBT check (uses cached energy)
 * @return: 1 if FREE, 0 if BUSY
 */
int nru_lbt_check_fast(void);

/**
 * LBE mode check (100μs sensing)
 * @return: 1 if FREE, 0 if BUSY
 */
int nru_lbt_check_lbe(void);

/**
 * Extended CCA with backoff (for LBE initial access)
 * @param defer_duration_us: Initial defer period
 * @param max_attempts: Maximum retry attempts
 * @return: 1 if channel acquired, 0 if failed
 */
int nru_lbt_extended_cca(int defer_duration_us, int max_attempts);

/**
 * Calibrate noise floor
 * Should be called with no signal present
 * @param samples: Number of measurements to average
 */
void nru_calibrate_noise_floor(int samples);

/**
 * Set manual calibration offset
 * @param offset_db: Offset in dB (dBm = dBFS + offset)
 */
void nru_set_calibration_offset(float offset_db);

/**
 * Auto-calibrate using known reference signal
 * @param known_power_dbm: Known signal power in dBm
 */
void nru_auto_calibrate_offset(float known_power_dbm);

/**
 * Set energy detection threshold
 * @param threshold_dbm: Threshold in dBm
 */
void nru_set_ed_threshold(float threshold_dbm);

/**
 * Get current ED threshold
 * @return: Threshold in dBm
 */
float nru_get_ed_threshold(void);

/**
 * Get noise floor
 * @return: Noise floor in dBm
 */
float nru_get_noise_floor(void);

/**
 * Stop RX stream (no-op in OAI integration)
 */
void nru_stop_rx_stream(void);

/**
 * Restart RX stream (no-op in OAI integration)
 */
void nru_restart_rx_stream(void);

/**
 * Cleanup resources
 */
void nru_cleanup(void);
extern nru_cfg_t g_nru_config;


/**
 * Print statistics
 */
void nru_print_stats(void);

/**
 * Get sample buffer size
 * @return: Number of samples in buffer
 */
size_t nru_get_buffer_size(void);

/**
 * Clear sample buffer
 */
void nru_clear_buffer(void);

/**
 * Print USRP information
 */
void nru_print_usrp_info(void);

/* ============================================
 *  GLOBAL VARIABLES (defined in .cpp)
 * ============================================ */

extern float noise_floor_dbm;
extern float nru_config_ed_threshold_dbm;
extern bool noise_calibrated;

#ifdef __cplusplus
}
#endif

#endif /* NRU_LBT_H */