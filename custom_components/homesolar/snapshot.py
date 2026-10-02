"""Build the same snapshot solarshim posts to solarstats."""

from __future__ import annotations

from datetime import datetime, timezone

from .const import BOARD_DOMAINS

_UNAVAILABLE = {"", "unavailable", "unknown", "none", "null"}


def _number(state: str | None) -> float | None:
    if state is None:
        return None
    text = str(state).strip()
    if text.lower() in _UNAVAILABLE:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def _energy_kwh(state: str | None, unit: str | None) -> float | None:
    value = _number(state)
    if value is None:
        return None
    normalized = (unit or "").lower().replace(" ", "")
    if normalized == "wh":
        return value / 1000
    if normalized == "mwh":
        return value * 1000
    return value


def _is_energy(entity_id: str, device_class: str | None, unit: str | None) -> bool:
    if not entity_id.startswith("sensor."):
        return False
    if (device_class or "").lower() == "power":
        return False
    if (device_class or "").lower() == "energy":
        return True
    normalized = (unit or "").lower().replace(" ", "")
    if normalized in {"kwh", "wh", "mwh"}:
        return True
    return False


def entity_choices(hass) -> dict[str, str]:
    """Checkbox labels for switches, lights, and sensors."""
    choices: dict[str, str] = {}
    states = [
        state
        for state in hass.states.async_all()
        if state.entity_id.split(".", 1)[0] in BOARD_DOMAINS
    ]
    states.sort(key=lambda state: ((state.name or state.entity_id).lower(), state.entity_id))
    for state in states:
        name = state.name or state.entity_id
        choices[state.entity_id] = name if name == state.entity_id else f"{name} — {state.entity_id}"
    return choices


def export_entity_ids(entry) -> set[str] | None:
    """None means an older entry that still exports every board entity."""
    if "entities" in entry.options:
        raw = entry.options["entities"]
    elif "entities" in entry.data:
        raw = entry.data["entities"]
    else:
        return None
    return {str(entity_id) for entity_id in raw or []}


def build_snapshot(states, allowed: set[str] | None = None, device_id_for=None) -> dict:
    devices = []
    loads: dict[str, float | None] = {}
    for state in states:
        entity_id = state.entity_id
        if allowed is not None and entity_id not in allowed:
            continue
        domain = entity_id.split(".", 1)[0]
        if domain not in BOARD_DOMAINS:
            continue
        attrs = state.attributes or {}
        device = {
            "entity_id": entity_id,
            "state": state.state,
            "name": attrs.get("friendly_name"),
            "device_class": attrs.get("device_class"),
            "unit": attrs.get("unit_of_measurement"),
            "device_id": device_id_for(entity_id) if device_id_for else None,
        }
        devices.append(device)
        if _is_energy(entity_id, device["device_class"], device["unit"]):
            loads[entity_id] = _energy_kwh(state.state, device["unit"])
    return {
        "ts": datetime.now(timezone.utc).isoformat(),
        "loadsDailyKwh": loads,
        "devices": devices,
    }
