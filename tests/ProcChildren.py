"""Observe direct children, including kernels without CONFIG_CHECKPOINT_RESTORE."""
from pathlib import Path


def children(pid):
    found = set()
    unavailable = False
    for task in Path('/proc', str(pid), 'task').iterdir():
        try:
            found.update(int(child) for child in (task / 'children').read_text().split())
        except FileNotFoundError:
            # A completed thread is normal. A live thread without this optional
            # proc file requires another observation, never an empty-child PASS.
            unavailable |= task.exists()
    if unavailable:
        for process in Path('/proc').iterdir():
            if not process.name.isdecimal():
                continue
            try:
                status = (process / 'status').read_text()
            except (FileNotFoundError, ProcessLookupError):
                continue  # A process exited during the scan.
            parent = next(line for line in status.splitlines() if line.startswith('PPid:'))
            if int(parent.split()[1]) == pid:
                found.add(int(process.name))
    return found
