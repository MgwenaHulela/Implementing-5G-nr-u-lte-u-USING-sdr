/*
 * NR-U LBT Core (OAI-compatible)
 * -------------------------------
 * Complete Listen-Before-Talk module for NR-U experiments with USRP-based sensing.
 * Works seamlessly with the C++ helper (nru_uhd_helper.cpp) for real RSSI measurements.
 * Author: Innocent Nhlanhla  (University of Cape Town)
 * Date: 2025-10-21
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>  
#include <time.h>
#include <sys/stat.h>
#include "common/utils/nru_lbt.h"
#include "NR_MAC_gNB/mac_proto.h"
#include "openair1/SCHED_NR/fapi_nr_l1.h"
#include "openair2/NR_PHY_INTERFACE/NR_IF_Module.h"

#include "NR_IF_Module.h"

extern NR_IF_Module_t NR_IF_Module[];
#define NR_IF_Module_get(i) (&NR_IF_Module[i])
;

// ---------------------------------------------------------------------
// External UHD helper hooks (from nru_uhd_helper.cpp)
// ---------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
void  nru_attach_usrp(void *priv);
void  nru_calibrate_noise_floor(int samples);
float nru_get_current_energy_dbm(void);
void  nru_set_ed_threshold(float threshold_dbm);
void  nru_stop_rx_stream(void);
void  nru_restart_rx_stream(void);
void  nru_cleanup(void);
extern float noise_floor_dbm;
extern float nru_config_ed_threshold_dbm;

int nr_is_prach_slot(int module_idP, int frame, int slot);  // used in scheduler

// ---------------------------------------------------------------------
// PRACH Slot Detection Implementation
// ---------------------------------------------------------------------
int nr_is_prach_slot(int module_idP, int frame, int slot) {
    // Get gNB instance
    if (!RC.nrmac || !RC.nrmac[module_idP]) {
        return 0;
    }
    
    gNB_MAC_INST *gNB = RC.nrmac[module_idP];
    if (!gNB->common_channels) {
        return 0;
    }
    
    // Use first carrier (like in gNB_scheduler_RA.c)
    NR_COMMON_channels_t *cc = &gNB->common_channels[0];
    
    if (!cc->ServingCellConfigCommon || 
        !cc->ServingCellConfigCommon->uplinkConfigCommon ||
        !cc->ServingCellConfigCommon->uplinkConfigCommon->initialUplinkBWP ||
        !cc->ServingCellConfigCommon->uplinkConfigCommon->initialUplinkBWP->rach_ConfigCommon) {
        return 0;
    }
    
    NR_RACH_ConfigCommon_t *rach_ConfigCommon = 
        cc->ServingCellConfigCommon->uplinkConfigCommon->initialUplinkBWP->rach_ConfigCommon->choice.setup;
    
    if (!rach_ConfigCommon) {
        return 0;
    }
    
    uint8_t config_index = rach_ConfigCommon->rach_ConfigGeneric.prach_ConfigurationIndex;
    const int ul_mu = cc->ServingCellConfigCommon->uplinkConfigCommon->frequencyInfoUL->scs_SpecificCarrierList.list.array[0]->subcarrierSpacing;
    frequency_range_t freq_range = get_freq_range_from_arfcn(
        cc->ServingCellConfigCommon->downlinkConfigCommon->frequencyInfoDL->absoluteFrequencyPointA);
    
    uint16_t RA_sfn_index = 0;
    
    // Use get_nr_prach_sched_from_info to check if this is a PRACH slot
    if (get_nr_prach_sched_from_info(cc->prach_info, config_index, frame, slot, 
                                     ul_mu, freq_range, &RA_sfn_index, cc->frame_type)) {
        return 1;  // This is a PRACH slot
    }
    
    return 0;  // Not a PRACH slot
}

#ifdef __cplusplus
}
#endif
void gNB_trigger_tx_window(void);
// ---------------------------------------------------------------------
// Global State
// ---------------------------------------------------------------------
static nru_cfg_t nru_cfg_global;
static nru_fbe_cfg_t fbe_cfg_global;
static bool nru_initialized = false;

// global gNB pointer (linked by MAC init)
void *global_gNB_ptr = NULL;

// ---------------------------------------------------------------------
// Time utility
// ---------------------------------------------------------------------
uint64_t nru_time_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

// ---------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------
int nru_lbt_init(const nru_cfg_t *cfg) {
    if (!cfg) {
        LOG_E(MAC, "[NRU] NULL configuration\n");
        return -1;
    }

    nru_cfg_global = *cfg;
    nru_set_ed_threshold((float)cfg->ed_threshold_dbm);

    if (strcmp(cfg->mode, "FBE") == 0) {
        fbe_cfg_global.T_frame_us = cfg->frame_period_ms * 1000;
        fbe_cfg_global.T_on_us    = cfg->tx_window_ms * 1000;
        fbe_cfg_global.duty.max_duty = cfg->duty_cycle_percent / 100.0;
        fbe_cfg_global.gnb_id = 0;
        fbe_cfg_global.log_level = 1;
        fbe_cfg_global.start_time_us = nru_time_now_us();
        printf("[NRU][FBE] %.2f ms frame | %.2f ms TX window | duty %.1f%%\n",
               fbe_cfg_global.T_frame_us/1000.0,
               fbe_cfg_global.T_on_us/1000.0,
               fbe_cfg_global.duty.max_duty*100.0);
    }

    nru_calibrate_noise_floor(400);
    nru_initialized = true;
    return 0;
}

// ---------------------------------------------------------------------
// TX Trigger Integration (for NR-U coexistence)
// ---------------------------------------------------------------------



static int consecutive_free = 0;
static const int FREE_TRIGGER_THRESHOLD = 3; // require 3 consecutive FREE detections

void nru_lbt_try_trigger_tx(void) {
    if (!nru_initialized || !global_gNB_ptr)
        return;

    float energy = nru_get_current_energy_dbm();
    float threshold = (float)nru_cfg_global.ed_threshold_dbm;
    bool free = (energy < threshold);

    if (free)
        consecutive_free++;
    else
        consecutive_free = 0;

   if (consecutive_free >= FREE_TRIGGER_THRESHOLD) {
    consecutive_free = 0;
    nru_stop_rx_stream();
    usleep(1000);

    LOG_I(MAC, "[NRU][LBT] 🚀 Channel FREE — calling gNB_trigger_tx_window()\n");
    gNB_trigger_tx_window();

    nru_lbt_on_tx_complete();
}

}

// ---------------------------------------------------------------------
// Sensing + Decision Engine
// ---------------------------------------------------------------------
int nru_lbt_sense_and_acquire(int gnb_id, int required_us) {
    if (!nru_initialized || !nru_cfg_global.enabled)
        return 1;

    // === FBE Mode ===
    if (strcmp(nru_cfg_global.mode, "FBE") == 0) {
        uint64_t now = nru_time_now_us();
        uint64_t off = now % fbe_cfg_global.T_frame_us;
        bool tx_ok = (off < fbe_cfg_global.T_on_us);

        if (tx_ok) {
            nru_stop_rx_stream();
            usleep(500); // guard
        } else {
            nru_restart_rx_stream();
        }

        if (nru_cfg_global.log_lbt)
            LOG_I(MAC, "[NRU][FBE] offset=%.2fms TX=%s\n", off/1000.0, tx_ok?"":"");
        return tx_ok;
    }

    // === LBE Mode ===
    float energy = nru_get_current_energy_dbm();
    float threshold = (float)nru_cfg_global.ed_threshold_dbm;
    bool free = (energy < threshold);

    if (nru_cfg_global.log_lbt) {
        LOG_I(MAC, "[NRU][LBE] Energy %.2f dBm | Thresh %.2f | %s\n",
              energy, threshold, free?" FREE":" BUSY");
    }

    // Try to trigger TX when channel is repeatedly free
   // nru_lbt_try_trigger_tx();

    int retries = 0;
    const int max_retries = (nru_cfg_global.mcot_ms * 1000 / nru_cfg_global.ed_sensing_time_us);
    while (!free && retries < max_retries) {
        usleep(nru_cfg_global.ed_sensing_time_us);
        energy = nru_get_current_energy_dbm();
        free = (energy < threshold);
        retries++;
    }

    if (free || retries >= max_retries) {
        nru_stop_rx_stream();
        usleep(1000);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------
// TX lifecycle (called from scheduler)
// ---------------------------------------------------------------------
void nru_lbt_on_tx_complete(void) {
    usleep(2000);
    nru_restart_rx_stream();
    if (nru_cfg_global.log_lbt)
        LOG_I(MAC, "[NRU] TX complete → RX resumed\n");
}

// ---------------------------------------------------------------------
// CSV Logging
// ---------------------------------------------------------------------
static void nru_log_csv(float energy, float threshold, bool free, const char *mode) {
    static FILE *f = NULL;
    if (!f) {
        mkdir("/tmp/nru_logs", 0777);
        f = fopen("/tmp/nru_logs/lbt_log.csv", "w");
        if (f)
            fprintf(f, "timestamp_us,energy_dbm,threshold_dbm,status,mode\n");
    }
    if (!f) return;
    fprintf(f, "%llu,%.2f,%.2f,%s,%s\n",
            (unsigned long long)nru_time_now_us(), energy, threshold,
            free?"FREE":"BUSY", mode);
    fflush(f);
}

// ---------------------------------------------------------------------
// Periodic duty heartbeat for FBE
// ---------------------------------------------------------------------
void nru_fbe_heartbeat(void) {
    if (strcmp(nru_cfg_global.mode, "FBE") != 0) return;
    uint64_t now = nru_time_now_us();
    bool tx_allowed = ((now % fbe_cfg_global.T_frame_us) < fbe_cfg_global.T_on_us);
    if (tx_allowed) nru_stop_rx_stream(); else nru_restart_rx_stream();
}

// ---------------------------------------------------------------------
// Config accessors
// ---------------------------------------------------------------------
const nru_cfg_t* nru_get_cfg(void) { return &nru_cfg_global; }

int nru_lbt_update_cfg(const nru_cfg_t *cfg) {
    if (!cfg) return -1;
    nru_cfg_global = *cfg;
    return 0;
}

// ---------------------------------------------------------------------
// Process USRP samples (optional hook)
// ---------------------------------------------------------------------
int nru_lbt_process_usrp_samples(void *samples, int len) {
    if (!samples || len <= 0) return -1;

    float energy = nru_get_current_energy_dbm();
    float threshold = (float)nru_cfg_global.ed_threshold_dbm;
    bool free = (energy < threshold);

    if (nru_cfg_global.log_lbt) {
        LOG_I(MAC, "[NRU][LBT] Sample window %d | Energy %.2f dBm | Thresh %.2f | %s\n",
              len, energy, threshold, free ? " FREE" : "BUSY");
    }

    if (free)
        nru_stop_rx_stream();
    else
        nru_restart_rx_stream();

    return 0;
}

// ---------------------------------------------------------------------
// Threshold update helper
// ---------------------------------------------------------------------
int nru_update_ed_threshold(float new_threshold_dbm) {
    nru_cfg_global.ed_threshold_dbm = (int)new_threshold_dbm;
    nru_set_ed_threshold(new_threshold_dbm);
    LOG_I(MAC, "[NRU] ED threshold updated to %.2f dBm\n", new_threshold_dbm);
    return 0;
}
// ---------------------------------------------------------------------
// TX window trigger integration (bridge to OAI scheduler)
// ---------------------------------------------------------------------
// ---------------------------------------------------------------------
// TX window trigger integration (bridge to OAI scheduler)
// ---------------------------------------------------------------------
void gNB_trigger_tx_window(void) {
    // Stub: Manual TX triggering disabled
    // The normal scheduler will handle TX when channel is free
    LOG_D(MAC, "[NRU][TX] Manual TX trigger bypassed - using normal scheduler flow\n");
    return;
}
