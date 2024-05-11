import os
import sys
import json
import copy
import time
import signal
import argparse
import subprocess
import psutil
import pynvml

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


def get_gpu_metrics():
    # Get the handle of the GPU you want to query
    handle = pynvml.nvmlDeviceGetHandleByIndex(0)

    # Get GPU information
    memory_info = pynvml.nvmlDeviceGetMemoryInfo(handle)
    utilization = pynvml.nvmlDeviceGetUtilizationRates(handle)
    power_usage = pynvml.nvmlDeviceGetPowerUsage(handle)
    clock_speed = pynvml.nvmlDeviceGetClockInfo(handle, 0)

    return {
        "memory": {
            "total": memory_info.total / (1024 * 1024),  # MB
            "used": memory_info.used / (1024 * 1024),  # MB
            "free": memory_info.free / (1024 * 1024),  # MB
        },
        "utilization": {
            "gpu": utilization.gpu / 100,  # %
            "memory": utilization.memory / 100,  # %
        },
        "power": power_usage / 1000,  # W
        "clock_speed": clock_speed,  # MHz
    }


if __name__ == "__main__":
    args = parse_args()

    # Initialize NVML
    pynvml.nvmlInit()

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
        pynvml.nvmlShutdown()
        renderer.kill()
        if not args.skip_relay:
            relay.kill()
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    # Collect live metrics
    renderer_process = psutil.Process(renderer.pid)
    if not args.skip_relay:
        relay_process = psutil.Process(relay.pid)

    def get_metrics_for(process):
        return {
            "cpu": {
                "times": {
                    "user": process.cpu_times().user,  # s
                    "system": process.cpu_times().system,  # s
                },
                "percent": process.cpu_percent(),  # Not accurate, single core %
            },
            "mem": {
                "rss": {
                    "percent": process.memory_percent(memtype="rss"),  # Not accurate, %
                    "value": process.memory_info().rss / (1024 * 1024),  # MB
                },
                "vms": {
                    "percent": process.memory_percent(memtype="vms"),  # Not accurate, %
                    "value": process.memory_info().vms / (1024 * 1024),  # MB
                },
            },
        }

    samples = []
    while True:
        try:
            sample = {
                "renderer": get_metrics_for(renderer_process),
                **(
                    {"relay": get_metrics_for(relay_process)}
                    if not args.skip_relay
                    else {}
                ),
                "gpu": get_gpu_metrics(),
            }

            print(json.dumps(sample, indent=4))
            samples.append(sample)
        except Exception as e:
            print(f"Error: {e}")
            break
        finally:
            time.sleep(1)

    # Clean up the processes
    cleanup(None, None)
