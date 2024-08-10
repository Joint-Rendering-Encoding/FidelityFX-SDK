import os
import glob
import time
import subprocess

__doc__ = """\
This script will run some configurations of governor.opy and check the SSIM score of the output images.
"""


def get_cross_command(upscaler):
    render_res = ["1280", "720"] if upscaler != "Native" else ["2560", "1440"]
    fps = 30 if upscaler in ["FSR3", "DLSS"] else 60
    return [
        "python",
        "governor.py",
        "--render-res",
        *render_res,
        "--present-res",
        "2560",
        "1440",
        "--benchmark",
        "1",
        "--upscaler",
        upscaler,
        "--fps",
        str(fps),
        "--scene",
        "Sponza",
        "--dlssMode",
        "Quality",
        "--reduced-motion",
        "--use-default",
        "--structured-logs",
        "--compare",
        "ssim",
        "--compare-with",
        upscaler,
        "--switch-default-mode",
        "--compare-fps",
        str(fps),
    ]


def get_native_command(upscaler):
    render_res = ["2560", "1440"]
    fps = 30 if upscaler in ["FSR3", "DLSS"] else 60
    return [
        "python",
        "governor.py",
        "--render-res",
        *render_res,
        "--present-res",
        "2560",
        "1440",
        "--benchmark",
        "1",
        "--upscaler",
        upscaler,
        "--fps",
        str(fps),
        "--scene",
        "Sponza",
        "--dlssMode",
        "Quality",
        "--reduced-motion",
        "--use-default",
        "--structured-logs",
        "--compare",
        "ssim",
        "--compare-with",
        "Native",
        "--compare-fps",
        "60",
    ]


def run_and_get_ssim(command, timeout=30, tries=3):
    while tries > 0:
        try:
            proc = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )

            # Wait for process to complete or timeout
            output, _ = proc.communicate(timeout=timeout)

            # Check each line for the SSIM metric
            for line in output.splitlines():
                if "METRIC_SSIM" in line:
                    return float(line.split()[-1])
        except subprocess.TimeoutExpired:
            proc.kill()
            print(f"Timeout reached. Killing process. {tries-1} tries left.")
        except Exception as e:
            print(f"An error occurred: {e}")

        tries -= 1
        print(f"Failed to get SSIM score. Retrying... {tries} tries left")

    return None  # Return None if SSIM score could not be obtained


if __name__ == "__main__":
    # Remove results
    for file in glob.glob("bin/benchmark/*"):
        os.remove(file)

    cross = []
    native = []

    upscalers = ["Native", "FSR3", "DLSS", "FSR3Upscale", "DLSSUpscale"]
    for upscaler in upscalers:
        command = get_cross_command(upscaler)
        print(" ".join(command))
        ssim = run_and_get_ssim(command)
        print(f"Cross-validation SSIM for {upscaler}: {ssim}")
        cross.append(ssim)
        print()

    for upscaler in upscalers:
        command = get_native_command(upscaler)
        print(" ".join(command))
        ssim = run_and_get_ssim(command)
        print(f"Native-validation SSIM for {upscaler}: {ssim}")
        native.append(ssim)
        print()

    # Visualize the results as a table
    print("\n\n")
    print("Cross-validation")
    print("Upscaler\t\tSSIM")
    for upscaler, ssim in zip(upscalers, cross):
        print(f"{upscaler}\t\t{ssim}")

    print("\n\n")
    print("Native-validation")
    print("Upscaler\t\tSSIM")
    for upscaler, ssim in zip(upscalers, native):
        print(f"{upscaler}\t\t{ssim}")
