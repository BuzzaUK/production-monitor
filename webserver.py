from flask import Flask, render_template, send_from_directory
import pandas as pd
import os
import yaml
from datetime import datetime

CONFIG_FILE = "config.yaml"
CSV_FILE = os.path.join("data", "events.csv")

app = Flask(__name__, template_folder='templates')

def load_config():
    with open(CONFIG_FILE, "r") as f:
        return yaml.safe_load(f)

@app.route("/")
def dashboard():
    config = load_config()
    df = pd.read_csv(CSV_FILE)
    now = datetime.now()
    stats = []
    for machine in config['machines']:
        name = machine['name']
        mdf = df[df['machine'] == name]
        last = mdf.tail(1)
        running = True if last.empty else (last.iloc[-1]['event'] == 'RUNNING')
        day_df = mdf[pd.to_datetime(mdf['timestamp']).dt.date == now.date()]
        uptime_s = 0.0
        total_s = 0.0
        last_time = None
        last_state = None
        for _, row in day_df.iterrows():
            t = pd.to_datetime(row['timestamp'])
            if row['event'] == 'RUNNING':
                last_time = t
                last_state = 'RUNNING'
            elif row['event'] == 'STOPPED' and last_time is not None and last_state == 'RUNNING':
                duration = (t - last_time).total_seconds()
                uptime_s += duration
                last_time = t
                last_state = 'STOPPED'
        if last_state == 'RUNNING' and last_time is not None:
            uptime_s += (now - last_time).total_seconds()
        if not day_df.empty:
            first_time = pd.to_datetime(day_df.iloc[0]['timestamp'])
            total_s = (now - first_time).total_seconds()
        else:
            total_s = (now - datetime(now.year, now.month, now.day)).total_seconds()
        uptime_pct = (uptime_s / total_s * 100) if total_s else 100.0
        stops = day_df[day_df['event'] == 'STOPPED']['duration_s'].astype(float).values
        runs = day_df[day_df['event'] == 'RUNNING']['duration_s'].astype(float).values
        mtbf = runs.mean() / 60 if len(runs) else 0.0
        mttr = stops.mean() / 60 if len(stops) else 0.0
        stats.append({
            "name": name,
            "running": running,
            "uptime": f"{uptime_pct:.1f}",
            "mtbf": f"{mtbf:.1f}",
            "mttr": f"{mttr:.1f}"
        })
    return render_template("dashboard.html", stats=stats)

@app.route("/data/events.csv")
def download_csv():
    return send_from_directory("data", "events.csv", as_attachment=True)

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)