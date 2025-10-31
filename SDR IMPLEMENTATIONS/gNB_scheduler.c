/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file gNB_scheduler.c
 * \brief gNB scheduler top level function operates on per subframe basis
 * \author Navid Nikaein, Raymond Knopp, WEI-TAI CHEN
 * \date 2010-2025
 * \version 1.0
 */
#include "NR_MAC_COMMON/nr_mac_common.h"
#include "../MAC/mac_extern.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"

//#include "NR_MAC_gNB/nr_ue_scheduler.h"


#include "assertions.h"
#include "NR_MAC_gNB/mac_proto.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "common/utils/LOG/vcd_signal_dumper.h"
#include "UTIL/OPT/opt.h"
#include "openair2/X2AP/x2ap_eNB.h"
#include "nr_pdcp/nr_pdcp_oai_api.h"
#include "intertask_interface.h"
#include "executables/softmodem-common.h"
#include "nfapi/oai_integration/vendor_ext.h"
#include "executables/nr-softmodem.h"
//#include "nru_lbt.h"
#include "common/ran_context.h" // For MAX_NUM_CCs
#include "common/utils/nru_lbt.h"

#ifndef MAX_NUM_CCs
#define MAX_NUM_CCs 1
#endif
// --- NR-U and BCH forward declarations ---

extern int nru_lbt_sense_and_acquire(int gnb_id, int required_us);

void schedule_nr_mib(module_id_t module_idP,
                     frame_t frameP,
                     slot_t slotP,
                     nfapi_nr_dl_tti_request_t *DL_req);

/* ---------------------------------------------------------------------- */
/*                         NR-U Scheduler Integration                     */
/* ---------------------------------------------------------------------- *//* ----------------------------------------------------------
 *  Dummy UE creation for NR-U PHY-test / standalone runs
 * ---------------------------------------------------------- */

// ------------------------------------------------------------
// Create a dummy UE for PHY-test or NR-U standalone operation
// ------------------------------------------------------------
static void nru_create_dummy_ue(module_id_t module_idP)
{
    static bool dummy_ue_created = false;
    
    // Only create once to avoid repeated checks and log spam
    if (dummy_ue_created)
        return;

    gNB_MAC_INST *gNB = RC.nrmac[module_idP];
    if (!gNB)
        return;

    NR_UEs_t *UE_info = &gNB->UE_info;
    rnti_t rnti = 0x1234;

    // Check if the dummy UE already exists
    if (find_nr_UE(UE_info, rnti) != NULL) {
        LOG_I(MAC, "[NRU][DUMMY-UE] UE 0x%04x already exists\n", rnti);
        dummy_ue_created = true;
        return;
    }

    // Use the proper UE creation function - pass NULL for CellGroup (PHY test mode)
    NR_UE_info_t *ue = get_new_nr_ue_inst(&UE_info->uid_allocator, rnti, NULL);
    if (!ue) {
        LOG_E(MAC, "[NRU][DUMMY-UE] ❌ Failed to create UE instance\n");
        return;
    }

    // CRITICAL: For connected UEs (PHY test), ra must be NULL
    // Free the RA structure that get_new_nr_ue_inst() allocated
    if (ue->ra) {
        free(ue->ra);
        ue->ra = NULL;
    }
    
    // Initialize statistics
    ue->mac_stats.dl.current_bytes = 0;
    ue->mac_stats.ul.current_bytes = 0;
    
    // Add UE to the connected list
    bool result = add_connected_nr_ue(gNB, ue);
    if (!result) {
        LOG_W(MAC, "[NRU][DUMMY-UE] ❌ Failed to add dummy UE to connected list\n");
        // Note: add_connected_nr_ue already handles cleanup on failure
        dummy_ue_created = true;
        return;
    }

    dummy_ue_created = true;
    LOG_I(MAC, "[NRU][DUMMY-UE] ✅ Created dummy UE (RNTI=0x%04x, UID=%d)\n", rnti, ue->uid);
}


