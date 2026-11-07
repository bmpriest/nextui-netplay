"""Netplay's spruce front end. All session work stays in netplay.elf."""

from __future__ import annotations

import logging
import math
import os
import subprocess
import sys
import threading
from pathlib import Path

CARD = Path(os.environ.get("SDCARD_PATH", "/mnt/SDCARD"))
PAK = Path(os.environ.get("NETPLAY_PAK", CARD / "App/Netplay"))
PLATFORM = os.environ["PLATFORM"]
ENGINE = PAK / "bin" / PLATFORM / "netplay.elf"
STATE = Path(os.environ.get("NETPLAY_STATE", CARD / "Saves/netplay"))
HINT = "B BACK    A SELECT"
FOOTER_CONTEXT = None
GLYPH_SCRATCH = None
CORE_RESULTS: dict[str, tuple[bool, str]] = {}
CORE_READY = threading.Event()


def control(*args: str) -> str:
    env = os.environ.copy()
    if engine_libraries := env.get("NETPLAY_ENGINE_LD_LIBRARY_PATH"):
        env["LD_LIBRARY_PATH"] = engine_libraries
    result = subprocess.run(
        [str(ENGINE), "--ctl", *args], capture_output=True, text=True, check=False, env=env
    )
    if result.returncode:
        raise RuntimeError(result.stdout.strip() or result.stderr.strip() or "Netplay command failed")
    return result.stdout


def fields(text: str) -> dict[str, str]:
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def warm_cores() -> None:
    """Read spruce's core choices while the user is still on the main screen."""
    def read():
        try:
            CORE_RESULTS["all"] = (True, control("cores"))
        except Exception as error:
            CORE_RESULTS["all"] = (False, str(error))
        finally:
            CORE_READY.set()

    threading.Thread(target=read, daemon=True).start()


def bootstrap() -> None:
    main_ui = CARD / "App/PyUI/main-ui"
    sys.path.insert(0, str(main_ui))
    from utils.logger import PyUiLogger
    from utils.py_ui_config import PyUiConfig
    from utils.py_ui_state import PyUiState
    from utils.cfw_system_config import CfwSystemConfig
    from devices.miyoo.user_config import UserConfig
    from devices.device import Device
    from menus.language.language import Language
    from themes.theme import Theme
    from display.display import Display
    from controller.controller import Controller

    PyUiLogger._logger = logging.getLogger("netplay.pyui")
    PyUiConfig.init(str(CARD / "App/PyUI/py-ui-config.json"))
    if os.environ.get("NETPLAY_PYUI_USER_CONFIG"):
        UserConfig.FILEPATH = os.environ["NETPLAY_PYUI_USER_CONFIG"]
    UserConfig.reload_config()
    CfwSystemConfig.init(str(CARD / "Saves/spruce/spruce-config.json"))
    Language.init()
    try:
        from devices.device_factory import initialize_device
    except ModuleNotFoundError as error:
        if error.name != "devices.device_factory":
            raise
        from mainui import initialize_device
    initialize_device(os.environ["NETPLAY_PYUI_DEVICE"], main_ui_mode=False)
    device = Device.get_device()
    device.restore_saved_timezone()
    state_file = STATE / "pyui-state.json"
    if not state_file.exists():
        state_file.write_text("{}\n", encoding="utf-8")
    PyUiState.init(str(state_file))
    theme = device.get_system_config().get_theme()
    Theme.init(str(Path(PyUiConfig.get("themeDir")) / theme), device.screen_width(), device.screen_height())
    Display.init()
    Display.present()
    Controller.init()

    from . import generated
    import tempfile
    global GLYPH_SCRATCH
    GLYPH_SCRATCH = tempfile.TemporaryDirectory(prefix="netplay-ui-")
    generated.use_scratch(Path(GLYPH_SCRATCH.name))
    original = Display.bottom_bar.render_bottom_bar

    def bottom_bar(bottom_bar_text=None, render_bottom_bar_icons_and_images=True):
        from . import footer
        # Leave native keyboard prompts alone when no Netplay menu owns them.
        if FOOTER_CONTEXT is None and bottom_bar_text is not None:
            return original(bottom_bar_text, render_bottom_bar_icons_and_images)
        original(bottom_bar_text=None, render_bottom_bar_icons_and_images=False)
        left = FOOTER_CONTEXT() if FOOTER_CONTEXT else footer.hints(HINT)
        footer.render(left, session_active() and HINT != "PLEASE WAIT",
                      HINT if not left else "")

    Display.bottom_bar.render_bottom_bar = bottom_bar


