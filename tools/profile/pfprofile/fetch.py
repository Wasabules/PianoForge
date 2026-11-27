# SPDX-License-Identifier: AGPL-3.0-or-later

"""Fetching a bank: a GitHub repository at a pinned commit, file by file, each one checked against its git blob
hash (an interrupted file resumed where it stopped: some networks cut long transfers after a few tens of MB), or a
bank downloaded by hand, then checked."""

from __future__ import annotations

import hashlib
import json
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

USER_AGENT = "PianoForge-profile-tool"  # (a generic client name: nothing about who runs it)
RETRIES = 8


def git_blob_sha1(path: Path) -> str:
    """The hash git gives a file's content (sha1 of "blob <size>\\0" + the bytes)."""
    h = hashlib.sha1()
    h.update(b"blob %d\0" % path.stat().st_size)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _get(url: str, headers: dict | None = None, timeout: float = 60.0):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    return urllib.request.urlopen(request, timeout=timeout)


def _tree(repository: str, commit: str) -> list[dict]:
    url = f"https://api.github.com/repos/{repository}/git/trees/{commit}?recursive=1"
    with _get(url, {"Accept": "application/vnd.github+json"}) as r:
        data = json.load(r)
    if data.get("truncated"):
        raise SystemExit(f"{repository}: the file list is truncated (too many files for one request)")
    return [e for e in data["tree"] if e["type"] == "blob"]


def _download(url: str, target: Path, size: int, log) -> None:
    """To target.part, resumed with a Range request after a cut; then renamed."""
    part = target.with_name(target.name + ".part")
    for attempt in range(1, RETRIES + 1):
        have = part.stat().st_size if part.exists() else 0
        if have > size:
            part.unlink()
            have = 0
        if have == size:
            break
        try:
            headers = {"Range": f"bytes={have}-"} if have else {}
            with _get(url, headers) as r, open(part, "ab" if have and r.status == 206 else "wb") as out:
                while chunk := r.read(1 << 20):
                    out.write(chunk)
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as error:
            log(f"    {target.name}: cut after {part.stat().st_size if part.exists() else 0} bytes ({error}); "
                f"resuming ({attempt}/{RETRIES})")
            time.sleep(min(30, 2 * attempt))
    if not part.exists() or part.stat().st_size != size:
        raise SystemExit(f"{target.name}: could not be downloaded whole after {RETRIES} tries")
    part.replace(target)


def fetch_github(repository: str, commit: str, folder: Path, log=print) -> int:
    """Every file of the repository at `commit` into `folder`; the ones already there and right are kept. Returns how
    many were downloaded."""
    files = _tree(repository, commit)
    total = sum(e["size"] for e in files)
    log(f"{repository} at {commit[:7]}: {len(files)} files, {total / 1e6:.0f} MB, into {folder}")
    done_bytes, fetched = 0, 0
    for i, e in enumerate(files, 1):
        target = folder / e["path"]
        if target.exists() and target.stat().st_size == e["size"] and git_blob_sha1(target) == e["sha"]:
            done_bytes += e["size"]
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/{repository}/{commit}/{urllib.parse.quote(e['path'])}"
        _download(url, target, e["size"], log)
        if git_blob_sha1(target) != e["sha"]:
            target.unlink()
            raise SystemExit(f"{e['path']}: its content does not match the repository's (hash), removed; run again")
        fetched += 1
        done_bytes += e["size"]
        if fetched == 1 or fetched % 25 == 0 or i == len(files):
            log(f"  [{i}/{len(files)}] {done_bytes / 1e6:.0f} of {total / 1e6:.0f} MB")
    log(f"{fetched} files downloaded, {len(files) - fetched} already there")
    return fetched
