import { useRef } from "react";
import { Player } from "./lib/playback";
import "./App.css";

function App() {
    const ref = useRef<HTMLCanvasElement>(null);
    const player = useRef<Player | null>(null);

    const handleStart = async () => {
        if (!ref.current) return;
        if (player.current) await player.current.close();

        const url = `https://${window.location.hostname}:4443`;
        const fingerprint = `${url}/fingerprint`;
        player.current = await Player.create({
            url,
            fingerprint,
            canvas: ref.current,
            namespace: "live",
        });
        player.current.play();
    };

    const handleSessionData = async () => {
        if (!player.current) return;
        const { stats, timings } = await player.current.getSessionData();
        await fetch(`http://${window.location.hostname}:12000/sink`, {
            method: "POST",
            mode: "no-cors",
            headers: {
                "Content-Type": "application/json",
            },
            body: JSON.stringify({
                name: "live",
                stats,
                timings,
            }),
        });
    };

    return (
        <>
            <canvas ref={ref}></canvas>
            <button onClick={handleStart}>Start</button>
            <button onClick={handleSessionData}>Notify Sink</button>
        </>
    );
}

export default App;
