// Compiles a program with the freshly built wasm `mim` to catch a wasm-only regression.
// usage: node smoke.js <dir holding mim.js> <source.mim>

const fs   = require('node:fs');
const path = require('node:path');

const OUTPUTS = ['/out.mim', '/out.ll', '/out.dot', '/out.nest'];

async function main([dir, src]) {
    if (!dir || !src) throw new Error('usage: node smoke.js <dir holding mim.js> <source.mim>');

    const createMim = require(path.resolve(dir, 'mim.js'));
    const log = [];
    // mim.wasm and mim.data sit next to mim.js, not in the cwd.
    const M = await createMim({
        print: s => log.push(s),
        printErr: s => log.push(s),
        locateFile: file => path.resolve(dir, file),
    });

    M.FS.writeFile('/in.mim', fs.readFileSync(src, 'utf8'));
    const code = M.callMain(['/in.mim', '-P', '/mim', '-p', 'opt', '-p', 'll',
                             '-X', 'll:o=/out.ll', '--output-dot', '/out.dot',
                             '--output-nest', '/out.nest', '-o', '/out.mim']);
    if (code !== 0) {
        console.error(log.join('\n'));
        throw new Error(`mim exited with ${code}`);
    }

    for (const out of OUTPUTS) {
        const size = M.FS.readFile(out, { encoding: 'utf8' }).length;
        if (size === 0) throw new Error(`${out} is empty`);
        console.log(`${out}: ${size} bytes`);
    }
}

main(process.argv.slice(2)).catch(e => {
    console.error(String(e?.message ?? e));
    process.exit(1);
});
