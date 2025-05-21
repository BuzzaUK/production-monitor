#!/bin/bash

set -e

echo "Updating system and installing dependencies..."
sudo apt-get update
sudo apt-get install -y python3 python3-pip python3-pyqt5 python3-flask python3-pandas python3-matplotlib python3-yaml python3-rpi.gpio git

echo "Installing additional Python packages..."
pip3 install flask pandas matplotlib pyyaml RPi.GPIO

echo "Creating project directory..."
mkdir -p ~/production_monitor/data
cd ~/production_monitor

echo "Ensuring permissions for data directory..."
chmod 755 ~/production_monitor/data

echo "Creating virtual environment (optional)..."
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt || true  # Only if you add a requirements.txt

echo "Copying systemd service files..."

cat > /etc/systemd/system/production-monitor.service <<EOF
[Unit]
Description=Production Line Monitor Main Service
After=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /home/pi/production_monitor/main.py
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/production-monitor-ui.service <<EOF
[Unit]
Description=Production Line Monitor HDMI UI
After=production-monitor.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /home/pi/production_monitor/ui.py
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/production-monitor-web.service <<EOF
[Unit]
Description=Production Line Monitor Web Dashboard
After=production-monitor.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /home/pi/production_monitor/webserver.py
Restart=always
User=pi

[Install]
WantedBy=multi-user.target
EOF

echo "Reloading systemd daemon and enabling services..."
sudo systemctl daemon-reload
sudo systemctl enable production-monitor.service
sudo systemctl enable production-monitor-ui.service
sudo systemctl enable production-monitor-web.service

echo "Setting up Wi-Fi static IP (manual step if not already set)..."
echo "Add the following to /etc/dhcpcd.conf if you haven't already:"
echo "
interface wlan0
static ip_address=192.168.1.100/24
static routers=192.168.1.1
static domain_name_servers=8.8.8.8
"
echo "And configure Wi-Fi credentials in /etc/wpa_supplicant/wpa_supplicant.conf."

echo "Setup complete! Reboot to start all services:"
echo "sudo reboot"