import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// https://vitejs.dev/config/
export default defineConfig({
    plugins: [react()],
    server: {
        host: true,
        port: 3000,
        https: {
            cert: "../bin/cert/localhost.crt",
            key: "../bin/cert/localhost.key",
        },
    },
    preview: {
        host: true,
        port: 3000,
        https: {
            cert: "../bin/cert/localhost.crt",
            key: "../bin/cert/localhost.key",
        },
    },
});
