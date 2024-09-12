import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// https://vitejs.dev/config/
export default defineConfig({
    plugins: [react()],
    server: {
        port: 3000,
        https: {
            cert: "../bin/cert/localhost.crt",
            key: "../bin/cert/localhost.key",
        },
    },
    preview: {
        port: 3000,
        https: {
            cert: "../bin/cert/localhost.crt",
            key: "../bin/cert/localhost.key",
        },
    },
});
