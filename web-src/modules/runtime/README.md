# Runtime Module Sources

These files are the editable source modules for large runtime-loaded pages:

- `protocol.js`
- `periph-exec.js`
- `device-control.js`
- `peripherals.js`
- `device-config.js` (device settings and lazy transfer entry points)
- `device-config-transfer.js` (configuration transfer/migration and its modal templates; first-action load only)

Published copies are written to:

- `data/www/js/modules/`

Build commands:

- `node scripts/build-web-modules.js`
- `node scripts/gzip-www.js`

Edit files in this directory, then rebuild publish assets.

The `admin/rule-script.js` and `admin/users.js` modules also own their modal
templates. Loading an unrelated page no longer downloads those templates through
the shared modal bundle; opening and closing them still uses PageLoader's normal
mount/unmount lifecycle.
