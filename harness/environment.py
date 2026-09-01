from __future__ import annotations

import os
from collections.abc import Iterable, Mapping


BASELINE = (
    "PATH", "HOME", "TEMP", "TMP", "TMPDIR", "LANG", "LANGUAGE", "LC_ALL",
    "LC_CTYPE", "LC_MESSAGES", "LC_COLLATE", "LC_MONETARY", "LC_NUMERIC",
    "LC_TIME", "TZ", "SSL_CERT_FILE", "SSL_CERT_DIR",
)
WINDOWS_BASELINE = (
    "SystemRoot", "WINDIR", "COMSPEC", "PATHEXT", "USERPROFILE", "HOMEDRIVE",
    "HOMEPATH", "APPDATA", "LOCALAPPDATA",
)


def build_child_environment(
    *,
    inherit: Iterable[str] = (),
    global_values: Mapping[str, str] = {},
    command_values: Mapping[str, str] = {},
    generated: Mapping[str, str] = {},
) -> dict[str, str]:
    names = (*BASELINE, *(WINDOWS_BASELINE if os.name == "nt" else ()), *inherit)
    environment: dict[str, str] = {}

    def update(values: Mapping[str, str]) -> None:
        for name, value in values.items():
            if os.name == "nt":
                for existing in list(environment):
                    if existing.casefold() == name.casefold():
                        del environment[existing]
            environment[name] = value

    update({name: os.environ[name] for name in names if os.environ.get(name)})
    update(global_values)
    update(command_values)
    update(generated)
    return environment


def validate_environment_names(names: Iterable[str]) -> None:
    invalid = [name for name in names if not name or "=" in name or "\0" in name]
    if invalid:
        raise ValueError(f"invalid child environment name: {invalid[0]!r}")


def redacted(text: str, inherit: Iterable[str], configured: Mapping[str, str]) -> str:
    values = [os.environ[name] for name in inherit if os.environ.get(name)]
    secret_words = ("KEY", "TOKEN", "SECRET", "PASSWORD", "CREDENTIAL")
    values.extend(
        value for name, value in configured.items()
        if any(word in name.upper() for word in secret_words)
    )
    for value in values:
        if value:
            text = text.replace(value, "[REDACTED]")
    return text
