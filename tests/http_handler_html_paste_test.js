'use strict';

const assert = require('assert');
const fs = require('fs');

const source = fs.readFileSync('src/http_handler.c', 'utf8');

assert.match(source, /\/api\/html-paste\/list/);
assert.match(source, /\/api\/html-paste\/read/);
assert.match(source, /\/api\/html-paste\/save/);
assert.match(source, /html_paste/);
assert.match(source, /html_paste_name_safe\s*\(/);
assert.match(source, /html_paste_json_bool\s*\(/);
assert.match(source, /html_paste_json_field\s*\(/);
assert.match(source, /strncmp\([^\n]+true/);
assert.match(source, /pthread_mutex_lock\s*\(&g_html_paste_mu\)/);
assert.match(source, /link\s*\(temp, filepath\)/);
assert.match(source, /html_paste_request_allowed\s*\(/);
assert.match(source, /auth_require_author\s*\(/);
assert.match(source, /static_path_safe\s*\(/);
assert.match(source, /html_paste_static_allowed\s*\(/);
assert.match(source, /url_decode_report_fn\(decoded_path\);[\s\S]{0,240}static_path_safe\(decoded_path\)/);
assert.match(source, /strcasecmp\([^\n]+\.json/);
assert.match(source, /strcasecmp\([^\n]+\.html/);
assert.match(source, /strstr\([^\n]+\.\./);
assert.match(source, /409/);
assert.match(source, /overwrite/);
assert.match(source, /rename\s*\(/);
assert.match(source, /MAX_HTML_PASTE_SIZE/);
assert.match(source, /handle_api_html_paste_list\s*\(/);
assert.match(source, /handle_api_html_paste_read\s*\(/);
assert.match(source, /handle_api_html_paste_save\s*\(/);

console.log('html paste network API contracts passed');
