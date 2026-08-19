# Oniongaurd
An offline-first, dual-core ESP32 climate controller designed to retrofit rural onion storage sheds (Kanda Chawl) and prevent post-harvest rot and sprouting.
Post-harvest onion storage across India faces severe losses every season due to unmonitored temperature spikes, monsoon humidity, and poor ventilation. Traditional farm sheds (Kanda Chawl) rely entirely on guesswork, while commercial cold storage remains out of reach for most smallholders.

OnionGuard is a low-cost, plug-and-play retrofit system built around a dual-core ESP32 running FreeRTOS. Rather than just acting as a passive dashboard, it handles local physical control autonomously:  
PDF

Runs without internet: Core 1 operates the sensors, rate-of-change cooling logic (dT/dt), and relay switches locally, ensuring storage microclimates are managed even during grid and network outages.  
PDF

Catches spikes early: Proactively triggers cooling when temperature rises faster than 1.5°C/min instead of waiting for upper thresholds to fail.  
PDF

Resilient sensing: Employs dual DHT22 sensors with automated divergence failover and a smoothed air-quality proxy to purge stale moisture.  
PDF

Saves bandwidth & data: Implements delta-encoded MQTT to transmit only when parameters shift, while caching dropped packets directly to SPIFFS flash memory during connection dropouts.
