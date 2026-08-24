'use strict';

const assert = require('assert');
const fs = require('fs');

const html = fs.readFileSync('html/index.html', 'utf8');

assert.match(html, /href=["']HtmlPasteGen\.html["']/i,
  'index.html must link to HtmlPasteGen.html');
assert.match(html, /快捷复制页面生成器/,
  'index.html must label the HtmlPasteGen entry');
assert.match(html, /target=["']_blank["']/i,
  'tool links should open in a separate tab');

console.log('index HtmlPasteGen navigation test passed');
