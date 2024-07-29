import os
import sys
import json
import copy
import time
import signal
import argparse
import subprocess
from glob import glob
from datetime import datetime, timezone

# Windows specific imports
import win32gui
import win32api
import win32process
import win32con
import ctypes

# Other
from skimage import img_as_float, io
from skimage.metrics import (
    peak_signal_noise_ratio,
    structural_similarity,
    mean_squared_error,
)

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

# List of upscalers that require jitter, otherwise it should be disabled
NEEDS_JITTER = ["FSR2", "FSR3Upscale", "FSR3", "DLSSUpscale", "DLSS"]

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


def utcnow_iso8601():
    return datetime.now(timezone.utc).isoformat()


# Prepare the config
def get_config(mode, opts):
    tmp = copy.deepcopy(config)

    # Apply mode specific settings
    tmp["FidelityFX FSR"]["Remote"]["Mode"] = mode

    # Apply present resolution settings
    tmp["FidelityFX FSR"]["Presentation"]["Width"] = opts.present_res[0]
    tmp["FidelityFX FSR"]["Presentation"]["Height"] = opts.present_res[1]

    # Apply FPS settings
    tmp["FidelityFX FSR"]["FPSLimiter"]["UseReflex"] = False  # Not operational
    tmp["FidelityFX FSR"]["FPSLimiter"][
        "UseGPULimiter"
    ] = False  # Messes with GPU metrics

    if mode != "Upscaler":
        assert opts.fps == 60 or opts.fps == 30, "FPS must be 60 or 30"
        tmp["FidelityFX FSR"]["FPSLimiter"]["TargetFPS"] = opts.fps
    else:
        tmp["FidelityFX FSR"]["FPSLimiter"][
            "TargetFPS"
        ] = 60  # We always run the upscaler at 60 FPS

    # Set the scene exposure
    tmp["FidelityFX FSR"]["Content"]["SceneExposure"] = opts.scene_exposure

    # Apply scene settings
    if opts.scene == "Sponza":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/SponzaNew/MainSponza.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "PhysCamera003"
    elif opts.scene == "Brutalism":
        tmp["FidelityFX FSR"]["Content"]["Scenes"] = [
            "../media/Brutalism/BrutalistHall.gltf"
        ]
        tmp["FidelityFX FSR"]["Content"]["Camera"] = "persp9_Orientation"
    else:
        raise ValueError("Invalid scene")

    # Apply reduced motion settings
    if opts.reduced_motion:
        tmp["FidelityFX FSR"]["Content"]["ParticleSpawners"] = []
        tmp["FidelityFX FSR"]["Remote"]["RenderModules"]["Default"].remove(
            "AnimatedTexturesRenderModule"
        )
        tmp["FidelityFX FSR"]["Remote"]["RenderModules"]["Renderer"].remove(
            "AnimatedTexturesRenderModule"
        )

    # Remove content on upscaler if running in detached mode
    if mode == "Upscaler":
        del tmp["FidelityFX FSR"]["Content"]["Scenes"]
        del tmp["FidelityFX FSR"]["Content"]["Camera"]
        del tmp["FidelityFX FSR"]["Content"]["DiffuseIBL"]
        del tmp["FidelityFX FSR"]["Content"]["SpecularIBL"]

    # Apply upscaler settings
    tmp["FidelityFX FSR"]["Remote"]["Upscaler"] = UPSCALERS.index(opts.upscaler)

    # Apply render settings
    tmp["FidelityFX FSR"]["Render"] = {
        "EnableJitter": opts.upscaler in NEEDS_JITTER,
        "InitialRenderWidth": opts.render_res[0],
        "InitialRenderHeight": opts.render_res[1],
    }

    # Apply DLSS settings
    if "DLSS" in opts.upscaler:
        tmp["FidelityFX FSR"]["Remote"]["RenderModuleOverrides"]["Default"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(opts.dlssMode) + 1)
        tmp["FidelityFX FSR"]["Remote"]["RenderModuleOverrides"]["Upscaler"][
            "DLSSUpscaleRenderModule"
        ]["mode"] = (DLSS_MODES.index(opts.dlssMode) + 1)

    return tmp


