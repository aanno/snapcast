import json
import matplotlib.pyplot as plt
from matplotlib.widgets import CheckButtons
from mpl_interactions import zoom_factory, panhandler

# Configuration: path to pidstat JSON output file
# filename = "pidstat_output.json"  # update as needed
filename = "cpu.log"

# Data holders
time = []
metrics = {
    'CPU %usr': [],
    'CPU %system': [],
    'Memory %mem': [],
    'Disk kB_rd/s': [],
    'Disk kB_wr/s': []
}

# Extract metrics based on pidstat JSON structure; adjust keys if needed
def extract_metrics(entry):
    cpu = entry.get("cpu", {})
    mem = entry.get("mem", {})
    disk = entry.get("disk", {})
    
    metrics['CPU %usr'].append(float(cpu.get("%usr", 0)))
    metrics['CPU %system'].append(float(cpu.get("%system", 0)))
    metrics['Memory %mem'].append(float(mem.get("%mem", 0)))
    metrics['Disk kB_rd/s'].append(float(disk.get("kB_rd/s", 0)))
    metrics['Disk kB_wr/s'].append(float(disk.get("kB_wr/s", 0)))

with open(filename, "r") as f:
    # for idx, line in enumerate(f):
    #     if not line.strip():
    #         continue
    entry = json.load(f)
    time.append(idx)  # Simple incremental timing; modify if precise timestamps available
    extract_metrics(entry)

# Start plotting
fig, ax = plt.subplots(figsize=(10, 6))
ax.set_title("pidstat Metrics Over Time")
ax.set_xlabel("Time (intervals)")
ax.set_ylabel("Value")

lines = {}
colors = {
    'CPU %usr': 'blue',
    'CPU %system': 'green',
    'Memory %mem': 'red',
    'Disk kB_rd/s': 'purple',
    'Disk kB_wr/s': 'orange'
}

# Plot each metric initially visible
for key in metrics:
    lines[key], = ax.plot(time, metrics[key], label=key, color=colors[key])

ax.legend(loc='upper left')

# Add interactive zoom and pan from mpl_interactions
zoom = zoom_factory(ax)
pan = panhandler(fig)

# Setup CheckButtons for toggling line visibility
rax = plt.axes([0.01, 0.4, 0.12, 0.2])  # Position for checkboxes (left side)

labels = list(metrics.keys())
visibility = [lines[label].get_visible() for label in labels]

check = CheckButtons(rax, labels, visibility)

def toggle_visibility(label):
    lines[label].set_visible(not lines[label].get_visible())
    fig.canvas.draw_idle()

check.on_clicked(toggle_visibility)

plt.tight_layout()
plt.show()