class SessionEnded(Exception):
    """Unwind nested menus after the global X action."""


def session_active() -> bool:
    return (STATE / "session").is_file()


def menu(title: str, rows: list[tuple[str, str, str, str]], hint: str = "A SELECT    B BACK",
         selected: int = 0, extra: tuple[str, ...] = (),
         actions: dict[str, str | None] | None = None, back: str = "BACK") -> tuple[str, str] | None:
    global HINT, FOOTER_CONTEXT
    from controller.controller_inputs import ControllerInput
    from views.grid_or_list_entry import GridOrListEntry
    from views.view_creator import ViewCreator
    from views.view_type import ViewType

    previous_hint = HINT
    HINT = hint
    entries = [GridOrListEntry(label, value_text=value or None, description=description or None,
                               value=key) for key, label, description, value in rows]
    if not entries:
        entries = [GridOrListEntry("Nothing found", description="B to go back", value="empty")]
    view = ViewCreator.create_view(ViewType.ICON_AND_DESC, entries, title, selected_index=selected)
    from .buttons import Button
    from .footer import hints

    def contextual_hints():
        key = rows[min(max(int(view.selected), 0), len(rows) - 1)][0] if rows else "empty"
        if actions is None:
            left = hints(hint)
        else:
            action = actions.get(key, "SELECT")
            left = [(Button.B, back)] + ([(Button.A, action)] if action else [])
        # Back/Exit is contextual even for ordinary selection lists.
        return [(button, back if button == Button.B else label) for button, label in left]

    previous = FOOTER_CONTEXT
    FOOTER_CONTEXT = contextual_hints
    accepted = [ControllerInput.A, ControllerInput.B, ControllerInput.X] + [ControllerInput[name] for name in extra]
    try:
        while True:
            selection = view.get_selection(accepted)
            pressed = selection.get_input()
            if pressed == ControllerInput.X:
                if session_active() and attempt("Netplay", "Ending session", "end"):
                    raise SessionEnded()
                continue
            if pressed == ControllerInput.B:
                return None
            if pressed == ControllerInput.A and actions is not None:
                key = str(selection.get_selection().value)
                if not actions.get(key, "SELECT"):
                    continue
            if pressed in accepted and pressed != ControllerInput.B:
                return str(selection.get_selection().value), pressed.name
    finally:
        HINT, FOOTER_CONTEXT = previous_hint, previous
        finished = getattr(view, "view_finished", None)
        if finished:
            finished()


def message(title: str, detail: str) -> bool:
    return menu(title, [("ok", "OK", detail, "")], "A OK    B BACK") is not None


def confirm(title: str, detail: str) -> bool:
    choice = menu(title, [("yes", "Yes", detail, ""), ("no", "No", "", "")])
    return choice is not None and choice[0] == "yes"


