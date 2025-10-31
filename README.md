# Implementing 5G NR-U and LTE-U Using SDR  
**University of Cape Town – Final Year Engineering Project (2025)**  
**Author:** Innocent Nhlanhla Makhubela  
**Supervisor:** Prof. Joyce Mwangama  
**Institution:** University of Cape Town (UCT)  
**Repository:** [github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr](https://github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr)

---

## Overview
This project demonstrates 5G NR-U (New Radio in Unlicensed) and LTE-U (LTE-Unlicensed) communication over the 5 GHz unlicensed band using Software-Defined Radio (SDR) platforms.  
It aims to experimentally validate that 5G NR-U can coexist fairly with Wi-Fi—that is, operate without diminishing Wi-Fi performance.

The repository includes:
- Custom OpenAirInterface (OAI) extensions for NR-U Listen-Before-Talk (LBT)
- Python-based coexistence simulator (CoexistenceSimPy)
- Wi-Fi baseline logs and SDR measurement data
- Scripts, configurations, and documentation for replication

---

## Project Objectives
1. Implement and evaluate 5G NR-U channel access (Listen-Before-Talk) in unlicensed 5 GHz Band 46.  
2. Demonstrate fair coexistence with Wi-Fi networks through experimental validation.  
3. Build a software-hardware testbed using USRP B210 SDRs and open-source 5G/Wi-Fi stacks.  
4. Develop a Python simulation framework to analyze channel access fairness and throughput under controlled conditions.  
5. Provide open-source reference data and code for future research (for example, Master's-level extension).

---

## System Architecture

The experimental setup consists of four major subsystems:

1. **Wi-Fi Network** – Windows Hotspot (IEEE 802.11ac, Channel 36 @ 5.18 GHz)  
2. **5G NR-U gNB** – OpenAirInterface NR softmodem running custom LBT core  
3. **5G Core Network (5GC)** – Open5GS AMF/SMF/UPF services for SA mode registration  
4. **User Equipment (UE)** – Attempted SDR UE (OAI UE softmodem), currently unattachable due to Band 46 limitations

Wi-Fi baselines and NR-U transmissions share the same 20 MHz channel to test coexistence.

---

## Software Stack

| Component | Framework | Function |
|------------|------------|-----------|
| RAN | [OpenAirInterface 5G](https://gitlab.eurecom.fr/oai/openairinterface5g) | gNB/UE softmodem with NR-U LBT core |
| 5G Core | [Open5GS](https://open5gs.org) | AMF, SMF, UPF, UDM, etc. |
| Simulation | Python 3 + SimPy + Pandas + Matplotlib | Coexistence modeling |
| Wi-Fi Baseline | Windows Hotspot + iPerf3 | Throughput and latency testing |
| SDR Driver | UHD 4.9.0 (Ettus Research) | Hardware interface for USRP B210 |

---

## Custom NR-U Implementation

### Key Modules
- `nru_lbt.c/.cpp` – Listen-Before-Talk core integrated in OAI MAC-gNB  
- `nru_phy_helper.cpp` – Energy detection and RX sample processing  
- `/tmp/nru_logs/` – CSV outputs for CCA, LBT decisions, and TX records  

### Features
- ETSI EN 301 893-style energy detection  
- Random backoff with configurable CWmin/CWmax  
- Duty-cycle and MCOT control  
- Channel state classification (“BUSY” / “FREE”)  
- CSV logging and runtime plotting

---

## Hardware Testbed

| Device | Role | Description |
|---------|------|-------------|
| USRP B210 (1) | gNB | 5 GHz Band 46 transmitter running OAI NR-U |
| USRP B210 (2) | UE (planned) | Reserved for future UE attach tests |
| HP ZBook 14u G6 | Host PC | Ubuntu Linux for OAI and Python framework |
| Windows Laptop | Wi-Fi AP | Hotspot for baseline and coexistence tests |
| Android Phone + iPerf3 | Wi-Fi Client | Throughput measurement tool |

---

## Experimental Setup and Results
- Band: 5 GHz (Unlicensed, Channel 36, Center 5.18079 GHz)  
- Bandwidth: 20 MHz, μ = 1, NRB = 51  
- Wi-Fi baseline: 34–38 Mbit/s TCP, ~1 Mbit/s UDP with < 1.2 % loss  
- NR-U LBT: Validated channel-free transmissions below –60 dBm  
- Fairness metric: Jain’s Index ≈ 0.97–0.99 (No Wi-Fi degradation)  
- UE attach: Unsuccessful in SA mode (Band 46 not fully supported)  

All measured data and plots are in:  
