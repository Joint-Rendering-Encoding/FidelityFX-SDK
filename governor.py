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
import win32api
import win32process
import win32con
import ctypes

__doc__ = """
This script is used to run FSR tests. It starts the renderer and upscaler processes
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

DLSS_MODES = [
    "Performance",
    "Balanced",
    "Quality",
    "UltraPerformance",
]

# Get the default config
with open(
    os.path.join(SCRIPT_DIR, "samples/fsr/config/fsrconfig.json"), "r", encoding="utf-8"
) as f:
    config = json.load(f)


# Prepare the config
def get_config(mode, args):
    tmp = copy.deepcopy(config)

    # Apply mode specific settings
    tmp["FidelityFX FSR"]["Remote"]["Mode"] = mode

    # Apply present resolution settings
    tmp["FidelityFX FSR"]["Presentation"]["Width"] = args.present_res[0]
    tmp["FidelityFX FSR"]["Presentation"]["Height"] = args.present_res[1]

    # Apply FPS settings
    tmp["FidelityFX FSR"]["FPSLimiter"]["UseReflex"] = False  # Not operational
    tmp["FidelityFX FSR"]["FPSLimiter"][
        "UseGPULimiter"
    ] = False  # Messes with GPU metrics

    if mode != "Upscaler":
        assert args.fps == 60 or args.fps == 30, "FPS must be 60 or 30"
        tmp["FidelityFX FSR"]["FPSLimiter"]["TargetFPS"] = args.fps
    else:
        tmp["FidelityFX FSR"]["FPSLimiter"][
            "TargetFPS"
        ] = 60  # We always run the upscaler at 60 FPS

    # Apply scene settings
    if args.scene == "Sponza":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/SponzaNew/MainSponza.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "PhysCamera003"
    elif args.scene == "Brutalism":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/Brutalism/BrutalistHall.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "persp9_Orientation"
    else:
        raise ValueError("Invalid scene")

    # Apply reduced motion settings
    if args.reduced_motion:
        tmp["FidelityFX FSR"]["Content"]["ParticleSpawners"] = []
        tmp["FidelityFX FSR"]["Remote"]["RenderModules"]["Default"].remove(
            "AnimatedTexturesRenderModule"
        )
        tmp["FidelityFX FSR"]["Remote"]["RenderModules"]["Renderer"].remove(
            "AnimatedTexturesRenderModule"
        )

    # Apply upscaler settings
    tmp["FidelityFX FSR"]["Remote"]["StartupConfiguration"] = {
        "Upscaler": UPSCALERS.index(args.upscaler),
        "RenderWidth": args.render_res[0],
        "RenderHeight": args.render_res[1],
    }

    # Apply DLSS settings
    if "DLSS" in args.upscaler:
        tmp["FidelityFX FSR"]["Remote"]["RenderModuleOverrides"]["Default"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(args.dlssMode) + 1)
        tmp["FidelityFX FSR"]["Remote"]["RenderModuleOverrides"]["Upscaler"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(args.dlssMode) + 1)

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
        "--dlssMode",
        type=str,
        default="Performance",
        choices=DLSS_MODES,
        help="DLSS mode to use",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=60,
        help="The FPS to run the renderer at",
    )
    parser.add_argument(
        "--reduced-motion",
        action="store_true",
        default=False,
        help="Enable reduced motion mode, disables animated textures and particles",
    )
    parser.add_argument(
        "--benchmark",
        type=int,
        default=-1,
        help="Enable benchmark mode, sets the test duration in seconds",
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
        "--skip-upscaler",
        action="store_true",
        default=False,
        help="Skip the upscaler process",
    )
    parser.add_argument(
        "--use-default",
        action="store_true",
        default=False,
        help="Use the default config, without decoupling the upscaler",
    )
    return parser.parse_args()


def find_window_by_pid(pid, window_title="FidelityFX FSR"):
    def enum_windows_callback(hwnd, windows):
        if win32gui.IsWindowVisible(hwnd) and win32gui.IsWindowEnabled(hwnd):
            _, found_pid = win32process.GetWindowThreadProcessId(hwnd)
            if found_pid == pid:
                title = win32gui.GetWindowText(hwnd)
                if window_title is None or title == window_title:
                    windows.append(hwnd)

    windows = []
    win32gui.EnumWindows(enum_windows_callback, windows)
    return windows[0] if windows else None


def focus_by_pid(pid):
    hwnd = find_window_by_pid(pid)
    if hwnd:
        ctypes.windll.user32.SetForegroundWindow(hwnd)
        ctypes.windll.user32.ShowWindow(hwnd, win32con.SW_RESTORE)
        return True
    return False


def close_by_pid(pid):
    hwnd = find_window_by_pid(pid)
    if hwnd:
        win32api.PostMessage(hwnd, win32con.WM_QUIT, 0, 0)
        return True
    return False


if __name__ == "__main__":
    args = parse_args()

    # Default process arguments
    process_args = [
        "-screenshot",
        "-displaymode",
        "DISPLAYMODE_LDR",
        "-benchmark",
        "json",
    ]

    # Create the renderer process
    mode = "Default" if args.use_default else "Renderer"
    renderer_config = get_config(mode, args)
    apply_config(renderer_config)
    renderer = subprocess.Popen(
        [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe"), *process_args],
        cwd=FSR_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    print(f"Renderer PID: {renderer.pid}")

    # Default mode implies skipping the upscaler
    if args.use_default:
        args.skip_upscaler = True

    # Wait until the renderer is ready
    if not args.use_default:
        time.sleep(2)

        # Create the upscaler process
        upscaler_config = get_config("Upscaler", args)
        apply_config(upscaler_config)
        # We do not encapsulate config creation in the following
        # if statement because we may use Visual Studio to debug the upscaler

    if not args.skip_upscaler:
        upscaler = subprocess.Popen(
            [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe"), *process_args],
            cwd=FSR_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        print(f"Upscaler PID: {upscaler.pid}")

    # Register signal handlers
    def cleanup(sig, frame):
        print()
        print("Cleaning up...")
        # Try to gracefully close the processes
        if renderer.poll() is None:
            close_by_pid(renderer.pid)

        if not args.skip_upscaler and upscaler.poll() is None:
            close_by_pid(upscaler.pid)

        # Wait for the processes to close
        start_time = time.time()
        while (
            renderer.poll() is None
            or (not args.skip_upscaler and upscaler.poll() is None)
        ) and time.time() - start_time < 10:
            time.sleep(0.1)

        # Kill the processes if they are still running
        if renderer.poll() is None:
            renderer.kill()

        if not args.skip_upscaler and upscaler.poll() is None:
            upscaler.kill()

        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # Set focus to the upscaler
    if not args.skip_upscaler:
        time.sleep(2)
        assert focus_by_pid(upscaler.pid)

    dots = 0
    test_start_time = time.time()
    while True:
        # Check if the renderer is still running
        if renderer.poll() is not None:
            break

        # Check if the upscaler is still running
        if not args.skip_upscaler and upscaler.poll() is not None:
            break

        # Check if the benchmark time has passed
        if args.benchmark > 0 and time.time() - test_start_time > args.benchmark:
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