def working(title: str, description: str, action):
    """Animate the theme's working page while native networking runs."""
    global HINT, FOOTER_CONTEXT
    from controller.controller import Controller
    from devices.device import Device
    from display.display import Display
    from display.font_purpose import FontPurpose
    from display.render_mode import RenderMode
    from themes.theme import Theme

    previous_hint, previous_context = HINT, FOOTER_CONTEXT
    HINT = "PLEASE WAIT"
    FOOTER_CONTEXT = None
    try:
        result = []

        def run():
            try:
                result.append((True, action()))
            except Exception as error:
                result.append((False, str(error)))

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        frame = 0
        while thread.is_alive():
            Display.clear(title)
            width = int(Device.get_device().screen_width())
            top = Display.get_top_bar_height()
            bottom = Display.get_bottom_bar_height()
            center_y = top + (int(Device.get_device().screen_height()) - top - bottom) // 2
            color = Theme.text_color(FontPurpose.DESCRIPTIVE_LIST_TITLE)
            for dot in range(8):
                angle = math.tau * (dot / 8)
                x = width // 2 + int(math.cos(angle) * 19)
                y = center_y - 13 + int(math.sin(angle) * 19)
                if dot == frame % 8 or dot == (frame - 1) % 8:
                    Display.render_box(color, x, y, 5, 5)
            Display.render_text(description, width // 2, center_y + 38, color,
                                FontPurpose.DESCRIPTIVE_LIST_TITLE, RenderMode.TOP_CENTER_ALIGNED)
            Display.present()
            Controller.get_input(timeout=0.08)
            frame += 1
        thread.join()
        if not result[0][0]:
            raise RuntimeError(result[0][1])
        return result[0][1]

    finally:
        HINT, FOOTER_CONTEXT = previous_hint, previous_context


def attempt(title: str, detail: str, *args: str) -> bool:
    try:
        answer = working(title, detail, lambda: control(*args))
    except Exception as error:
        message(title, str(error))
        return False
    if answer.startswith("error "):
        message(title, answer[6:])
        return False
    return True


