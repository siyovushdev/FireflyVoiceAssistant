"""Bounded release-resource barrier and idempotent recovery; no device I/O.

Operations are supplied by the rollout supervisor. Never kill a PID reported
by fuser: it may belong to a different service. A failed probe is not 'free'.
"""
import time


class RecoveryTimeout(RuntimeError):
    pass


def quiescent(snapshot):
    return (not snapshot.get('error') and snapshot['owners'] == []
            and snapshot['cgroup_pids'] == [] and snapshot['main_pid'] == 0
            and snapshot['control_pid'] == 0
            and snapshot['active_state'] in ('inactive', 'failed'))


def wait_quiescent(probe, record, deadline, *, clock=time.monotonic,
                   pause=time.sleep, interval=.25):
    """Require two consecutive complete observations within an absolute deadline."""
    stable = 0
    last = None
    while clock() < deadline:
        try:
            last = probe()
        except Exception as error:
            last = {'error': repr(error)}
        record('resource-observation', last)
        # A slow probe returning after the deadline cannot authorize a start.
        if clock() >= deadline:
            break
        stable = stable + 1 if quiescent(last) else 0
        if stable == 2:
            return last
        pause(min(interval, max(0, deadline - clock())))
    raise RecoveryTimeout('Resources not verified free before deadline: ' + repr(last))


def recover(ops, old, deadline, *, clock=time.monotonic, pause=time.sleep):
    """Restore only the recorded old release; retry failures, never a cutover.

    The deadline comes from the armed plan, not from this process's start time.
    Re-entry after a supervisor crash first verifies an already recovered unit.
    Every mutation is followed by (or requires) the same ownership/health gates.
    """
    last = None
    attempt = 0
    while clock() < deadline:
        attempt += 1
        ops.record('recovery-attempt', {'attempt': attempt, 'remaining_seconds': deadline - clock()})
        try:
            if ops.current() == old:
                try:
                    result = ops.gates(old)
                    if clock() < deadline:
                        return result
                except Exception:
                    pass
            ops.stop()
            if clock() >= deadline:
                raise RecoveryTimeout('Deadline reached during service stop')
            wait_quiescent(ops.resource_snapshot, ops.record,
                           min(deadline, clock() + 30), clock=clock, pause=pause)
            if ops.current() != old:
                ops.switch(old)
            if clock() >= deadline:
                raise RecoveryTimeout('Deadline reached before service start')
            # Switch/audit can take time; recheck the barrier before starting.
            wait_quiescent(ops.resource_snapshot, ops.record,
                           min(deadline, clock() + 30), clock=clock, pause=pause)
            ops.start()
            result = ops.wait_ready(old, min(90, max(0, deadline - clock())))
            if clock() < deadline:
                return result
        except Exception as error:
            last = repr(error)
            ops.record('recovery-failure', {'attempt': attempt, 'error': last})
        pause(min(1, max(0, deadline - clock())))
    raise RecoveryTimeout('Recovery deadline exhausted: ' + str(last))
