import sys
import yaml
import csv
import os
from datetime import datetime, timedelta
from PyQt5.QtWidgets import (
    QApplication, QWidget, QLabel, QVBoxLayout, QHBoxLayout, QGridLayout, QSizePolicy
)
from PyQt5.QtGui import QColor, QFont, QPalette
from PyQt5.QtCore import Qt, QTimer
import matplotlib
matplotlib.use('Agg')
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
from matplotlib.figure import Figure
import pandas as pd

CONFIG_FILE = "config.yaml"
CSV_FILE = os.path.join("data", "events.csv")

def load_config():
    with open(CONFIG_FILE, "r") as f:
        return yaml.safe_load(f)

class HistoryChart(FigureCanvas):
    def __init__(self, df, machine_name, max_days):
        fig = Figure(figsize=(3, 1.2), dpi=100)
        super().__init__(fig)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.machine_name = machine_name
        self.max_days = max_days
        self.plot(df)

    def plot(self, df):
        ax = self.figure.add_subplot(111)
        ax.clear()
        if df is not None and not df.empty:
            df = df[df['machine'] == self.machine_name]
            df['date'] = pd.to_datetime(df['timestamp']).dt.date
            daily = df.groupby(['date', 'event']).size().unstack(fill_value=0)
            last_days = pd.date_range(end=datetime.now(), periods=self.max_days).date
            for d in last_days:
                if d not in daily.index:
                    daily.loc[d] = [0, 0]
            daily = daily.sort_index().tail(self.max_days)
            ax.plot(daily.index, daily.get('STOPPED', 0), 'r-', label='Stops/day')
            ax.set_title('Stops/day')
            ax.set_xticklabels([str(d)[5:] for d in daily.index], rotation=45, fontsize=8)
            ax.set_yticks([])
        self.figure.tight_layout()
        self.draw()

class MachinePanel(QWidget):
    def __init__(self, machine, config):
        super().__init__()
        self.machine = machine
        self.config = config
        self.status_label = QLabel("Status: --")
        self.uptime_label = QLabel("Uptime: -- %")
        self.mtbf_label = QLabel("MTBF: -- min")
        self.mttr_label = QLabel("MTTR: -- min")
        font = QFont("Arial", self.config['display']['font_size'], QFont.Bold)
        for label in [self.status_label, self.uptime_label, self.mtbf_label, self.mttr_label]:
            label.setFont(font)
            label.setAlignment(Qt.AlignCenter)
        vbox = QVBoxLayout()
        title = QLabel(self.machine['name'])
        title.setFont(QFont("Arial", self.config['display']['font_size'] + 12, QFont.Bold))
        title.setAlignment(Qt.AlignCenter)
        vbox.addWidget(title)
        vbox.addWidget(self.status_label)
        vbox.addWidget(self.uptime_label)
        vbox.addWidget(self.mtbf_label)
        vbox.addWidget(self.mttr_label)
        self.chart = None
        if self.config['display'].get('show_historical_chart', True):
            self.chart = HistoryChart(pd.DataFrame(), self.machine['name'], self.config['display'].get('max_history_days', 30))
            vbox.addWidget(self.chart)
        self.setLayout(vbox)
        self.setAutoFillBackground(True)
        self.update_colors(False)

    def update_colors(self, running):
        pal = self.palette()
        if running:
            pal.setColor(QPalette.Window, QColor(self.config['display']['running_color']))
            self.status_label.setStyleSheet("color: white")
        else:
            pal.setColor(QPalette.Window, QColor(self.config['display']['highlight_color']))
            self.status_label.setStyleSheet("color: white; background-color: red;")
        self.setPalette(pal)

    def update_panel(self, running, uptime, mtbf, mttr, df):
        self.status_label.setText("Running" if running else "Stopped")
        self.update_colors(running)
        self.uptime_label.setText(f"Uptime: {uptime:.1f} %")
        self.mtbf_label.setText(f"MTBF: {mtbf:.1f} min")
        self.mttr_label.setText(f"MTTR: {mttr:.1f} min")
        if self.chart:
            self.chart.plot(df)

class MainWindow(QWidget):
    def __init__(self, config):
        super().__init__()
        self.config = config
        n = len(config['machines'])
        cols = 2 if n <= 4 else 5
        rows = (n + cols - 1) // cols
        grid = QGridLayout()
        self.panels = {}
        for idx, machine in enumerate(config['machines']):
            row = idx // cols
            col = idx % cols
            panel = MachinePanel(machine, config)
            self.panels[machine['name']] = panel
            grid.addWidget(panel, row, col)
        self.setLayout(grid)
        self.setWindowTitle("Production Line Monitor")
        self.showFullScreen()
        self.timer = QTimer()
        self.timer.timeout.connect(self.refresh)
        self.timer.start(config['display']['update_interval'] * 1000)

    def refresh(self):
        df = pd.read_csv(CSV_FILE)
        now = datetime.now()
        for machine in self.config['machines']:
            name = machine['name']
            mdf = df[df['machine'] == name]
            last = mdf.tail(1)
            running = True if last.empty else (last.iloc[-1]['event'] == 'RUNNING')
            # Uptime for today
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
            # If currently running, add time to now
            if last_state == 'RUNNING' and last_time is not None:
                uptime_s += (now - last_time).total_seconds()
            # Total time since first event today or midnight
            if not day_df.empty:
                first_time = pd.to_datetime(day_df.iloc[0]['timestamp'])
                total_s = (now - first_time).total_seconds()
            else:
                total_s = (now - datetime(now.year, now.month, now.day)).total_seconds()
            uptime_pct = (uptime_s / total_s * 100) if total_s else 100.0
            # MTBF/MTTR
            stops = day_df[day_df['event'] == 'STOPPED']['duration_s'].astype(float).values
            runs = day_df[day_df['event'] == 'RUNNING']['duration_s'].astype(float).values
            mtbf = runs.mean() / 60 if len(runs) else 0.0
            mttr = stops.mean() / 60 if len(stops) else 0.0
            self.panels[name].update_panel(running, uptime_pct, mtbf, mttr, df)

if __name__ == "__main__":
    app = QApplication(sys.argv)
    config = load_config()
    mw = MainWindow(config)
    sys.exit(app.exec_())