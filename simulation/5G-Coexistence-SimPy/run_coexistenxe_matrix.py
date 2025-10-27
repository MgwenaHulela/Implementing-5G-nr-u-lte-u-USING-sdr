import click
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import seaborn as sns
from coexistanceSimpy.Coexistence import *

@click.command()
@click.option("-r", "--runs", default=3, help="Number of simulation runs per configuration")
@click.option("--seed", default=1, help="Starting seed for simulation")
@click.option("-t", "--simulation-time", default=10.0, help="Duration of the simulation in seconds")
@click.option("--wifi_cw_min", default=15, help="Size of Wi-Fi cw min")
@click.option("--wifi_cw_max", default=63, help="Size of Wi-Fi cw max")
@click.option("--nru_cw_min", default=15, help="Size of NR-U cw min")
@click.option("--nru_cw_max", default=63, help="Size of NR-U cw max")
@click.option("--wifi_r_limit", default=7, help="Number of failed Wi-Fi transmissions in a row")
@click.option("-m", "--mcs-value", default=7, help="Value of MCS")
@click.option("-syn_slot", "--synchronization_slot_duration", default=1000, help="Synchronization slot length in microseconds")
@click.option("-max_des", "--max_sync_slot_desync", default=1000, help="Max value of gNB desynchronization")
@click.option("-min_des", "--min_sync_slot_desync", default=0, help="Min value of gNB desynchronization")
@click.option("-nru_obser_slots", "--nru_observation_slot", default=3, help="Amount of observation slots for NR-U")
@click.option("--mcot", default=6, help="Max channel occupancy time for NR-U (ms)")
@click.option("--output", default="coexistence_matrix.csv", help="Output CSV file")
@click.option("--visualize/--no-visualize", default=True, help="Generate visualizations after simulation")
def run_coexistence_matrix(
    runs,
    seed,
    simulation_time,
    wifi_cw_min,
    wifi_cw_max,
    nru_cw_min,
    nru_cw_max,
    wifi_r_limit,
    mcs_value,
    synchronization_slot_duration,
    max_sync_slot_desync,
    min_sync_slot_desync,
    nru_observation_slot,
    mcot,
    output,
    visualize
):
    """Run simulations for a matrix of Wi-Fi APs (1-5) and NR-U gNBs (1-5)"""
    global output_csv
    output_csv = output
    
    # Create configurations
    wifi_config = Config(
        data_size=1472,
        cw_min=wifi_cw_min, 
        cw_max=wifi_cw_max,
        r_limit=wifi_r_limit,
        mcs=mcs_value
    )
    
    nru_config = Config_NR(
        deter_period=16,
        observation_slot_duration=9,
        synchronization_slot_duration=synchronization_slot_duration,
        max_sync_slot_desync=max_sync_slot_desync,
        min_sync_slot_desync=min_sync_slot_desync,
        M=nru_observation_slot,
        cw_min=nru_cw_min,
        cw_max=nru_cw_max,
        mcot=mcot
    )
    
    # Clear output file
    with open(output_csv, mode='w', newline='') as f:
        f.write("")  # Clear file contents
    
    # Run matrix of simulations
    print("Starting coexistence matrix simulation (1-5 APs × 1-5 gNBs)")
    print("=" * 60)
    
    total_configs = 5 * 5 * runs
    config_counter = 0
    
    for wifi_count in range(1, 6):
        for gnb_count in range(1, 6):
            print(f"\nRunning configuration: {wifi_count} Wi-Fi APs, {gnb_count} NR-U gNBs")
            
            for run in range(runs):
                config_counter += 1
                current_seed = seed + run
                progress = (config_counter / total_configs) * 100
                
                print(f"Progress: {progress:.1f}% - Run {run+1}/{runs}, Seed {current_seed}")
                
                # Initialize data structures for this run
                backoffs = {key: {wifi_count: 0} for key in range(wifi_cw_max + 1)}
                airtime_data = {f"Station {i}": 0 for i in range(1, wifi_count + 1)}
                airtime_control = {f"Station {i}": 0 for i in range(1, wifi_count + 1)}
                airtime_data_NR = {f"Gnb {i}": 0 for i in range(1, gnb_count + 1)}
                airtime_control_NR = {f"Gnb {i}": 0 for i in range(1, gnb_count + 1)}
                
                # Run simulation
                run_simulation(
                    wifi_count, gnb_count, current_seed, simulation_time,
                    wifi_config, nru_config,
                    backoffs, airtime_data, airtime_control,
                    airtime_data_NR, airtime_control_NR
                )
    
    print("\nSimulations complete!")
    
    # Generate visualizations if requested
    if visualize:
        print("Generating visualizations...")
        visualize_results(output)
    
    print(f"All done! Results saved to {output}")