static void copy_ul_tti_req(nfapi_nr_ul_tti_request_t *to, nfapi_nr_ul_tti_request_t *from)
{
    to->header = from->header;
    to->SFN = from->SFN;
    to->Slot = from->Slot;
    to->n_pdus = from->n_pdus;
    to->rach_present = from->rach_present;
    to->n_ulsch = from->n_ulsch;
    to->n_ulcch = from->n_ulcch;
    to->n_group = from->n_group;

    for (int i = 0; i < from->n_pdus; i++) {
        to->pdus_list[i].pdu_type = from->pdus_list[i].pdu_type;
        to->pdus_list[i].pdu_size = from->pdus_list[i].pdu_size;
        switch (from->pdus_list[i].pdu_type) {
            case NFAPI_NR_UL_CONFIG_PRACH_PDU_TYPE:
                to->pdus_list[i].prach_pdu = from->pdus_list[i].prach_pdu;
                break;
            case NFAPI_NR_UL_CONFIG_PUSCH_PDU_TYPE:
                to->pdus_list[i].pusch_pdu = from->pdus_list[i].pusch_pdu;
                break;
            case NFAPI_NR_UL_CONFIG_PUCCH_PDU_TYPE:
                to->pdus_list[i].pucch_pdu = from->pdus_list[i].pucch_pdu;
                break;
            case NFAPI_NR_UL_CONFIG_SRS_PDU_TYPE:
                to->pdus_list[i].srs_pdu = from->pdus_list[i].srs_pdu;
                break;
        }
    }

    for (int i = 0; i < from->n_group; i++)
        to->groups_list[i] = from->groups_list[i];
}

uint8_t nr_get_rv(int rel_round)
{
  const uint8_t nr_rv_round_map[4] = {0, 2, 3, 1};
  AssertFatal(rel_round < 4, "Invalid index %d for rv\n", rel_round);
  return nr_rv_round_map[rel_round];
}

void clear_nr_nfapi_information(gNB_MAC_INST *gNB,
                                int CC_idP,
                                frame_t frameP,
                                slot_t slotP,
                                nfapi_nr_dl_tti_request_t *DL_req,
                                nfapi_nr_tx_data_request_t *TX_req,
                                nfapi_nr_ul_dci_request_t *UL_dci_req)
{
  const int num_slots = gNB->frame_structure.numb_slots_frame;
  UL_tti_req_ahead_initialization(gNB, num_slots, CC_idP, frameP, slotP);

  nfapi_nr_dl_tti_pdcch_pdu_rel15_t **pdcch =
      (nfapi_nr_dl_tti_pdcch_pdu_rel15_t **)gNB->pdcch_pdu_idx[CC_idP];

  gNB->pdu_index[CC_idP] = 0;
  DL_req[CC_idP].SFN = frameP;
  DL_req[CC_idP].Slot = slotP;
  DL_req[CC_idP].dl_tti_request_body.nPDUs = 0;
  DL_req[CC_idP].dl_tti_request_body.nGroup = 0;
  memset(pdcch, 0, sizeof(*pdcch) * MAX_NUM_CORESET);

  UL_dci_req[CC_idP].SFN = frameP;
  UL_dci_req[CC_idP].Slot = slotP;
  UL_dci_req[CC_idP].numPdus = 0;

  const int size = gNB->UL_tti_req_ahead_size;
  const int prev_slot = frameP * num_slots + slotP + size - 1;
  nfapi_nr_ul_tti_request_t *future_ul_tti_req =
      &gNB->UL_tti_req_ahead[CC_idP][prev_slot % size];
  future_ul_tti_req->SFN = (prev_slot / num_slots) % 1024;

  for (int i = 0; i < future_ul_tti_req->n_pdus; i++) {
    future_ul_tti_req->pdus_list[i].pdu_type = 0;
    future_ul_tti_req->pdus_list[i].pdu_size = 0;
  }

  future_ul_tti_req->n_pdus = 0;
  future_ul_tti_req->n_ulsch = 0;
  future_ul_tti_req->n_ulcch = 0;
  future_ul_tti_req->n_group = 0;
  TX_req[CC_idP].Number_of_PDUs = 0;
}

static void clear_beam_information(NR_beam_info_t *beam_info,
                                   int frame, int slot, int slots_per_frame)
{
  if (beam_info->beam_mode == NO_BEAM_MODE)
    return;

  int idx_to_clear =
      (frame * slots_per_frame + slot) / beam_info->beam_duration;
  idx_to_clear =
      (idx_to_clear + beam_info->beam_allocation_size - 1) %
      beam_info->beam_allocation_size;

  if (slot % beam_info->beam_duration == 0) {
    LOG_D(NR_MAC, "%d.%d Clear beam information for index %d\n",
          frame, slot, idx_to_clear);
    for (int i = 0; i < beam_info->beams_per_period; i++)
      beam_info->beam_allocation[i][idx_to_clear] = -1;
  }
}

