// Static asset checks supplement the Chromium network/CSP acceptance.
import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';

const root = new URL('../dist/', import.meta.url);
const manifest = JSON.parse(await readFile(new URL('firefly-assets.json', root), 'utf8'));
const names = new Set(manifest.files.map(entry => entry.file));
assert.ok(names.has('index.html'));
assert.ok([...names].some(name => /^assets\/.+-[\w-]+\.js$/.test(name)), 'hashed JS');
assert.ok([...names].some(name => /^assets\/.+-[\w-]+\.css$/.test(name)), 'hashed CSS');
for (const entry of manifest.files) {
    const bytes = await readFile(new URL(entry.file, root));
    assert.equal(createHash('sha256').update(bytes).digest('hex'), entry.sha256);
    const text = bytes.toString('utf8');
    if (entry.file.endsWith('.html')) {
        for (const [, url] of text.matchAll(/(?:src|href)="([^"]+)"/g)) {
            assert.ok(url.startsWith('/v2/'), `non-local resource ${url}`);
            assert.ok(names.has(url.slice(4)), `undeclared resource ${url}`);
            assert.ok(!url.includes('?'), 'no query-string cache busters');
        }
        assert.doesNotMatch(text, /<script(?![^>]*\bsrc=)[^>]*>|<style\b/i, 'no inline scripts/styles');
    }
    if (entry.file.endsWith('.css')) assert.doesNotMatch(text, /@import|url\(\s*["']?(?:https?:|\/\/|data:)/i);
    // Embedded dependency error/help URLs are not fetched resources. Browser
    // interception verifies the full JS bundle, rather than banning URL strings.
}
for (const file of await readdir(new URL('assets/', root))) assert.ok(names.has(`assets/${file}`), `unlisted asset ${file}`);
console.log(`PASS offline artifact: ${names.size} declared local files, hashed JS/CSS, no external HTML/CSS resources`);