def visualize_results(csv_file):
    """Generate heatmap visualizations from simulation results"""
    # Create output directory if it doesn't exist
    if not os.path.exists("results"):
        os.makedirs("results")
    
    # Read the CSV data
    df = pd.read_csv(csv_file)
    
    # Calculate average values for each AP/gNB combination
    matrix_data = df.groupby(['WiFi_Nodes', 'NRU_Nodes']).agg({
        'WiFi_Throughput': 'mean',
        'NRU_Throughput': 'mean',
        'Total_Throughput': 'mean',
        'WiFi_PLR': 'mean',
        'NRU_PLR': 'mean',
        'WiFi_Latency': 'mean', 
        'NRU_Latency': 'mean',
        'Traditional_Fairness': 'mean',
        'Jains_Fairness': 'mean',
        'WiFi_SINR': 'mean',
        'NRU_SINR': 'mean',
    }).reset_index()
    
    # Create pivot tables for each metric
    metrics = [
        ('WiFi_Throughput', 'Wi-Fi Throughput (Mbps)'),
        ('NRU_Throughput', 'NR-U Throughput (Mbps)'),
        ('Total_Throughput', 'Total System Throughput (Mbps)'),
        ('WiFi_PLR', 'Wi-Fi Packet Loss Rate'),
        ('NRU_PLR', 'NR-U Packet Loss Rate'),
        ('WiFi_Latency', 'Wi-Fi Latency (μs)'),
        ('NRU_Latency', 'NR-U Latency (μs)'),
        ('Traditional_Fairness', 'Traditional Fairness Index'),
        ('Jains_Fairness', 'Jain\'s Fairness Index'),
        ('WiFi_SINR', 'Wi-Fi SINR (dB)'),
        ('NRU_SINR', 'NR-U SINR (dB)')
    ]
    
    # Generate heatmaps for each metric
    for col_name, title in metrics:
        plt.figure(figsize=(8, 6))
        
        # Create pivot table
        pivot = matrix_data.pivot(index='WiFi_Nodes', columns='NRU_Nodes', values=col_name)
        
        # Determine appropriate colormap
        if 'Fairness' in col_name:
            cmap = 'viridis'  # Higher values are better
            vmin = 0
            vmax = 1
        elif 'SINR' in col_name:
            cmap = 'viridis'  # Higher values are better
            vmin = None
            vmax = None
        elif 'Latency' in col_name or 'PLR' in col_name:
            cmap = 'viridis_r'  # Lower values are better
            vmin = None
            vmax = None
        else:  # Throughput
            cmap = 'viridis'  # Higher values are better
            vmin = None
            vmax = None
        
        # Create heatmap
        ax = sns.heatmap(pivot, annot=True, fmt='.2f', cmap=cmap, 
                         vmin=vmin, vmax=vmax, cbar_kws={'label': col_name})
        
        # Set labels and title
        ax.set_title(title)
        ax.set_xlabel('Number of NR-U gNBs')
        ax.set_ylabel('Number of Wi-Fi APs')
        
        # Save figure
        plt.tight_layout()
        plt.savefig(f"results/{col_name}_heatmap.png", dpi=300)
        plt.close()
    
    # Create combined metric visualization (throughput vs fairness)
    plt.figure(figsize=(10, 8))
    plt.scatter(
        matrix_data['Total_Throughput'], 
        matrix_data['Jains_Fairness'], 
        s=100, alpha=0.7
    )
    
    # Add labels for each point
    for i, row in matrix_data.iterrows():
        plt.annotate(
            f"{int(row['WiFi_Nodes'])},{int(row['NRU_Nodes'])}",
            (row['Total_Throughput'], row['Jains_Fairness']),
            xytext=(5, 5),
            textcoords='offset points'
        )
    
    plt.title('System Throughput vs Fairness')
    plt.xlabel('Total Throughput (Mbps)')
    plt.ylabel('Jain\'s Fairness Index')
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig("results/throughput_vs_fairness.png", dpi=300)
    plt.close()


if __name__ == "__main__":
    run_coexistence_matrix()