def onboarding() -> bool:
    marker = STATE / "onboarding-complete"
    if marker.exists():
        return True
    import json
    from controller.controller import Controller
    from controller.controller_inputs import ControllerInput
    from devices.device import Device
    from display.display import Display
    from display.font_purpose import FontPurpose
    from display.render_mode import RenderMode
    from themes.theme import Theme

    pages = json.loads((PAK / "pak_onboarding.json").read_text()).get("pages", [])
    if not pages:
        return True
    settings_now = fields(control("settings"))
    choices = {}
    for index, page in enumerate(pages):
        if page.get("setting"):
            current = settings_now.get(page["setting"], "")
            choices[index] = next((i for i, option in enumerate(page["options"])
                                   if str(option["value"]) == current), 0)
    width = int(Device.get_device().screen_width())
    height = int(Device.get_device().screen_height())
    page_index = 0
    while page_index < len(pages):
        page = pages[page_index]
        has_options = page_index in choices
        global HINT
        HINT = "A NEXT    B BACK    LEFT/RIGHT CHOOSE" if has_options else (
            "A FINISH    B BACK" if page_index == len(pages) - 1 else "A NEXT    B BACK"
        )
        Display.clear("")
        top = Display.get_top_bar_height()
        bottom = height - Display.get_bottom_bar_height()
        usable = bottom - top
        color = Theme.text_color(FontPurpose.LIST)
        Display.render_text(page["title"], width // 2, top + int(usable * 0.14),
                            color, FontPurpose.MESSAGE,
                            RenderMode.MIDDLE_CENTER_ALIGNED)
        lines = Display.split_message(page["description"], FontPurpose.LIST,
                                      clip_to_device_width=True)
        Display.write_message_multiline(lines, top + int(usable * (0.44 if has_options else 0.51)))
        if has_options:
            options = page["options"]
            for i, option in enumerate(options):
                x = width * (i + 1) // (len(options) + 1)
                picked = choices[page_index] == i
                label = f"< {option['label']} >" if picked else option["label"]
                Display.render_text(label, x, top + int(usable * 0.76),
                                    Theme.text_color_selected(FontPurpose.LIST) if picked else color,
                                    FontPurpose.LIST, RenderMode.MIDDLE_CENTER_ALIGNED)
        dots_y = bottom - max(20, int(usable * 0.08))
        step = max(18, min(28, width // 24))
        first_x = width // 2 - (len(pages) - 1) * step // 2
        for i in range(len(pages)):
            Display.render_text("●" if i == page_index else "○", first_x + i * step,
                                dots_y, color, FontPurpose.LIST, RenderMode.MIDDLE_CENTER_ALIGNED)
        Display.present()
        if not Controller.get_input():
            continue
        pressed = Controller.last_input()
        if pressed == ControllerInput.X and session_active():
            attempt("Netplay", "Ending session", "end")
        elif pressed == ControllerInput.B:
            if page_index == 0:
                return False
            page_index -= 1
        elif has_options and pressed in (ControllerInput.DPAD_LEFT, ControllerInput.DPAD_RIGHT):
            delta = -1 if pressed == ControllerInput.DPAD_LEFT else 1
            choices[page_index] = (choices[page_index] + delta) % len(page["options"])
        elif pressed == ControllerInput.A:
            page_index += 1
    for index, selected in choices.items():
        page = pages[index]
        if not attempt(page["title"], "Saving choice", "set", page["setting"],
                       str(page["options"][selected]["value"])):
            return False
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text("1\n")
    return True


def host() -> None:
    status = fields(control("status"))
    choices = []
    if status.get("can_host_adhoc") == "1":
        choices.append(("adhoc", "Create ad hoc network", "Fast local connection", ""))
    choices.append(("wifi", "Host over WiFi", "Use the current network", ""))
    choice = menu("Host", choices)
    if choice:
        attempt("Hosting", "Checking and starting session", "host", choice[0])


def join() -> None:
    while True:
        if not join_once():
            return


def join_once() -> bool:
    try:
        result = working("Join", "Looking for hosts", lambda: control("peers", "3"))
    except Exception as error:
        message("Join", str(error))
        return False
    peers = [parts for line in result.splitlines() if line.startswith("peer\t")
             if len(parts := line.split("\t")) >= 8]
    rows = []
    for i, peer in enumerate(peers):
        _, ip, nick, platform, protocol, hotspot, ssid, _psk = peer[:8]
        label = nick or ssid or ip
        rows.append((str(i), label, f"{platform} · {'Ad hoc' if hotspot == '1' else ip}",
                     "RetroArch" if protocol == "1" else "Linkplay"))
    if not rows:
        message("Join", "No hosts found. Check Wi-Fi and try again.")
        return False
    choice = menu("Join", rows, "A JOIN    B BACK    Y RESCAN", extra=("Y",))
    if choice is None:
        return False
    if choice[1] == "Y":
        return True
    peer = peers[int(choice[0])]
    _, ip, _nick, _platform, protocol, hotspot, ssid, psk = peer[:8]
    args = ("join-peer", ip, "retroarch" if protocol == "1" else "linkplay")
    if hotspot == "1":
        args += (ssid, psk)
    attempt("Joining", "Connecting and checking cores", *args)
    return False


def settings() -> None:
    while True:
        values = fields(control("settings"))
        rows = [
            ("host_protocol", "Host protocol", "Protocol used for the next session",
             "RetroArch" if values.get("host_protocol") == "retroarch" else "Linkplay"),
            ("hybrid_network", "Hybrid Network Traffic", "UDP inputs and hashes; applies to the next session",
             "On" if values.get("hybrid_network", "1") == "1" else "Off"),
            ("compatibility_cores", "Compatibility cores", "Use matched cores when builds differ",
             "On" if values.get("compatibility_cores") == "1" else "Off"),
            ("disable_powersave", "Disable Wi-Fi Power Saving", "May improve connection timing",
             "On" if values.get("disable_powersave") == "1" else "Off"),
        ]
        choice = menu("Settings", rows, actions={row[0]: "CHANGE" for row in rows})
        if choice is None:
            return
        key = choice[0]
        if key == "host_protocol":
            value = "linkplay" if values[key] == "retroarch" else "retroarch"
        else:
            value = "0" if values.get(key, "1" if key == "hybrid_network" else "0") == "1" else "1"
            if key == "disable_powersave" and value == "1" and not confirm(
                "Wi-Fi Power Saving", "This may reload the Wi-Fi driver for several seconds."
            ):
                continue
        if key == "disable_powersave":
            attempt("Settings", "Applying change", "powersave", "off" if value == "1" else "on")
        else:
            attempt("Settings", "Applying change", "set", key, value)


def core_rows(output: str) -> tuple[list[tuple[str, str, str, str]], dict[str, str | None]]:
    """Combine spruce systems, keeping readiness separate from core names."""
    values = fields(output)
    descriptions = {p[1]: p[2] for line in output.splitlines()
                    if len(p := line.split("\t")) == 3 and p[0] == "core_hint"}
    names = {"FC": "FCEUmm", "SFC": "Snes9x", "MD": "PicoDrive", "PS": "PCSX ReARMed",
             "32X": "PicoDrive", "FDS": "FCEUmm", "GG": "PicoDrive", "SMS": "PicoDrive",
             "SG1000": "PicoDrive", "SEGACD": "PicoDrive"}
    rows, actions, ds = [], {}, []
    for line in output.splitlines():
        parts = line.split("\t")
        if len(parts) != 6 or parts[0] != "core": continue
        _, _, tag, label, state, more = parts
        if tag in ("NDS", "DSP"):
            ds.append((tag, state)); continue
        paired = state in ("dual", "paired", "instanced")
        mark = "✓✓" if paired else "✓" if state == "ready" else "○" if state in ("present", "installed", "off") else "–"
        detail = descriptions.get(tag, names.get(tag, tag))
        if tag == "GB": detail = "Gambatte" + (", Gambatte Dual" if paired else "")
        elif tag == "GBA": detail = "gpSP (Wireless Adapter, AW/Gen 3 Pkmn)" + (", mGBA Dual" if paired else "")
        rows.append((tag, label, detail, mark))
        available = values.get("gambatte_dual" if tag == "GB" else "mgba_dual") == "1"
        actions[tag] = "CHANGE" if tag in ("GB", "GBA") and more == "1" and available else None
    if ds:
        ready = any(state == "ready" for _, state in ds)
        present = any(state not in ("absent", "unavailable") for _, state in ds)
        engines = ["DraStic" if tag == "NDS" else "DSperate" for tag, state in ds if state not in ("absent", "unavailable")]
        rows.append(("NDS", "NDS", ", ".join(engines) or "DraStic, DSperate", "✓" if ready else "○" if present else "–"))
        actions["NDS"] = None
    return sorted(rows, key=lambda row: row[0]), actions


def cores() -> None:
    try:
        if not CORE_READY.is_set(): working("Cores", "Checking cores", CORE_READY.wait)
        ok, output = CORE_RESULTS["all"]
        if not ok: output = working("Cores", "Checking cores", lambda: control("cores"))
    except Exception as error:
        message("Cores", str(error)); return
    selected = 0
    while True:
        rows, actions = core_rows(output)
        picked = menu("Cores", rows, selected=selected, actions=actions)
        if picked is None: return
        tag = picked[0]
        selected = next(i for i, row in enumerate(rows) if row[0] == tag)
        setting = "inst_gambatte" if tag == "GB" else "inst_mgba"
        current = "1" if rows[selected][3] == "✓✓" else "0"
        if confirm(tag, "Turn paired play " + ("off" if current == "1" else "on") + "?"):
            if attempt(tag, "Saving core choice", "set", setting, "0" if current == "1" else "1"):
                output = working("Cores", "Checking cores", lambda: control("cores"))
                CORE_RESULTS["all"] = (True, output)


def debug() -> None:
    while True:
        values = fields(control("settings"))
        rows = [(key, label, detail, "On" if values.get(key) == "1" else "Off") for key, label, detail in (
            ("force_compatibility", "Force compatibility cores", "Testing option"),
            ("verbose_logs", "Verbose debugging logs", "Keep game launch logs"),
            ("session_save_backups", "Session save backups", "Keep three original save snapshots"),
        )]
        rows += [("checks", "Run checks", "Bindings, paths, cores and network", ""),
                 ("restore", "Restore Wi-Fi", "Reconnect to your usual network", "")]
        choice = menu("Debug", rows, actions={row[0]: "RUN" if row[0] == "checks" else "RESTORE" if row[0] == "restore" else "CHANGE" for row in rows})
        if choice is None:
            return
        key = choice[0]
        if key == "checks":
            try:
                result = working("Checks", "Checking device", lambda: control("checks"))
                checks = [line.split("\t", 3) for line in result.splitlines() if line.startswith("check\t")]
                menu("Checks", [(str(i), check[2], check[3], ("OK", "Warn", "Fail")[int(check[1])])
                                for i, check in enumerate(checks)], actions={str(i): None for i in range(len(checks))})
            except Exception as error:
                message("Checks", str(error))
        elif key == "restore":
            if confirm("Restore Wi-Fi", "Reconnect to your usual network?"):
                attempt("Wi-Fi", "Restoring connection", "restore-wifi")
        else:
            attempt("Debug", "Saving setting", "set", key, "0" if values.get(key) == "1" else "1")


def configuration() -> None:
    while True:
        values = fields(control("settings"))
        rows = [("nickname", "Nickname", "Shown to other players", values.get("nickname", "")),
                ("cores", "Cores", "Supported systems and paired play", ""),
                ("settings", "Settings", "Protocol and connection options", ""),
                ("debug", "Debug", "Checks, logs and backups", ""),
                ("off", "Turn Off Netplay", "End the session and restore ordinary emulator launches", "")]
        choice = menu("Configuration", rows, actions={"nickname": "CHANGE", "off": "TURN OFF"})
        if choice is None:
            return
        key = choice[0]
        if key == "nickname":
            from display.on_screen_keyboard import OnScreenKeyboard
            nickname = OnScreenKeyboard().get_input("Nickname", values.get("nickname_custom", ""))
            if nickname is not None:
                attempt("Nickname", "Saving nickname", "set", "nickname", str(nickname)[:31])
        elif key == "cores":
            cores()
        elif key == "settings":
            settings()
        elif key == "debug":
            debug()
        elif confirm("Turn Off Netplay", "Games will launch normally until you host or join again."):
            attempt("Netplay", "Turning off", "off")
            return



def main_menu() -> bool:
    status = fields(control("status"))
    if status.get("armed") == "1":
        detail = "Hosting" if status.get("role") == "host" else "Joined " + status.get("host", "")
        rows = [("session", "Session status", detail, ""),
                ("config", "Configuration", "Nickname, cores and settings", ""),
                ("end", "End session", "Return to normal play", "")]
    else:
        rows = [("host", "Host", "Start a session", ""),
                ("join", "Join", "Find someone hosting", ""),
                ("config", "Configuration", "Nickname, cores and settings", "")]
    choice = menu("Netplay", rows, back="EXIT", actions={"session": "VIEW", "end": "END SESSION"})
    if choice is None:
        return False
    key = choice[0]
    if key == "host": host()
    elif key == "join": join()
    elif key == "config": configuration()
    elif key == "end" and confirm("End session", "Disconnect and restore normal play?"):
        attempt("Netplay", "Ending session", "end")
    elif key == "session":
        guests = status.get("guests", "0")
        message("Session", detail + (f" · {guests} guest(s)" if status.get("role") == "host" else ""))
    return True


def run() -> None:
    if not onboarding(): return
    while True:
        try:
            if not main_menu(): return
        except SessionEnded:
            continue


def main() -> int:
    STATE.mkdir(parents=True, exist_ok=True)
    logging.basicConfig(filename=str(STATE / "pyui.log"), level=logging.INFO)
    try:
        bootstrap()
        warm_cores()
        run()
        return 0
    except Exception:
        logging.exception("PyUI front end failed")
        return 1


if __name__ == "__main__":
    sys.exit(main())
