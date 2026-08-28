'use strict';

const assert = require('assert');
const fs = require('fs');

const source = fs.readFileSync('simplewebserver.sh', 'utf8');
const restartMatch = source.match(/cmd_restart\(\) \{([\s\S]*?)\n\}/);
assert.ok(restartMatch, 'cmd_restart function must exist');
const restartBody = restartMatch[1];
const buildIndex = restartBody.indexOf('cmd_build');
const stopIndex = restartBody.indexOf('cmd_stop');
assert.ok(buildIndex >= 0 && stopIndex >= 0 && buildIndex < stopIndex,
  'restart must compile before stopping the running server');
assert.match(restartBody, /if\s+!\s+cmd_build[\s\S]*?return\s+1/);
assert.match(restartBody, /保留当前运行服务|未执行重启/);
assert.match(restartBody, /cmd_stop\s+"\$@"/,
  'restart must forward start/restart options to stop');
assert.doesNotMatch(source, /_START_ARGS/,
  'stop must not reference an undefined global argument array');

const buildMatch = source.match(/cmd_build\(\) \{([\s\S]*?)\n\}/);
assert.ok(buildMatch, 'cmd_build function must exist');
assert.match(buildMatch[1], /return 1/);

console.log('simplewebserver restart ordering tests passed');
