import pandas as pd
import seaborn as sns
import matplotlib.pyplot as plt
import os

# Set professional styling
sns.set_theme(style="whitegrid")
plt.rcParams['figure.dpi'] = 150

def analyze_mutex_scaling(filename):
    if not os.path.exists(filename):
        print(f"Skipping {filename}: File not found.")
        return

    df = pd.read_csv(filename)
    
    plt.figure(figsize=(10, 6))
    # Line plot showing mean latency with a 95% confidence interval shadow (from the trials)
    plot = sns.lineplot(
        data=df, 
        x='threads', 
        y='latency_ns_per_op', 
        hue='mutex', 
        marker='o',
        linewidth=2.5
    )
    
    plt.title('Mutex Performance Scaling under Contention', fontsize=16)
    plt.xlabel('Number of OS Threads', fontsize=12)
    plt.ylabel('Latency per Lock/Unlock (ns)', fontsize=12)
    plt.xticks(df['threads'].unique())
    
    # Save the plot
    plt.savefig('mutex_scaling.png')
    print("Generated: mutex_scaling.png")
    
    # Print summary statistics
    print("\n--- Mutex Throughput Summary (Average ns) ---")
    summary = df.groupby(['mutex', 'threads'])['latency_ns_per_op'].mean().unstack()
    print(summary)

def analyze_fiber_latency(filename):
    if not os.path.exists(filename):
        print(f"Skipping {filename}: File not found.")
        return

    df = pd.read_csv(filename)
    
    plt.figure(figsize=(8, 6))
    # Bar plot for context switch latency
    plot = sns.barplot(
        data=df, 
        x='system', 
        y='value_ns', 
        hue='system',    # Assign system to hue
        legend=False,    # Disable the redundant legend
        palette='viridis'
    )
    
    plt.title('Context Switch Latency: Fiber vs OS Thread', fontsize=16)
    plt.ylabel('Nanoseconds per Switch', fontsize=12)
    plt.xlabel('Synchronization System', fontsize=12)
    
    # Annotate bars with values
    for p in plot.patches:
        plot.annotate(format(p.get_height(), '.1f'), 
                       (p.get_x() + p.get_width() / 2., p.get_height()), 
                       ha = 'center', va = 'center', 
                       xytext = (0, 9), 
                       textcoords = 'offset points',
                       weight='bold')

    plt.savefig('fiber_latency.png')
    print("Generated: fiber_latency.png")
    
    # Calculate the speedup factor
    os_val = df[df['system'] == 'OSThread']['value_ns'].values[0]
    mag_val = df[df['system'] == 'MagFiber']['value_ns'].values[0]
    print(f"\n--- Fiber Advantage ---")
    print(f"MagFiber is {os_val / mag_val:.2f}x faster than OS Context Switching.")

if __name__ == "__main__":
    # Ensure the CSVs exist or notify the user
    analyze_mutex_scaling('mutex_scaling.csv')
    analyze_fiber_latency('fiber_latency.csv')
    plt.show()