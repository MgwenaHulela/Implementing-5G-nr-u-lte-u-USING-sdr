import click
import os
import time
import csv
from coexistanceSimpy.Coexistence import *

# Define the scenarios we want to test
SCENARIOS = [
    {"name": "wifi_only", "description": "Wi-Fi only (no NR-U)", "gap_value": None},
    {"name": "nru_only", "description": "NR-U only (no Wi-Fi)", "gap_value": False},
    {"name": "coexistence_gap", "description": "Coexistence with Gap-based LBT", "gap_value": True},
    {"name": "coexistence_rs", "description": "Coexistence with Reservation Signal", "gap_value": False},
    {"name": "coexistence_adaptive", "description": "Coexistence with Dynamic CW Adjustment", "gap_value": False ,"dynamic_cw": True},  # NEW
    {"name": "coexistence_optimized", "description": "Coexistence with All Optimizations", "gap_value": True, "dynamic_cw": True, "disable_backoff": False}
]

def ensure_directory(directory_path):
    """Create directory if it doesn't exist"""
    if not os.path.exists(directory_path):
        try:
            os.makedirs(directory_path)
            print(f"Created directory: {directory_path}")
        except Exception as e:
            print(f"Error creating directory {directory_path}: {e}")
            raise

def create_csv_with_header(file_path):
    """Create a CSV file with appropriate headers if it doesn't exist"""
    # Define headers based on what we're collecting in run_simulation
    headers = [
        "Seed", "WiFi_Nodes", "NRU_Nodes", "WiFi_CW_Min", "WiFi_CW_Max", "NRU_CW_Min", "NRU_CW_Max",
        "WiFi_Throughput", "NRU_Throughput", "Total_Throughput",
        "WiFi_PLR", "NRU_PLR",
        "WiFi_Latency", "NRU_Latency",
        "WiFi_Access_Delay", "NRU_Access_Delay",
        "WiFi_SINR", "NRU_SINR",
        "Traditional_Fairness", "Jains_Fairness", "Joint_Metric",
        "WiFi_COT", "NRU_COT", "Total_COT",
        "WiFi_Efficiency", "NRU_Efficiency", "Total_Efficiency"
    ]
    
    # Create directory for the file if it doesn't exist
    file_dir = os.path.dirname(file_path)
    if file_dir and not os.path.exists(file_dir):
        ensure_directory(file_dir)
    
    # Create the file with headers if it doesn't exist or is empty
    if not os.path.exists(file_path) or os.path.getsize(file_path) == 0:
        try:
            with open(file_path, 'w', newline='') as csvfile:
                writer = csv.writer(csvfile)
                writer.writerow(headers)
            print(f"Created CSV file with headers: {file_path}")
        except Exception as e:
            print(f"Error creating CSV file {file_path}: {e}")
            raise

