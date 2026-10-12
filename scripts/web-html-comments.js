'use strict';

// Only these static fragments have been reviewed for comment-node independence.
// Source comments and every other byte remain intact; this is publish-only.
const REVIEWED_FRAGMENTS = new Set([
    'pages/modals.html', 'pages/network.html', 'pages/admin.html',
    'pages/features.html', 'pages/peripheral.html', 'pages/protocol.html',
    'pages/rule-script.html', 'pages/fragments/device-ota.html',
    'pages/fragments/protocol-modbus-rtu.html'
]);

function stripReviewedHtmlComments(relativePath, source) {
    if (!REVIEWED_FRAGMENTS.has(relativePath.replace(/\\/g, '/'))) return source;
    const removals = [];
    let index = 0;
    while (index < source.length) {
        const start = source.indexOf('<', index);
        if (start < 0) break;
        if (source.startsWith('<!--', start)) {
            const end = source.indexOf('-->', start + 4);
            // Malformed or directive comments require a fresh review.
            if (end < 0) return source;
            const body = source.slice(start + 4, end).trim();
            if (!/^(?:[\[!#/@$]|ko(?:\s|$))/i.test(body)) removals.push([start, end + 3]);
            index = end + 3;
            continue;
        }
        // Do not reason about raw text, foreign markup or declarations here.
        // If a future edit introduces one, keep the complete fragment unchanged.
        if (/^<(?:script|style|textarea|title|xmp|iframe|noembed|noframes|plaintext|svg|math)\b/i.test(source.slice(start)) ||
            source.startsWith('<!', start) || source.startsWith('<?', start)) return source;
        if (!/^<\/?[a-z][\w:-]*(?:\s|\/?\s*>)/i.test(source.slice(start))) {
            index = start + 1;
            continue;
        }
        // Consume a complete tag without interpreting comment-like attribute
        // values, including quoted values containing > or either quote style.
        let quote = '', end = start + 1;
        for (; end < source.length; end += 1) {
            const character = source[end];
            if (quote) { if (character === quote) quote = ''; }
            else if (character === '"' || character === "'") quote = character;
            else if (character === '>') break;
        }
        if (end === source.length) return source;
        index = end + 1;
    }
    let result = '', cursor = 0;
    for (const [start, end] of removals) { result += source.slice(cursor, start); cursor = end; }
    return result + source.slice(cursor);
}

module.exports = {stripReviewedHtmlComments};
