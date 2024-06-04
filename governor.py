import os
import sys
import json
import copy
import time
import signal
import argparse
import subprocess

# Windows specific imports
import win32gui
import win32process
import win32con
import ctypes

__doc__ = """
This script is used to run FSR tests. It starts the renderer and relay processes
and collects metrics from them and the GPU. The metrics are then printed to the
console in JSON format.
"""

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
FSR_DIR = os.path.join(SCRIPT_DIR, "bin")

UPSCALERS = [
    "Native",
    "Point",
    "Bilinear",
    "Bicubic",
    "FSR1",
    "FSR2",
    "FSR3Upscale",
    "FSR3",
    "DLSSUpscale",
    "DLSS",
]

# Get the default configf
with open(os.path.join(FSR_DIR, "configs/fsrconfig.json"), "r", encoding="utf-8") as f:
    config = json.load(f)


# Prepare the config
def get_config(
    mode,
    render_res=(1920, 1080),
    present_res=(2560, 1440),
    scene="Sponza",
    upscaler="FSR3",
):
    tmp = copy.deepcopy(config)

    # Apply mode specific settings
    tmp["FidelityFX FSR"]["Remote"]["Mode"] = mode
    if mode == "Relay":
        tmp["FidelityFX FSR"]["FPSLimiter"]["UseGPULimiter"] = False

    # Apply resolution settings
    tmp["FidelityFX FSR"]["Remote"]["RenderModuleOverrides"][mode][
        "FSRRemoteRenderModule"
    ] = {
        "Mode": mode,
        "RenderWidth": render_res[0],
        "RenderHeight": render_res[1],
    }

    # Apply present resolution settings
    tmp["FidelityFX FSR"]["Presentation"]["Width"] = present_res[0]
    tmp["FidelityFX FSR"]["Presentation"]["Height"] = present_res[1]

    # Apply scene settings
    if scene == "Sponza":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/SponzaNew/MainSponza.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "PhysCamera003"
    elif scene == "Brutalism":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/Brutalism/BrutalistHall.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "persp9_Orientation"
    else:
        raise ValueError("Invalid scene")

    # Apply upscaler settings
    tmp["FidelityFX FSR"]["Remote"]["StartupConfiguration"]["Upscaler"] = (
        UPSCALERS.index(upscaler)
    )

    return tmp


def apply_config(config):
    with open(
        os.path.join(FSR_DIR, "configs/fsrconfig.json"), "w", encoding="utf-8"
    ) as f:
        json.dump(config, f, indent=4)


def parse_args():
    parser = argparse.ArgumentParser(description="Run FSR tests")
    parser.add_argument(
        "--render-res",
        type=int,
        nargs=2,
        default=[1920, 1080],
        help="Render resolution (width height)",
    )
    parser.add_argument(
        "--present-res",
        type=int,
        nargs=2,
        default=[2560, 1440],
        help="Present resolution (width height)",
    )
    parser.add_argument(
        "--scene",
        type=str,
        default="Sponza",
        choices=["Sponza", "Brutalism"],
        help="Scene to use",
    )
    parser.add_argument(
        "--upscaler",
        type=str,
        default="DLSSUpscale",
        choices=UPSCALERS,
        help="Upscaler to use",
    )
    parser.add_argument(
        "--skip-relay",
        action="store_true",
        default=False,
        help="Skip the relay process",
    )
    return parser.parse_args()


def set_focus_by_pid(pid):
    def enum_windows_callback(hwnd, pid):
        if win32gui.IsWindowVisible(hwnd) and win32gui.IsWindowEnabled(hwnd):
            _, found_pid = win32process.GetWindowThreadProcessId(hwnd)
            title = win32gui.GetWindowText(hwnd)
            if found_pid == pid and title == "FidelityFX FSR":
                ctypes.windll.user32.SetForegroundWindow(hwnd)
                ctypes.windll.user32.ShowWindow(hwnd, win32con.SW_RESTORE)
                return True

    win32gui.EnumWindows(enum_windows_callback, pid)


if __name__ == "__main__":
    args = parse_args()

    # Create the renderer process
    renderer_config = get_config(
        "Renderer", args.render_res, args.present_res, args.scene, args.upscaler
    )
    apply_config(renderer_config)
    renderer = subprocess.Popen(
        [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe")],
        cwd=FSR_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    print(f"Renderer PID: {renderer.pid}")

    # Wait until the renderer is ready
    time.sleep(2)

    # Create the relay process
    relay_config = get_config(
        "Relay", args.render_res, args.present_res, args.scene, args.upscaler
    )
    apply_config(relay_config)
    if not args.skip_relay:
        relay = subprocess.Popen(
            [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe")],
            cwd=FSR_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        print(f"Relay PID: {relay.pid}")

    # Register signal handlers
    def cleanup(sig, frame):
        print("Cleaning up...")
        renderer.kill()
        if not args.skip_relay:
            relay.kill()
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # Set focus to the relay
    if not args.skip_relay:
        time.sleep(2)
        set_focus_by_pid(relay.pid)

    dots = 0
    while True:
        # Check if the renderer is still running
        if renderer.poll() is not None:
            break

        # Check if the relay is still running
        if not args.skip_relay and relay.poll() is not None:
            break

        print(
            f"Waiting for process(es) to finish{'.' * dots + ' ' * (4 - dots)}",
            end="\r",
        )
        time.sleep(0.5)
        dots = (dots + 1) % 4

    print()
    print("Process(es) finished")
    cleanup(None, None)
