# Admin Module Sources

These files are the editable source modules for the admin area:

- `users.js`
- `roles.js`
- `logs.js`
- `files.js`
- `rule-script.js`

Runtime does not load these files directly.

The published runtime bundle is:

- `data/www/js/modules/admin-bundle.js`

Build commands:

- `node scripts/build-admin-bundle.js`
- `node scripts/gzip-www.js`
