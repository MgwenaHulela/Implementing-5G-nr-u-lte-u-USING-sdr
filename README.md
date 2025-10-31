# 5G New Radio Communication over Wi-Fi 5 GHz Unlicensed Band using an SDR Platfor 
**University of Cape Town – Final Year Engineering Project (2025)**  
**Author:** Innocent Nhlanhla Makhubela  
**Supervisor:** Prof. Joyce Mwangama  
**Institution:** University of Cape Town (UCT)  
**Repository:** [github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr](https://github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr)

---

## Overview
This project demonstrates 5G NR-U (New Radio in Unlicensed) communication over the 5 GHz unlicensed band using Software-Defined Radio (SDR) platforms.  
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
- Channel state classification ("BUSY" / "FREE")  
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
- Fairness metric: Jain's Index ≈ 0.97–0.99 (No Wi-Fi degradation)  
- UE attach: Unsuccessful in SA mode (Band 46 not fully supported)  

All measured data and plots are in:  
```
results/

```

---

## Installation Guide

### Prerequisites
- Ubuntu 20.04 LTS or later  
- GNU Radio 3.10+  
- UHD 4.9.0 (driver for USRP)  
- Python 3.8+ with Pandas, SimPy, Matplotlib  
- iPerf3 (for throughput tests)  
- Root access for real-time threads and USB3 drivers  

### Steps
```bash
sudo apt update
sudo apt install -y git build-essential cmake python3-pip gnuradio uhd-host
pip install pandas simpy matplotlib
git clone https://github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr.git
cd Implementing-5G-nr-u-lte-u-USING-sdr
```

Build OAI and Open5GS as per documentation in `docs/setup_guides/`.

---

## Directory Structure

```
Implementing-5G-nr-u-lte-u-USING-sdr/
│
├── oai_mods/                 # Custom NR-U source (LBT, ED, logging)
├── simulation/               # CoexistenceSimPy framework
├── results/                  # Wi-Fi & NR-U measurements
├── docs/                     # Thesis sections, ethics forms, poster
├── scripts/                  # Analysis & plotting tools
├── configs/                  # OAI & Open5GS configs
├── figures/                  # Diagrams, spectra, poster images
└── README.md
```

---

## Running the Experiments

### 1. Wi-Fi Baseline

```bash
# On Windows hotspot:
iperf3 -s
# On client:
iperf3 -c <Windows-IP> -n 500M -i 1
```

### 2. NR-U gNB (LBT Active)

```bash
cd oai_mods/
sudo ./nr-softmodem -O configs/gnb.nru.band46.conf --sa -E
```

### 3. Data Collection

Logs and energy detections are stored under `/tmp/nru_logs/` and parsed via:

```bash
python3 scripts/parse_logs.py
```

---

## Sample Results

| Test Case | Scenario                   | Throughput (Mbps) | Jain's Index         |
| --------- | -------------------------- | ----------------- | -------------------- |
| T1        | Wi-Fi only                 | 36.7              | 1.000                |
| T2        | Wi-Fi + NR-U (TCP 200 MB)  | 34.9              | 0.986                |
| T3        | Wi-Fi + NR-U (TCP 1 GB)    | 33.5              | 0.969                |
| T4        | NR-U only (LBT verify)     | –                 | –                    |
| T6        | NR-U data transfer attempt | –                 | – (failed UE attach) |

---

## Known Limitations

* Only one SDR used for gNB (transmit tests only).
* UE attach not achieved due to Band 46 hardware support gaps.
* Limited runtime for field tests (academic time constraints).
* Open-source OAI may not fully replicate commercial 5G features.

---

## Ethics and AI Usage

* The project complied with UCT research ethics policies.
* AI tools (ChatGPT) were used only for debugging C++/Python code and grammar improvement.
* All source files and results are available for replication and transparency.
* No proprietary data was used.

---

## Citation and BibTeX

If you use this work or code, please cite as:

> I.N. Makhubela, "Achieving 5G NR-U Communication over Wi-Fi 5 GHz Unlicensed Band Using an SDR Platform," University of Cape Town, 2025.

### BibTeX

```bibtex
@thesis{makhubela2025nru,
  author    = {Innocent Nhlanhla Makhubela},
  title     = {Achieving 5G NR-U Communication over Wi-Fi 5 GHz Unlicensed Band Using an SDR Platform},
  school    = {University of Cape Town},
  year      = {2025},
  note      = {Supervisor: Prof. Joyce Mwangama}
}
```

---

## Contributing

Contributions are welcome.
Please fork the repository and submit pull requests for:

* Code optimizations (for example, NR-U LBT core)
* New simulation scenarios or fairness metrics
* Improved logging and visualization tools
* Additional documentation or setup guides

Steps:

```bash
git checkout -b feature-youridea
git commit -m "Added feature-youridea"
git push origin feature-youridea
```

Then open a pull request on GitHub.

---

## Acknowledgements

"Munhu i munhu hi vanhu – A person is a person through other people."

* Prof. Joyce Mwangama – Supervisor, University of Cape Town
* Gcinumzi Haduse – Line manager and mentor
* Rockete Ngoepe, TraviMadox, Maisha, Talifhani, Zwivhuya, Gingirikani, AA Hlantshwayo – Support and encouragement
* OpenAirInterface & Open5GS Communities – for open-source tools
* UCT Department of Electrical Engineering – for lab access and equipment

---

## Contact

**Author:** Innocent Nhlanhla Makhubela  
**Email:** [mkhinn011@uct.ac.za](mailto:mkhinn011@uct.ac.za)  
**GitHub:** [@MgwenaHulela](https://github.com/MgwenaHulela)  
**Supervisor:** Prof. Joyce Mwangama – University of Cape Town

---

## Future Work (Planned Master's Extension)

* Full NR-U and Wi-Fi coexistence test with two USRP nodes
* Integration of 6 GHz Wi-Fi 6E/7 spectrum for advanced fairness testing
* Machine-learning driven dynamic LBT parameter adaptation
* Multi-cell simulation and cross-layer optimization

---

## License

This project is licensed under the MIT License – see LICENSE for details.
Feel free to reuse, modify, and share with proper attribution.

---

## Quick Start

```bash
# Clone repository
git clone https://github.com/MgwenaHulela/Implementing-5G-nr-u-lte-u-USING-sdr.git
cd Implementing-5G-nr-u-lte-u-USING-sdr

# Install dependencies
sudo apt install gnuradio uhd-host python3-pip
pip install pandas simpy matplotlib

# Run simulation
cd simulation
python3 coexistence_sim.py
```

---

If you find this project useful, please star the repository and share it with other wireless researchers.
