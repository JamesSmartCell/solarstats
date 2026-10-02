"""Config flow: show a 5-character code until Home Solar claims it."""

from __future__ import annotations

import asyncio
import secrets
from typing import Any

import aiohttp
import voluptuous as vol
from homeassistant import config_entries
from homeassistant.helpers import selector
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .const import BOARD_DOMAINS, CODE_ALPHABET, DEFAULT_BASE_URL, DOMAIN
from .snapshot import entity_choices

PAIR_TIMEOUT_SECONDS = 10 * 60


def entity_checklist(hass, default: list[str] | None = None):
    """Scrollable checkbox list of every switch, light, and sensor."""
    choices = entity_choices(hass)
    selected = [entity_id for entity_id in (default or []) if entity_id in choices]
    return selector.SelectSelector(
        selector.SelectSelectorConfig(
            options=[{"value": entity_id, "label": label} for entity_id, label in choices.items()],
            multiple=True,
            mode="list",
        )
    ), selected


class NameTaken(Exception):
    """The typed home name already has an address."""


class HomeSolarConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Pair this Home Assistant with a Home Solar site."""

    VERSION = 1

    def __init__(self) -> None:
        self._name = ""
        self._email = ""
        self._base = DEFAULT_BASE_URL
        self._code = ""
        self._poll_token = ""
        self._pair_task: asyncio.Task | None = None

    async def async_step_user(self, user_input: dict[str, Any] | None = None):
        errors: dict[str, str] = {}
        if user_input is not None:
            self._email = str(user_input.get("email") or "").strip().lower()
            self._base = str(user_input.get("base_url") or DEFAULT_BASE_URL).strip().rstrip("/")
            if "@" not in self._email or "." not in self._email.split("@")[-1]:
                errors["email"] = "invalid_email"
            else:
                return await self.async_step_entities()
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required("email", default=self._email): str,
                    vol.Required("base_url", default=self._base or DEFAULT_BASE_URL): str,
                }
            ),
            errors=errors,
            description_placeholders={"base": self._base or DEFAULT_BASE_URL},
        )

    async def _check_name(self) -> None:
        session = async_get_clientsession(self.hass)
        async with session.post(
            f"{self._base}/api/pair/name",
            json={"name": self._name},
            timeout=aiohttp.ClientTimeout(total=20),
        ) as resp:
            body = await resp.json(content_type=None)
            if resp.status == 409 or body.get("error") == "name_taken":
                raise NameTaken
            if resp.status >= 400:
                raise aiohttp.ClientResponseError(
                    resp.request_info,
                    resp.history,
                    status=resp.status,
                    message=str(body.get("error") or resp.reason),
                )
            self._slug = body.get("slug") or ""

    async def _start_with_fresh_code(self) -> None:
        last_error: Exception | None = None
        for _ in range(4):
            self._code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(5))
            try:
                await self._post_start()
                return
            except aiohttp.ClientResponseError as err:
                last_error = err
                if err.status != 409 or "name_taken" in str(err.message or ""):
                    raise
        if last_error:
            raise last_error

    async def _post_start(self) -> None:
        session = async_get_clientsession(self.hass)
        async with session.post(
            f"{self._base}/api/pair/start",
            json={
                "code": self._code,
                "name": self._name,
                "email": self._email,
                "pollToken": self._poll_token,
            },
            timeout=aiohttp.ClientTimeout(total=20),
        ) as resp:
            if resp.status >= 400:
                raise aiohttp.ClientResponseError(
                    resp.request_info,
                    resp.history,
                    status=resp.status,
                    message=await resp.text(),
                )

    async def _poll_until_linked(self) -> dict[str, Any]:
        session = async_get_clientsession(self.hass)
        deadline = asyncio.get_running_loop().time() + PAIR_TIMEOUT_SECONDS
        while asyncio.get_running_loop().time() < deadline:
            async with session.get(
                f"{self._base}/api/pair/poll",
                params={"token": self._poll_token},
                timeout=aiohttp.ClientTimeout(total=20),
            ) as resp:
                body = await resp.json(content_type=None)
            status = body.get("status")
            if status == "linked" and body.get("secret"):
                return body
            if status == "expired":
                raise TimeoutError("expired")
            await asyncio.sleep(2)
        raise TimeoutError("expired")

    async def async_step_code(self, user_input: dict[str, Any] | None = None):
        """Show the pairing code and collect the installation name on the same screen."""
        errors: dict[str, str] = {}
        if not self._code:
            self._code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(5))
        if user_input is not None:
            self._name = str(user_input.get("name") or "").strip()
            if len(self._name) < 1:
                errors["name"] = "invalid_name"
            else:
                self._poll_token = secrets.token_urlsafe(32)
                try:
                    await self._check_name()
                    await self._post_start()
                except NameTaken:
                    errors["name"] = "name_taken"
                except aiohttp.ClientResponseError as err:
                    message = str(err.message or "")
                    if err.status == 409 and "code_in_use" in message:
                        self._code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(5))
                        errors["base"] = "unknown"
                    else:
                        errors["name" if err.status == 400 else "base"] = (
                            "invalid_name" if err.status == 400 else "cannot_connect"
                        )
                except aiohttp.ClientError:
                    errors["base"] = "cannot_connect"
                else:
                    return await self.async_step_link()
        return self.async_show_form(
            step_id="code",
            data_schema=vol.Schema(
                {
                    vol.Required("name", default=self._name): str,
                }
            ),
            errors=errors,
            description_placeholders={
                "code": self._code or "……",
                "base_url": self._base,
            },
        )

    async def async_step_link(self, user_input: dict[str, Any] | None = None):
        if self._pair_task is None:
            self._pair_task = self.hass.async_create_task(self._poll_until_linked())
        if not self._pair_task.done():
            return self.async_show_progress(
                step_id="link",
                progress_action="pair",
                progress_task=self._pair_task,
                description_placeholders={
                    "code": self._code,
                    "base_url": self._base,
                    "url": f"{self._base}/connect?code={self._code}",
                    "qr_url": f"{self._base}/api/pair/qr.png?code={self._code}",
                },
            )
        try:
            result = self._pair_task.result()
        except (TimeoutError, aiohttp.ClientError):
            return self.async_show_progress_done(next_step_id="expired")
        self._linked = result
        return self.async_show_progress_done(next_step_id="finish")

    def _entity_schema(self, default: list[str] | None = None):
        checklist, selected = entity_checklist(self.hass, default)
        return vol.Schema({vol.Required("entities", default=selected): checklist})

    def _selected_entities(self, user_input: dict[str, Any]) -> list[str]:
        selected = []
        for entity in user_input.get("entities") or []:
            entity_id = str(entity)
            if entity_id.split(".", 1)[0] in BOARD_DOMAINS:
                selected.append(entity_id)
        return selected

    async def async_step_entities(self, user_input: dict[str, Any] | None = None):
        errors: dict[str, str] = {}
        if user_input is not None:
            self._entities = self._selected_entities(user_input)
            return await self.async_step_code()
        return self.async_show_form(
            step_id="entities",
            data_schema=self._entity_schema(),
            errors=errors,
        )

    async def async_step_expired(self, user_input: dict[str, Any] | None = None):
        return self.async_abort(reason="expired")

    async def async_step_finish(self, user_input: dict[str, Any] | None = None):
        slug = self._linked["slug"]
        await self.async_set_unique_id(slug)
        self._abort_if_unique_id_configured()
        return self.async_create_entry(
            title=self._name,
            data={
                "base_url": self._base,
                "slug": slug,
                "secret": self._linked["secret"],
                "name": self._name,
                "email": self._email,
                "entities": getattr(self, "_entities", []),
            },
        )

    @staticmethod
    def async_get_options_flow(config_entry: config_entries.ConfigEntry):
        return HomeSolarOptionsFlow()


class HomeSolarOptionsFlow(config_entries.OptionsFlow):
    """Change which entities this home exports."""

    async def async_step_init(self, user_input: dict[str, Any] | None = None):
        entry = self.config_entry
        current = list(entry.options.get("entities", entry.data.get("entities", [])))
        errors: dict[str, str] = {}
        if user_input is not None:
            name = str(user_input.get("name") or "").strip()
            email = str(user_input.get("email") or "").strip().lower()
            selected = []
            for entity in user_input.get("entities") or []:
                entity_id = str(entity)
                if entity_id.split(".", 1)[0] in BOARD_DOMAINS:
                    selected.append(entity_id)
            try:
                updated = await self._save_profile(name, email)
            except NameTaken:
                errors["name"] = "name_taken"
            except aiohttp.ClientError:
                errors["base"] = "cannot_connect"
            else:
                self.hass.config_entries.async_update_entry(
                    entry,
                    title=updated.get("name") or name,
                    unique_id=updated.get("slug") or entry.unique_id,
                    data={
                        **entry.data,
                        "name": updated.get("name") or name,
                        "email": email,
                        "slug": updated.get("slug") or entry.data.get("slug"),
                    },
                )
                return self.async_create_entry(title="", data={"entities": selected})
        checklist, selected = entity_checklist(self.hass, current)
        return self.async_show_form(
            step_id="init",
            data_schema=vol.Schema(
                {
                    vol.Required("name", default=entry.data.get("name") or entry.title): str,
                    vol.Required("email", default=entry.data.get("email") or ""): str,
                    vol.Required("entities", default=selected): checklist,
                }
            ),
            errors=errors,
        )

    async def _save_profile(self, name: str, email: str) -> dict[str, Any]:
        data = self.config_entry.data
        base = str(data["base_url"]).rstrip("/")
        session = async_get_clientsession(self.hass)
        async with session.post(
            f"{base}/api/ingest/{data['slug']}/profile",
            json={"name": name, "adminEmail": email},
            headers={"Authorization": f"Bearer {data['secret']}"},
            timeout=aiohttp.ClientTimeout(total=20),
        ) as resp:
            body = await resp.json(content_type=None)
            if resp.status == 409 or body.get("error") == "name_taken":
                raise NameTaken
            if resp.status >= 400:
                raise aiohttp.ClientError(str(body.get("error") or resp.status))
            return body
