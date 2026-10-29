// Host fallback for scripts/web/serve.sh when Docker is unavailable: a static server on 127.0.0.1 only
// (never expose it: it serves your ROM copy). Same layout as the container: build-web/site -> /app,
// data/ (dev ROM copy + dev sidecars) -> /dev.
//   node scripts/web/serve_node.mjs [port=8770]
import http from 'node:http'; import fs from 'node:fs'; import path from 'node:path'; import url from 'node:url';
const root = path.resolve(path.dirname(url.fileURLToPath(import.meta.url)), '../..');
const port = +(process.argv[2] || 8770);
const mounts = { '/app/': path.join(root, 'build-web/site'), '/dev/': path.join(root, 'data') };
const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.json': 'application/json', '.css': 'text/css', '.png': 'image/png' };
http.createServer((req, res) => {
  const p = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  const m = Object.keys(mounts).find((k) => p.startsWith(k));
  const f = m && path.resolve(mounts[m], '.' + p.slice(m.length - 1));
  if (!f || !f.startsWith(mounts[m]) || !fs.existsSync(f) || !fs.statSync(f).isFile()) { res.writeHead(404); res.end('not found'); return; }
  res.writeHead(200, { 'Content-Type': types[path.extname(f)] || 'application/octet-stream', 'Cache-Control': 'no-store' });
  fs.createReadStream(f).pipe(res);
}).listen(port, '127.0.0.1', () => console.log(`http://localhost:${port}/app/index.html?romurl=../dev/ge007.pal-final.z64&assets=../dev`));