@click.command()
@click.option("-r", "--runs", default=10, help="Number of simulation runs per configuration")
@click.option("--seed-start", default=1, help="Starting seed for simulation")
@click.option("-t", "--simulation-time", default=30.00, help="Duration of the simulation in seconds")
@click.option("--output-dir", default="results", help="Directory to save results")
@click.option("--wifi-max", default=3, help="Maximum number of Wi-Fi nodes to simulate")
@click.option("--nru-max", default=3, help="Maximum number of NR-U nodes to simulate")
@click.option("--dynamic-cw", is_flag=True, default=False, help="Enable dynamic CW adjustment for fairness")
def test_all_modes(runs, seed_start, simulation_time, output_dir, wifi_max, nru_max,dynamic_cw):
    """Test all operation modes: Wi-Fi only, NR-U only, and coexistence with different LBT methods."""
    
    # Ensure output directory exists
    ensure_directory(output_dir)
    
    # Get global access to gap variable
    global gap
    global output_csv
    
    # Create configurations
    wifi_config = Config()
    nru_config = Config_NR()
    
    # Run all scenarios
    for scenario in SCENARIOS:
        print("\n" + "="*80)
        print(f"SCENARIO: {scenario['description']}")
        print("="*80)
        
        # Set the output file for this scenario
        output_file = os.path.join(output_dir, f"{scenario['name']}.csv")
        output_csv = output_file
        
        # Ensure CSV file exists with headers
        create_csv_with_header(output_csv)
        
        # Set gap mode if applicable
        original_gap = None
        if scenario['gap_value'] is not None:
            # Remember original gap value
            original_gap = gap
            # Set the gap mode for this scenario
            gap = scenario['gap_value']
            print(f"Gap mode set to: {gap}")
        scenario_dynamic_cw = scenario.get('dynamic_cw', False) or dynamic_cw
        # Determine node ranges based on scenario
        if scenario['name'] == 'wifi_only':
            wifi_range = range(1, wifi_max + 1)  # 1-N Wi-Fi nodes
            gnb_range = [0]  # No NR-U nodes
        elif scenario['name'] == 'nru_only':
            wifi_range = [0]  # No Wi-Fi nodes
            gnb_range = range(1, nru_max + 1)  # 1-N NR-U nodes
        elif 'wifi_fixed' in scenario and 'nru_fixed' in scenario:
            wifi_range = [scenario['wifi_fixed']]
            gnb_range = [scenario['nru_fixed']]
        else:
            wifi_range = range(1, wifi_max + 1)  # 1-N Wi-Fi nodes
            gnb_range = range(1, nru_max + 1)  # 1-N NR-U nodes
        
        # Calculate total configurations for progress reporting
        total_configs = len(list(wifi_range)) * len(list(gnb_range)) * runs
        config_counter = 0
        
        # Run the simulations for this scenario
        for wifi_count in wifi_range:
            for gnb_count in gnb_range:
                print(f"\nRunning configuration: {wifi_count} Wi-Fi APs, {gnb_count} NR-U gNBs")
                
                for run in range(runs):
                    config_counter += 1
                    # Use sequential seeds
                    current_seed = seed_start + run
                    progress = (config_counter / total_configs) * 100
                    
                    print(f"Progress: {progress:.1f}% - Run {run+1}/{runs}, Seed {current_seed}")
                    print(f"Simulation time: {simulation_time} seconds")
                    
                    # Initialize data structures for this run
                    backoffs = {key: {wifi_count if wifi_count > 0 else 0: 0} for key in range(wifi_config.cw_max + 1)}
                    airtime_data = {f"Station {i}": 0 for i in range(1, wifi_count + 1)} if wifi_count > 0 else {}
                    airtime_control = {f"Station {i}": 0 for i in range(1, wifi_count + 1)} if wifi_count > 0 else {}
                    airtime_data_NR = {f"Gnb {i}": 0 for i in range(1, gnb_count + 1)} if gnb_count > 0 else {}
                    airtime_control_NR = {f"Gnb {i}": 0 for i in range(1, gnb_count + 1)} if gnb_count > 0 else {}
                    
                    # Run simulation
                    start_time = time.time()
                    try:
                        run_simulation(
                            wifi_count, gnb_count, current_seed, simulation_time,
                            wifi_config, nru_config,
                            backoffs, airtime_data, airtime_control,
                            airtime_data_NR, airtime_control_NR,
                            enable_dynamic_cw=scenario_dynamic_cw ,
                            csv_output_path=output_file
                        )
                        end_time = time.time()
                        elapsed = end_time - start_time
                        print(f"Simulation completed in {elapsed:.2f} seconds (real time)")
                        print(f"Simulation speedup: {simulation_time/elapsed:.2f}x real-time")
                    except Exception as e:
                        print(f"Error in simulation: {e}")
                        print("Continuing with next configuration...")
        
        # Reset gap mode if it was changed
        if original_gap is not None:
            gap = original_gap
    





    print("\nAll simulations complete!")
    print(f"Results saved to the {output_dir} directory")

if __name__ == "__main__":
    test_all_modes()
