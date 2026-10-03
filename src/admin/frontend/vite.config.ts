import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';

export default defineConfig({
    base: '/v2/',
    plugins: [vue(), {
        name: 'firefly-explicit-assets',
        enforce: 'post',
        generateBundle: { order: 'post', async handler(_options, bundle) {
            const files = [];
            for (const [file, output] of Object.entries(bundle).sort(([a], [b]) => a.localeCompare(b))) {
                const source = output.type === 'chunk' ? output.code : output.source;
                const bytes = typeof source === 'string' ? new TextEncoder().encode(source) : new Uint8Array(source);
                const digest = await crypto.subtle.digest('SHA-256', bytes);
                files.push({ file, sha256: Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, '0')).join('') });
            }
            this.emitFile({ type: 'asset', fileName: 'firefly-assets.json', source: JSON.stringify({ version: 1, files }, null, 2) + '\n' });
        } }
    }],
    build: { target: 'es2022', sourcemap: false, assetsInlineLimit: 0 },
});
