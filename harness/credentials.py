"""Credential storage for model providers.

Agents and subagents need model access at run time, so Odin stores provider
credentials itself rather than depending on whichever vendor CLI happens to be
installed. The store is project-local and never committed.

Security properties this module is responsible for:

* **Secrets never appear in argv.** Adapters receive a credential *name* and
  read the value themselves. Process listings are readable by other users on
  most systems, so passing a key as a command-line argument would leak it.
* **Secrets never appear in output.** Everything that renders a credential goes
  through `mask`; the raw value is only returned by an explicit `secret()` call.
* **Restrictive file mode.** The store is written 0600 where the platform
  supports it, and always inside `.odin/`, which is gitignored.
"""

from __future__ import annotations

import datetime as dt
import os
from pathlib import Path
from typing import Any

from .io import read_json, write_json_atomic

STORE_VERSION = 1
STORE_RELATIVE_PATH = Path(".odin") / "credentials.json"

API_KEY = "api_key"
OAUTH = "oauth"


class CredentialError(Exception):
    """A credential was missing, malformed, or could not be stored."""


def mask(value: str | None) -> str:
    """Render a secret safe for logs, reports, and GUI display."""
    if not value:
        return "<empty>"
    if len(value) <= 12:
        return "*" * len(value)
    return f"{value[:3]}...{value[-4:]}"


class CredentialStore:
    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.path = self.root / STORE_RELATIVE_PATH

    # ---------------------------------------------------------------- io

    def _load(self) -> dict[str, Any]:
        if not self.path.exists():
            return {"version": STORE_VERSION, "credentials": {}}
        try:
            data = read_json(self.path)
        except ValueError as error:
            raise CredentialError(f"credential store is unreadable: {error}") from error
        credentials = data.get("credentials")
        if not isinstance(credentials, dict):
            raise CredentialError("credential store is malformed: 'credentials' is not an object")
        return data

    def _save(self, data: dict[str, Any]) -> None:
        write_json_atomic(self.path, data)
        self._harden()

    def _harden(self) -> None:
        """Restrict the store to the owner where the platform supports it."""
        if os.name == "nt":
            return  # NTFS inherits the user-profile ACL; see docs for the caveat.
        try:
            os.chmod(self.path, 0o600)
        except OSError:
            pass

    # ------------------------------------------------------------ queries

    def names(self) -> list[str]:
        return sorted(self._load()["credentials"])

    def has(self, name: str) -> bool:
        return name in self._load()["credentials"]

    def describe(self, name: str) -> dict[str, Any] | None:
        """Metadata plus a masked value. Safe to print."""
        entry = self._load()["credentials"].get(name)
        if entry is None:
            return None
        described = {
            "name": name,
            "type": entry.get("type", API_KEY),
            "created": entry.get("created"),
            "note": entry.get("note"),
        }
        if entry.get("type") == OAUTH:
            described["value"] = mask(entry.get("access"))
            described["expires"] = entry.get("expires")
            described["expired"] = self.is_expired(name)
        else:
            described["value"] = mask(entry.get("value"))
        return described

    def describe_all(self) -> list[dict[str, Any]]:
        return [described for name in self.names() if (described := self.describe(name))]

    def is_expired(self, name: str) -> bool | None:
        entry = self._load()["credentials"].get(name) or {}
        expires = entry.get("expires")
        if not isinstance(expires, (int, float)):
            return None
        return dt.datetime.now(dt.UTC).timestamp() >= float(expires)

    def secret(self, name: str) -> str:
        """Return the usable secret. The only method that exposes a raw value."""
        entry = self._load()["credentials"].get(name)
        if entry is None:
            raise CredentialError(
                f"no credential named '{name}'. Add one with: python odin.py auth set {name}"
            )
        if entry.get("type") == OAUTH:
            access = entry.get("access")
            if not access:
                raise CredentialError(f"credential '{name}' has no access token")
            if self.is_expired(name):
                raise CredentialError(
                    f"credential '{name}' has expired; refresh it in the issuing tool "
                    f"and re-import, or run: python odin.py auth set {name}"
                )
            return str(access)
        value = entry.get("value")
        if not value:
            raise CredentialError(f"credential '{name}' has no value")
        return str(value)

    # ------------------------------------------------------------ mutation

    def set_api_key(self, name: str, value: str, note: str | None = None) -> None:
        if not value or not value.strip():
            raise CredentialError("refusing to store an empty credential")
        data = self._load()
        data["credentials"][name] = {
            "type": API_KEY,
            "value": value.strip(),
            "created": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
            "note": note,
        }
        self._save(data)

    def set_oauth(
        self,
        name: str,
        access: str,
        refresh: str | None = None,
        expires: float | None = None,
        note: str | None = None,
    ) -> None:
        if not access or not access.strip():
            raise CredentialError("refusing to store an empty access token")
        data = self._load()
        data["credentials"][name] = {
            "type": OAUTH,
            "access": access.strip(),
            "refresh": refresh,
            "expires": expires,
            "created": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
            "note": note,
        }
        self._save(data)

    def remove(self, name: str) -> bool:
        data = self._load()
        if name not in data["credentials"]:
            return False
        del data["credentials"][name]
        self._save(data)
        return True


def resolve_secret(
    root: Path,
    credential: str | None = None,
    api_key_env: str | None = None,
) -> str | None:
    """Resolve a provider secret.

    Environment first, so containers and CI can inject a value without writing
    it to disk; then the stored credential. Returns None when neither is
    configured, which is valid for local servers that need no key.
    """
    if api_key_env:
        value = os.environ.get(api_key_env)
        if value:
            return value
    if credential:
        environment_override = os.environ.get(f"ODIN_CREDENTIAL_{credential.upper().replace('-', '_')}")
        if environment_override:
            return environment_override
        return CredentialStore(root).secret(credential)
    if api_key_env:
        raise CredentialError(f"{api_key_env} is not set")
    return None
