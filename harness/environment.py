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
    environment = {name: os.environ[name] for name in names if os.environ.get(name)}
    environment.update(global_values)
    environment.update(command_values)
    environment.update(generated)
    return environment
