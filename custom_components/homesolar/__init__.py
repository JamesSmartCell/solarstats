"""Home Solar: pair with a 5-character code, then push HA snapshots."""

from __future__ import annotations

import logging
from datetime import timedelta
from urllib.parse import quote

import aiohttp
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.event import async_track_time_interval
from homeassistant.loader import async_get_integration

from .const import CLICKABLE_DOMAINS, DEFAULT_BASE_URL, DOMAIN, POLL_INTERVAL_SECONDS
from .snapshot import build_snapshot, export_entity_ids

_LOGGER = logging.getLogger(__name__)


async def async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    await hass.config_entries.async_reload(entry.entry_id)


async def _point_help_link(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Point the integration (?) button at this installation's help page."""
    name = str(entry.data.get("name") or entry.title or "").strip()
    base = str(entry.data.get("base_url") or DEFAULT_BASE_URL).rstrip("/")
    if not name or not base:
        return
    try:
        integration = await async_get_integration(hass, DOMAIN)
    except Exception as err:
        _LOGGER.warning("Home Solar help link was not updated: %s", err)
        return
    integration.manifest["documentation"] = f"{base}/help?name={quote(name)}"


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    await _point_help_link(hass, entry)
    runtime = HomeSolarRuntime(hass, entry)
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = runtime
    entry.async_on_unload(entry.add_update_listener(async_update_listener))
    await runtime.async_start()
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    runtime = hass.data.get(DOMAIN, {}).pop(entry.entry_id, None)
    if runtime:
        runtime.async_stop()
    return True


class HomeSolarRuntime:
    """Poll local HA and POST the snapshot solarstats already accepts."""

    def __init__(self, hass: HomeAssistant, entry: ConfigEntry) -> None:
        self.hass = hass
        self.entry = entry
        self._unsub = None

    async def async_start(self) -> None:
        self._unsub = async_track_time_interval(
            self.hass,
            self._async_tick,
            timedelta(seconds=POLL_INTERVAL_SECONDS),
        )
        await self._async_tick(None)

    def async_stop(self) -> None:
        if self._unsub:
            self._unsub()
            self._unsub = None

    def _urls(self) -> tuple[str, str, str]:
        data = self.entry.data
        base = str(data["base_url"]).rstrip("/")
        slug = data["slug"]
        return (
            f"{base}/api/ingest/{slug}",
            f"{base}/api/agent/commands/{slug}",
            f"{base}/api/agent/{slug}/commands",
        )

    async def _async_tick(self, _now) -> None:
        try:
            await self._push_commands()
            allowed = export_entity_ids(self.entry)
            registry = er.async_get(self.hass)

            def device_id_for(entity_id: str) -> str | None:
                entry = registry.async_get(entity_id)
                return entry.device_id if entry else None

            snapshot = build_snapshot(self.hass.states.async_all(), allowed, device_id_for)
            ingest, _commands, _complete = self._urls()
            session = async_get_clientsession(self.hass)
            async with session.post(
                ingest,
                json=snapshot,
                headers=self._headers(),
                timeout=aiohttp.ClientTimeout(total=20),
            ) as resp:
                if resp.status >= 400:
                    body = await resp.text()
                    _LOGGER.warning("Home Solar ingest HTTP %s %s", resp.status, body[:200])
        except aiohttp.ClientError as err:
            _LOGGER.warning("Home Solar push failed: %s", err)

    def _headers(self) -> dict[str, str]:
        return {
            "Authorization": f"Bearer {self.entry.data['secret']}",
            "Content-Type": "application/json",
        }

    async def _push_commands(self) -> None:
        _ingest, commands_url, complete_base = self._urls()
        session = async_get_clientsession(self.hass)
        async with session.get(
            commands_url,
            headers=self._headers(),
            timeout=aiohttp.ClientTimeout(total=20),
        ) as resp:
            if resp.status >= 400:
                return
            payload = await resp.json(content_type=None)
        for cmd in payload.get("commands") or []:
            entity_id = str(cmd.get("entityId") or "")
            domain = entity_id.split(".", 1)[0]
            allowed = export_entity_ids(self.entry)
            ok = False
            reported = None
            if allowed is not None and entity_id not in allowed:
                ok = False
            elif domain in CLICKABLE_DOMAINS:
                before = self.hass.states.get(entity_id)
                previous = before.state if before is not None else None
                try:
                    await self.hass.services.async_call(
                        domain,
                        "toggle" if cmd.get("action") == "toggle" else str(cmd.get("action") or "toggle"),
                        {"entity_id": entity_id},
                        blocking=True,
                    )
                    ok = True
                    after = self.hass.states.get(entity_id)
                    reported = after.state if after is not None else None
                    if reported in (None, previous) and previous in ("on", "off"):
                        reported = "off" if previous == "on" else "on"
                except Exception as err:  # noqa: BLE001 — report failure upstream
                    _LOGGER.warning("Home Solar command %s failed: %s", entity_id, err)
            await session.post(
                f"{complete_base}/{cmd.get('id')}/complete",
                json={"ok": ok, "state": reported},
                headers=self._headers(),
                timeout=aiohttp.ClientTimeout(total=20),
            )
