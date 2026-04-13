# Runtime Module Sources

These files are the editable source modules for large runtime-loaded pages:

- `protocol.js`
- `periph-exec.js`
- `device-control.js`
- `peripherals.js`

Published copies are written to:

- `data/www/js/modules/`

Build commands:

- `node scripts/build-web-modules.js`
- `node scripts/gzip-www.js`

Edit files in this directory, then rebuild publish assets.
