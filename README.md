# 🧅 OnionGuard — Smart IoT & AI Post-Harvest Onion Storage System

> An intelligent, low-cost post-harvest storage solution leveraging IoT environmental monitoring and predictive machine learning to detect early rot, optimize airflow, and extend onion shelf life up to 160 days.

---

## 📌 Overview

Post-harvest onion loss accounts for substantial agricultural waste due to fungal decay, premature sprouting, and uncontrolled humidity. **OnionGuard** continuously tracks microclimatic variables and volatile gas emissions inside storage structures to alert farmers and automate active ventilation before visible decay spreads.

---

## ⚡ Key Features

- **Early Rot Prediction:** Detects volatile organic compounds and gases ($H_2S$, $NH_3$, $CO_2$) to forecast spoilage 24–48 hours before physical damage occurs.
- **Dynamic Microclimate Control:** Automated fan/blower trigger based on configurable temperature and relative humidity (RH) thresholds.
- **Shelf-Life Estimation:** ML-driven classification (*Healthy*, *At-Risk*, *Spoiled*) calculating remaining safe storage duration.
- **Farmer-Centric Alerts:** Real-time mobile/web dashboard with SMS notifications and multi-language support.
- **Off-Grid Compatibility:** Low-power ESP32 architecture optimized for solar panel and battery integration.

---

## 🛠️ System Architecture

### 1. Hardware Layer
- **Microcontroller:** ESP32 (Wi-Fi / Bluetooth Low Energy)
- **Climate Monitoring:** DHT22 / BME280 (Temperature & Relative Humidity)
- **Gas Sensing Array:** 
  - `MQ-136` / `MQ-137` — Hydrogen Sulfide ($H_2S$) & Ammonia ($NH_3$) detection
  - `MQ-135` — Air quality and Carbon Dioxide ($CO_2$) concentration
- **Actuation:** 12V DC / BLDC exhaust ventilation fans via relay module
- **Power Unit:** 12V solar charge controller with Li-ion battery backup

### 2. Software & Intelligence Layer
- **Firmware:** C++ / Arduino IDE with FreeRTOS multitasking
- **Cloud & Backend:** Node.js / FastAPI backend with MQTT / HTTP protocols
- **Machine Learning:** Random Forest & LSTM models trained on multi-sensor time-series decay data
- **Frontend / App:** Flutter / React dashboard for real-time visualization and alert logs

---

## 📊 Target Environmental Thresholds

| Parameter | Optimal Range | Warning Trigger | Critical Action |
| :--- | :--- | :--- | :--- |
| **Temperature** | 25°C – 30°C | > 32°C | Activate continuous ventilation |
| **Humidity (RH)** | 65% – 70% | > 75% | Trigger high-speed dehumidifying exhaust |
| **$H_2S$ Gas Level** | < 0.02 ppm | ≥ 0.05 ppm | Spoilage alert & pinpoint batch inspection |
| **$CO_2$ Level** | < 600 ppm | > 1000 ppm | Purge storage chamber air |

---

## 🚀 Quick Setup

```bash
# Clone the repository
git clone [https://github.com/your-username/OnionGuard.git](https://github.com/your-username/OnionGuard.git)

# Navigate to backend directory
cd OnionGuard/backend

# Install dependencies and launch
npm install
npm run start
