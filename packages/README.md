# Home Assistant packages

Copy `energy_flows.yaml` into your HA `config/packages/` folder (or keep this repo synced there).

In `configuration.yaml`:

```yaml
homeassistant:
  packages: !include_dir_named packages
```

Restart Home Assistant after adding.

## Energy dashboard (UI)

Settings → Dashboards → Energy:

| Slot | Suggested sensor |
|------|------------------|
| Solar production | `sensor.solar_production_energy` (lifetime) — daily view uses history; or add the daily utility meter to a Lovelace card |
| Individual devices (optional) | Plug energy sensors if they already report kWh |

## Split Power Flow dashboard (Lovelace YAML)

File: `dashboards/power_flow.yaml` (two columns: Solar/inverter vs Grid).

Put it next to `configuration.yaml` as `/homeassistant/dashboards/power_flow.yaml`, then add:

```yaml
lovelace:
  dashboards:
    power-flow:
      mode: yaml
      title: Power Flow
      icon: mdi:transmission-tower
      show_in_sidebar: true
      filename: dashboards/power_flow.yaml
```

Restart HA (or reload Lovelace). Sidebar → **Power Flow**.

**Charts** tab uses pie/donut cards → install HACS → **ApexCharts Card**, then reload Lovelace.

Keep grid plugs off the Energy dashboard individual-devices list.

## Grid load entities

- Office PC: `sensor.smart_socket_2_power`
- Front Room PC: `sensor.smart_socket_power`
- Pi5 Server: `sensor.ts011f_power`
- Motorbike charger: `sensor.zigbeesensor_power`
- Fridge: `sensor.kitchen_refrigerator_power`
