import { useEffect, useRef, useState } from "react";
import { Player } from "./lib/playback";
import "./App.css";
import { useLocalStorage } from "react-use";

function App() {
    const ref = useRef<HTMLCanvasElement>(null);
    const player = useRef<Player | null>(null);
    const [config, setConfig, remove] = useLocalStorage<{
        upscaler?: string,
        ratio?: string,
        mode?: string
    }>('config', {
        upscaler: "FSR",
        ratio: "Quality",
        mode: "Detached"
    });

    const handleStart = async () => {
        if (!ref.current) return;

        if (player.current)
            await player.current.close();

        const url = `https://localhost:4443`;
        const fingerprint = `${url}/fingerprint`;
        player.current = await Player.create({
            url,
            fingerprint,
            canvas: ref.current,
            namespace: "live",
        })
        player.current.play();
    }

    const downloadJSON = (filename: string, data: BlobPart) => {
        const a = document.createElement("a");
        a.href = URL.createObjectURL(new Blob([data]));
        a.download = filename;
        a.click();
        a.remove();
    }

    const downloadVideo = (filename: string, data: Blob) => {
        const a = document.createElement("a");
        a.href = URL.createObjectURL(data);
        a.download = filename;
        a.click();
        a.remove();
    }

    const handleSessionData = async () => {
        if (!player.current) return;
        const { stats, timings } = await player.current.getSessionData();

        // Decide on name
        const name = `browser_${config?.upscaler}-${config?.ratio}-${config?.mode}`;
        const repeat = localStorage.getItem(name);
        if (repeat !== null)
            name.concat(`-${repeat}`);
        localStorage.setItem(name, repeat ? `${parseInt(repeat) + 1}` : "1");

        downloadJSON(`${name}.json`, JSON.stringify({ stats, timings }, null, 2));
        console.log("stats", stats);
        console.log("timings", timings);
    }

    return <>
        <canvas ref={ref}></canvas>
        <form>
            <label>
                Upscaler:
                <select value={config!.upscaler} onChange={(e) => setConfig({ ...config, upscaler: e.target.value })}>
                    <option value="FSR3Upscale">FSR</option>
                    <option value="DLSSUpscale">DLSS</option>
                    <option value="Native">Native</option>
                </select>
            </label>
            <label>
                Ratio:
                <select value={config!.ratio} onChange={(e) => setConfig({ ...config, ratio: e.target.value })}>
                    <option value="Quality">Quality</option>
                    <option value="UltraPerformance">Ultra Performance</option>
                </select>
            </label>
            <label>
                Mode:
                <select value={config!.mode} onChange={(e) => setConfig({ ...config, mode: e.target.value })}>
                    <option value="Detached">Detached</option>
                    <option value="Default">Default</option>
                </select>
            </label>
        </form>
        <button onClick={handleStart}>Start</button>
        <button onClick={handleSessionData}>Get Session Data</button>
    </>;
}

export default App;
