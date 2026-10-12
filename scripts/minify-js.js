/**
 * Lightweight JS minifier - no npm dependencies.
 * Removes comments, collapses whitespace, trims operator padding.
 *
 * Usage:
 *   const { minifyJS } = require('./minify-js');
 *   const minified = minifyJS(sourceCode);
 */
'use strict';

function minifyJS(src) {
    // 1. Remove comments (before string extraction to avoid quotes inside comments)
    var out = src.replace(/\/\*[\s\S]*?\*\//g, '');
    out = out.replace(/(^|[^:])\/\/[^\n]*/gm, '$1');

    // 2. Extract string literals to protect them from whitespace/keyword transforms
    var strings = [];
    out = out.replace(/(["'])(?:(?!\1|\\).|\\.)*\1/g, function(match) {
        var idx = strings.length;
        strings.push(match);
        return '__MSTR' + idx + '__';
    });

    // 3. Collapse consecutive whitespace (preserve single newline for ASI)
    out = out.replace(/[ \t]+/g, ' ');
    out = out.replace(/\n\s*\n/g, '\n');
    // 4. Trim leading/trailing whitespace per line
    out = out.replace(/^ +| +$/gm, '');
    // 5. Remove spaces around safe operators
    out = out.replace(/ ?([\{\}\(\)\[\];,=\+\-\*<>!&\|:?]) ?/g, '$1');
    // 6. Restore required spaces after keywords
    out = out.replace(/(return|var|let|const|function|typeof|instanceof|new|delete|throw|case|in|of)\b(?! )/g, '$1 ');

    // 7. Restore string literals
    out = out.replace(/__MSTR(\d+)__/g, function(_, idx) {
        return strings[parseInt(idx, 10)];
    });

    return out.trim();
}

// Runtime modules can contain URLs, regular expressions and template literals.
// Parse them before removing comments; rename only local bindings. Keep public
// properties, top-level bindings and function names available to other modules.
// Use parser-driven, non-unsafe expression compression for lazy modules. The
// request-governor chunk keeps its inspectable constant for release validation.
function minifyRuntimeJS(source) {
    return require('terser').minify_sync(source, {
        compress: source.includes('WEB_PROFILE_REQUEST_LIMITS') ? false : { passes: 2 },
        // The release verifier inspects this scoped constant in the published
        // core chunk to prove the target-specific request pressure limit.
        mangle: { reserved: ['WEB_PROFILE_REQUEST_LIMITS'] },
        keep_fnames: true
    }).code;
}

module.exports = { minifyJS, minifyRuntimeJS };
