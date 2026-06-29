"""Subprocess worker: applies the IROM2 sector_size patch, then runs
pyocd FileProgrammer on the given HEX file. Emits progress lines on
stdout that the parent webgui parses.

Run as:  python pyocd_flash_worker.py <uid> <chip> <freq_hz> <hex_path>

Output protocol (line-based, one JSON per line):
  {"event":"progress","phase":"Erasing","percent":47}
  {"event":"progress","phase":"Programming","percent":12}
  {"event":"done","ok":true,"message":"...","sectors":135,"bytes":552960}
  {"event":"done","ok":false,"message":"<error>"}

Lives in its own process so that pyocd's hid-library finalizer crash on
macOS 26.4.1 (rdar://hid_exit-IOHIDManagerClose-doesNotRecognizeSelector)
can't take down the webgui.
"""
import json
import sys
import logging


def emit(**kw):
    print(json.dumps(kw), flush=True)


def main():
    if len(sys.argv) != 5:
        emit(event="done", ok=False, message=f"usage: {sys.argv[0]} <uid> <chip> <freq_hz> <hex>")
        sys.exit(2)

    uid_arg, chip, freq_str, hex_path = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    freq_hz = int(freq_str)
    uid = uid_arg if uid_arg and uid_arg != '-' else None

    logging.basicConfig(level=logging.WARNING)

    try:
        import pyocd_nrf5340_fix
        from pyocd.core.helpers import ConnectHelper
        from pyocd.flash.file_programmer import FileProgrammer

        if not uid:
            for p in ConnectHelper.get_all_connected_probes(blocking=False):
                desc = (p.description or "") + " " + (getattr(p, 'product_name', '') or '')
                if 'edev_dap' in desc.lower():
                    uid = p.unique_id
                    break

        session = ConnectHelper.session_with_chosen_probe(
            unique_id=uid, target_override=chip,
            frequency=freq_hz, blocking=False,
        )
        session.open()
        try:
            pyocd_nrf5340_fix.fixup_session(session)
            # FileProgrammer's progress callback gets called as floats 0.0..1.0
            # for each phase. We piggy-back phase tracking via a closure.
            current_phase = {"name": "Erasing"}

            def cb(pct):
                emit(event="progress", phase=current_phase["name"],
                     percent=max(0, min(100, int(pct * 100))))

            prog = FileProgrammer(session,
                                  chip_erase='sector',
                                  smart_flash=False,
                                  progress=cb)
            # Hook the Flash class to detect when programming starts (after
            # erase completes). pyocd's progress callback covers the whole
            # operation as a single 0→100 range, so let's just relabel based
            # on what pyocd's loader.commit() internally reports.
            #
            # For simplicity here: we emit "Erasing" then switch to
            # "Programming" once percent has been seen at 100 once and resets.
            seen_full = {"v": False}
            def cb2(pct):
                p = max(0, min(100, int(pct * 100)))
                if p == 100 and not seen_full["v"]:
                    seen_full["v"] = True
                    emit(event="progress", phase=current_phase["name"], percent=100)
                    current_phase["name"] = "Programming"
                    return
                if seen_full["v"] and p < 5:
                    # Programming phase restarting at 0.
                    pass
                emit(event="progress", phase=current_phase["name"], percent=p)
            prog._progress = cb2
            prog.program(hex_path)
            emit(event="done", ok=True, message="flash complete")
        finally:
            try:
                session.close()
            except Exception:
                pass
    except Exception as e:
        emit(event="done", ok=False, message=f"{type(e).__name__}: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