def apply_config(config_data):
    with open(
        os.path.join(FSR_DIR, "configs/fsrconfig.json"), "w", encoding="utf-8"
    ) as config_file:
        json.dump(config_data, config_file, indent=4)


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
        "--scene-exposure",
        type=float,
        default=1.355,
        help="Scene exposure",
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
    parser.add_argument(
        "--structured-logs",
        action="store_true",
        default=False,
        help="Enable structured logs",
    )
    parser.add_argument(
        "--compare",
        type=str,
        choices=["ssim", "psnr", "mse"],
        help="Enable comparison with the reference image, will run the same configuration but without upscaler",
    )
    parser.add_argument(
        "--switch-default-mode",
        action="store_true",
        default=False,
        help="In compare mode, switch the default mode when running the reference image",
    )
    parser.add_argument(
        "--compare-with",
        type=str,
        help="Compare with the given upscaler",
        default="Native",
        choices=UPSCALERS,
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


def main(opts):
    # Default process arguments
    process_args = [
        "-screenshot",
        "-displaymode",
        "DISPLAYMODE_LDR",
        "-benchmark",
        "json",
    ]

    # For Native rendering, match the render resolution to the present resolution
    if opts.upscaler == "Native":
        if opts.render_res != opts.present_res:
            if opts.structured_logs:
                raise ValueError(
                    "Native rendering is enabled, but the render resolution does not match the present resolution."
                )
            print(
                "Warning: Native rendering is enabled, but the render resolution does not match the present resolution. The render resolution will be set to the present resolution."
            )
            opts.render_res = opts.present_res

    # Create the renderer process
    mode = "Default" if opts.use_default else "Renderer"
    renderer_config = get_config(mode, opts)
    apply_config(renderer_config)
    renderer = subprocess.Popen(
        [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe"), *process_args],
        cwd=FSR_DIR,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        stdin=subprocess.DEVNULL,
    )
    if opts.structured_logs:
        print("RENDERER_PID", renderer.pid)
    else:
        print(f"Renderer PID: {renderer.pid}")

    # Default mode implies skipping the upscaler
    if opts.use_default:
        opts.skip_upscaler = True

    # Wait until the renderer is ready
    if not opts.use_default:
        time.sleep(2)

        # Create the upscaler process
        upscaler_config = get_config("Upscaler", opts)
        apply_config(upscaler_config)
        # We do not encapsulate config creation in the following
        # if statement because we may use Visual Studio to debug the upscaler

    if not opts.skip_upscaler:
        upscaler = subprocess.Popen(
            [os.path.join(FSR_DIR, "FFX_FSR_NATIVE_DX12D.exe"), *process_args],
            cwd=FSR_DIR,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        if opts.structured_logs:
            print("UPSCALER_PID", upscaler.pid)
        else:
            print(f"Upscaler PID: {upscaler.pid}")

    # Register signal handlers
    def cleanup(sig, _):
        if not opts.structured_logs:
            print()
            print("Cleaning up...")

        # Try to gracefully close the processes
        if renderer.poll() is None:
            close_by_pid(renderer.pid)

        if not opts.skip_upscaler and upscaler.poll() is None:
            close_by_pid(upscaler.pid)

        # Wait for the processes to close
        start_time = time.time()
        while (
            renderer.poll() is None
            or (not opts.skip_upscaler and upscaler.poll() is None)
        ) and time.time() - start_time < 10:
            time.sleep(0.1)

        # Kill the processes if they are still running
        if renderer.poll() is None:
            renderer.kill()

        if not opts.skip_upscaler and upscaler.poll() is None:
            upscaler.kill()

        if not opts.compare:
            sys.exit(0)

        if opts.compare and sig is not None:
            print(
                "In comparison mode, exiting is not allowed. You must wait for the test to finish."
            )

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # Focus the correct window
    if not opts.skip_upscaler:
        time.sleep(2)
        assert focus_by_pid(
            renderer.pid if opts.use_default else upscaler.pid
        ), "Could not focus the window"

    dots = 0
    test_start_time = time.time()
    if opts.structured_logs:
        print("TEST_START", utcnow_iso8601())
    while True:
        # Check if the renderer is still running
        if renderer.poll() is not None:
            break

        # Check if the upscaler is still running
        if not opts.skip_upscaler and upscaler.poll() is not None:
            break

        # Check if the benchmark time has passed
        if opts.benchmark > 0 and time.time() - test_start_time > opts.benchmark:
            break

        if not opts.structured_logs:
            print(
                f"Waiting for process(es) to finish{'.' * dots + ' ' * (4 - dots)}",
                end="\r",
            )
            dots = (dots + 1) % 4
        time.sleep(0.5)

    if opts.structured_logs:
        print("TEST_END", utcnow_iso8601())
    else:
        print()
        print("Process(es) finished")
    cleanup(None, None)

    return renderer.pid if opts.use_default else upscaler.pid


def load_image_for_pid(pid):
    image_path = glob(os.path.join(FSR_DIR, "benchmark", f"*_{pid}_*.jpg"))
    assert len(image_path) == 1, "Could not find the image"
    return img_as_float(io.imread(image_path[0]))


if __name__ == "__main__":
    args = parse_args()

    if args.benchmark > 0:
        assert args.benchmark >= 5, "Benchmark duration must be at least 5 seconds"

    # Run with the requested arguments
    test_pid = main(args)

    if args.compare:
        # Run the same configuration without the upscaler
        compare_args = parse_args()
        compare_args.upscaler = args.compare_with
        if compare_args.switch_default_mode:
            compare_args.use_default = not args.use_default
        ref_pid = main(compare_args)

        # Load the images
        test_image = load_image_for_pid(test_pid)
        ref_image = load_image_for_pid(ref_pid)

        # Calculate the metrics
        if args.compare == "ssim":
            metric = structural_similarity(
                test_image, ref_image, data_range=1, channel_axis=2
            )
        elif args.compare == "psnr":
            metric = peak_signal_noise_ratio(test_image, ref_image, data_range=1)
        elif args.compare == "mse":
            metric = mean_squared_error(test_image, ref_image)
        else:
            raise ValueError("Invalid metric")

        print(f"{args.compare.upper()}: {metric:.6f}")
