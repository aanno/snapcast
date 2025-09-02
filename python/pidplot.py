import json
import matplotlib.pyplot as plt
from matplotlib.widgets import CheckButtons
from mpl_interactions import zoom_factory, panhandler

# Configuration: path to pidstat JSON output file
# filename = "pidstat_output.json"  # update as needed
filename = "cpu.log"

    
def extract(records, key, subkey):
    return [item[key][0][subkey] for item in records]

def read_json(filename):
    with open(filename, "r") as f:
        entry = json.load(f)
        list = entry['sysstat']['hosts'][0]['statistics']
        return list
        
def normalize(list):
    low = min(list)
    up = max(list)
    if (low >= 0 and up > 0):
        return ([item/up for item in list], up)
    else:
        return (list, 1)
        
record = read_json(filename)

# Extract data lists from nested JSON structure
timestamps = [rec['timestamp'] for rec in record]

cpu_usr, cpu_usr_up = normalize(extract(record, 'task-cpu-load', 'usr'))
cpu_system, cpu_system_up = normalize(extract(record, 'task-cpu-load', 'system'))
stack_size, stack_size_up = normalize(extract(record, 'stack', 'StkSize'))
mem_usage, mem_usage_up = normalize(extract(record, 'task-memory', 'MEM'))
disk_rd, disk_rd_up = normalize(extract(record, 'io', 'kB_rd/s'))
disk_wr, disk_wr_up = normalize(extract(record, 'io', 'kB_wr/s'))

# For plotting, we'll use indices as x-axis (interval count)
time = list(range(len(timestamps)))

metrics = {
    f'CPU usr % {cpu_usr_up}': cpu_usr,
    f'CPU system % {cpu_system_up}': cpu_system,
    f'Stack Size {stack_size_up}': stack_size,
    f'Memory Usage % {mem_usage_up}': mem_usage,
    f'Disk Read (kB/s) {disk_rd_up}': disk_rd,
    f'Disk Write (kB/s) {disk_wr_up}': disk_wr
}

# Prepare the plot
fig, ax = plt.subplots(figsize=(12, 7))
ax.set_title("pidstat Metrics Over Time")
ax.set_xlabel("Sample Intervals")
ax.set_ylabel("Metric Values")

lines = {}
colors = ['blue', 'green', 'red', 'purple', 'orange', 'brown']

for i, (label, values) in enumerate(metrics.items()):
    # Replace None with 0 or suitable default for plotting
    safe_values = [v if v is not None else 0 for v in values]
    line, = ax.plot(time, safe_values, label=label, color=colors[i % len(colors)])
    lines[label] = line

ax.legend(loc='upper left')

# Add mpl_interactions zoom and pan
zoom = zoom_factory(ax)
pan = panhandler(fig)

# Setup checkboxes for toggling visibility using matplotlib.widgets.CheckButtons
rax = plt.axes([0.01, 0.4, 0.15, 0.25])  # Position for checkboxes
labels = list(metrics.keys())
visibility = [line.get_visible() for line in lines.values()]
check = CheckButtons(rax, labels, visibility)

def toggle(label):
    line = lines[label]
    line.set_visible(not line.get_visible())
    fig.canvas.draw_idle()

check.on_clicked(toggle)

plt.tight_layout()
plt.show()

