# Admin Module Sources

These files are the editable source modules for the admin area:

- `users.js`
- `logs.js`
- `rule-script.js`

Runtime does not load these files directly.

Published runtime modules (built by `node scripts/build-admin-bundle.js`):

- `data/www/js/modules/users.js`
- `data/www/js/modules/logs.js`
- `data/www/js/modules/rule-script.js`

Build commands:

- `node scripts/build-admin-bundle.js`
- `node scripts/gzip-www.js`
