#!/bin/bash

# Test ingest script to toggle rain sensor state

STATE=${1:-dry}

curl -X POST http://localhost:8787/api/ingest \
  -H "Authorization: Bearer test-secret" \
  -H "Content-Type: application/json" \
  -d '{
  "ts": "'$(date -Iseconds)'",
  "battery_voltage": 51.2,
  "battery_soc": 85,
  "pv_power": 150,
  "output_power": 200,
  "devices": [
    {
      "entity_id": "binary_sensor.garden_rainfall_rain_detected",
      "domain": "binary_sensor",
      "name": "Garden rainfall",
      "device_class": "moisture",
      "state": "'$STATE'",
      "unit": ""
    },
    {
      "entity_id": "sensor.battery_voltage",
      "domain": "sensor",
      "name": "Battery Voltage",
      "state": "51.2",
      "unit": "V"
    }
  ]
}'

echo ""