/* ---------------------------------------------------------------------- */
/*                 Main scheduling loop — NR-U integrated                 */
/* ---------------------------------------------------------------------- */
void gNB_dlsch_ulsch_scheduler(module_id_t module_idP,
                               frame_t frame,
                               slot_t slot,
                               NR_Sched_Rsp_t *sched_info)
{
  protocol_ctxt_t ctxt = {0};
  PROTOCOL_CTXT_SET_BY_MODULE_ID(&ctxt, module_idP,
                                 ENB_FLAG_YES, NOT_A_RNTI,
                                 frame, slot, module_idP);

  gNB_MAC_INST *gNB = RC.nrmac[module_idP];

  if (get_softmodem_params()->phy_test)
      nru_create_dummy_ue(module_idP);
  NR_COMMON_channels_t *cc = gNB->common_channels;
  NR_ServingCellConfigCommon_t *scc = cc->ServingCellConfigCommon;

  NR_SCHED_LOCK(&gNB->sched_lock);

  /* ---------------------------------------------------------- */
  /*                NR-U Coexistence Integration                */
  /* ---------------------------------------------------------- */
  const nru_cfg_t *cfg = nru_get_cfg();
  bool channel_free = true;
  if (cfg && cfg->enabled) {
    bool is_prach = nr_is_prach_slot(module_idP, frame, slot);
    if (is_prach) {
        // 🟢 Bypass LBT during PRACH RX/TX occasions
        LOG_D(MAC, "[NRU][LBT] PRACH slot %d.%d → bypass sensing\n", frame, slot);
        channel_free = true;
    } else if (strcmp(cfg->mode, "LBE") == 0) {
        int sense_result = nru_lbt_sense_and_acquire(module_idP, 1000);
        channel_free = (sense_result == 1);
    } else if (strcmp(cfg->mode, "FBE") == 0) {
        nru_fbe_heartbeat();
    }
}


  if (!channel_free) {
    LOG_I(MAC,
          "[NRU][SCHED] Frame %d Slot %d: Channel BUSY → skip DL/UL scheduling\n",
          frame, slot);
    NR_SCHED_UNLOCK(&gNB->sched_lock);
    return;
  }

  int slots_frame = gNB->frame_structure.numb_slots_frame;

  /* ----------------------------------------------------------
 * NR-U: Trigger SSB/BCH scheduling with Listen-Before-Talk
 * ---------------------------------------------------------- */
  if (IS_SA_MODE(get_softmodem_params())) {
    if (nru_lbt_sense_and_acquire(module_idP, -1)) {
        LOG_I(MAC,
              "[NRU][SSB] 🚀 Channel free - scheduling BCH/SSB (frame=%d, slot=%d)\n",
              frame, slot);
        schedule_nr_mib(module_idP, frame, slot, &sched_info->DL_req);

    } else {
        LOG_I(MAC,
              "[NRU][SSB] 🛑 Channel busy - skipping BCH/SSB (frame=%d, slot=%d)\n",
              frame, slot);
    }
}

  clear_beam_information(&gNB->beam_info, frame, slot, slots_frame);

  gNB->frame = frame;
  start_meas(&gNB->gNB_scheduler);
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(
      VCD_SIGNAL_DUMPER_FUNCTIONS_gNB_DLSCH_ULSCH_SCHEDULER, VCD_FUNCTION_IN);

  /* ============================================================
   * Standard OAI scheduling — executes only if channel is free
   * ============================================================ */

  for (int CC_id = 0; CC_id < MAX_NUM_CCs; CC_id++) {
    int num_beams = 1;
    if (gNB->beam_info.beam_mode != NO_BEAM_MODE)
      num_beams = gNB->beam_info.beams_per_period;

    for (int i = 0; i < num_beams; i++)
      memset(cc[CC_id].vrb_map[i], 0, sizeof(uint16_t) * MAX_BWP_SIZE);

    const int size = gNB->vrb_map_UL_size;
    const int prev_slot = frame * slots_frame + slot + size - 1;

    for (int i = 0; i < num_beams; i++) {
      uint16_t *vrb_map_UL = cc[CC_id].vrb_map_UL[i];
      memcpy(&vrb_map_UL[prev_slot % size * MAX_BWP_SIZE],
             &gNB->ulprbbl, sizeof(uint16_t) * MAX_BWP_SIZE);
    }

    clear_nr_nfapi_information(gNB, CC_id, frame, slot,
                               &sched_info->DL_req,
                               &sched_info->TX_req,
                               &sched_info->UL_dci_req);
  }

  bool wait_prach_completed =
      gNB->num_scheduled_prach_rx >= NUM_PRACH_RX_FOR_NOISE_ESTIMATE;

  if ((wait_prach_completed || get_softmodem_params()->phy_test) &&
      (slot == 0) && (frame & 127) == 0) {
    char stats_output[32656] = {0};
    dump_mac_stats(gNB, stats_output, sizeof(stats_output), true);
    LOG_I(NR_MAC, "Frame.Slot %d.%d\n%s\n", frame, slot, stats_output);
  }

  nr_measgap_scheduling(gNB, frame, slot);
  nr_mac_update_timers(module_idP, frame, slot);

  if ((wait_prach_completed || get_softmodem_params()->phy_test)) {
    schedule_nr_mib(module_idP, frame, slot, &sched_info->DL_req);

    if (IS_SA_MODE(get_softmodem_params())) {
      schedule_nr_sib1(module_idP, frame, slot,
                       &sched_info->DL_req, &sched_info->TX_req);
      schedule_nr_other_sib(module_idP, frame, slot,
                            &sched_info->DL_req, &sched_info->TX_req);
    }
  }

  if (get_softmodem_params()->phy_test == 0) {
    const int n_slots_ahead = slots_frame - cc->prach_len +
                              get_NTN_Koffset(scc);
    const frame_t f =
        (frame + (slot + n_slots_ahead) / slots_frame) % 1024;
    const slot_t s = (slot + n_slots_ahead) % slots_frame;
    schedule_nr_prach(module_idP, f, s);
  }

  nr_csirs_scheduling(module_idP, frame, slot, &sched_info->DL_req);
  nr_csi_meas_reporting(module_idP, frame, slot);
  nr_schedule_srs(module_idP, frame, slot);

  if (get_softmodem_params()->phy_test == 0) {
    nr_schedule_RA(module_idP, frame, slot,
                   &sched_info->UL_dci_req,
                   &sched_info->DL_req,
                   &sched_info->TX_req);
  }

  start_meas(&gNB->schedule_ulsch);
  nr_schedule_ulsch(module_idP, frame, slot, &sched_info->UL_dci_req);
  stop_meas(&gNB->schedule_ulsch);

  start_meas(&gNB->schedule_dlsch);
  nr_schedule_ue_spec(module_idP, frame, slot,
                      &sched_info->DL_req, &sched_info->TX_req);
  stop_meas(&gNB->schedule_dlsch);

  /* -------------------------------------------------------
   * NR-U Throughput Logging (for adaptive coexistence)
   * ------------------------------------------------------ */
  NR_UEs_t *UE_info = &gNB->UE_info;
  if (UE_info->connected_ue_list == NULL) {
      LOG_I(MAC, "[NRU][THROUGHPUT] No active UEs — waiting for connection...\n");
  }

  UE_iterator(UE_info->connected_ue_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_mac_stats_t *stats = &UE->mac_stats;

    LOG_I(MAC,
          "[NRU][THROUGHPUT] Frame %d Slot %d UE RNTI=0x%04x: DL %.2f Mbit/s | UL %.2f Mbit/s\n",
          frame, slot, UE->rnti,
          (float)(stats->dl.current_bytes * 8e-6),
          (float)(stats->ul.current_bytes * 8e-6));
  }

  nr_sr_reporting(gNB, frame, slot);
  nr_schedule_pucch(gNB, frame, slot);

  AssertFatal(MAX_NUM_CCs == 1, "only 1 CC supported\n");
  const int current_index =
      ul_buffer_index(frame, slot, slots_frame,
                      gNB->UL_tti_req_ahead_size);
  copy_ul_tti_req(&sched_info->UL_tti_req,
                  &gNB->UL_tti_req_ahead[0][current_index]);

  stop_meas(&gNB->gNB_scheduler);
  NR_SCHED_UNLOCK(&gNB->sched_lock);
  VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME(
      VCD_SIGNAL_DUMPER_FUNCTIONS_gNB_DLSCH_ULSCH_SCHEDULER,
      VCD_FUNCTION_OUT);
}
