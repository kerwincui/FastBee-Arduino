// Resolve all fastbee devices via mDNS
const mdns = require('mdns-js');

console.log('Resolving mDNS services...');

const resolver = mdns.createResolver(null, null, (err, info) => {
    if (err) { console.error(err); return; }
    const sub = resolver Service('_http._tcp.local.')
    sub.on('ip', (name, type, value) => {
        if (name.startsWith('fastbee-')) {
            console.log(`${name}: ${value}`);
        }
    });
    sub.start();
});

setTimeout(() => {
    resolver.destroy();
    console.log('Done');
}, 10000);
