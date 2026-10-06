/**

Compresses Mim source for a playground link: UTF-8 -> deflate-raw -> base64url.
A `#z=` fragment never leaves the browser, so it carries far more source than a `?src=` query ever could.

*/

const ShareLink = {
    async encode(text) {
        const stream = new Blob([text]).stream().pipeThrough(new CompressionStream("deflate-raw"))
        const buf = await new Response(stream).arrayBuffer()
        return toBase64Url(new Uint8Array(buf))
    },

    async decode(z) {
        const stream = new Blob([fromBase64Url(z)]).stream().pipeThrough(new DecompressionStream("deflate-raw"))
        return new Response(stream).text()
    },
}

function toBase64Url(bytes) {
    let bin = ""
    for (const b of bytes) bin += String.fromCharCode(b)
    return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "")
}

function fromBase64Url(z) {
    const pad = z.length % 4 ? "=".repeat(4 - (z.length % 4)) : ""
    return Uint8Array.from(atob(z.replace(/-/g, "+").replace(/_/g, "/") + pad), c => c.charCodeAt(0))
}
