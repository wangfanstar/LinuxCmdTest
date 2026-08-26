'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const baseUrl = process.env.HTML_PASTE_API_URL || 'http://127.0.0.1:8881';
const htmlPasteDir = path.resolve('html', 'html_paste');
const token = `${process.pid}-${Date.now()}`;
const jsonName = `bundle-api-${token}.json`;
const htmlName = `bundle-api-${token}.html`;

async function request(url, options) {
  const response = await fetch(`${baseUrl}${url}`, options);
  const text = await response.text();
  let payload = null;
  try {
    payload = text ? JSON.parse(text) : null;
  } catch (_) {
    // Static HTML responses are intentionally not JSON.
  }
  return { response, text, payload };
}

async function save(name, content, overwrite = false) {
  return request('/api/html-paste/save', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ name, content, overwrite })
  });
}

(async () => {
  try {
    const jsonContent = '{"groups":[]}';
    const htmlContent = '<!doctype html><title>bundle api test</title>';

    let result = await save(jsonName, jsonContent);
    assert.strictEqual(result.response.status, 200, `JSON save failed: ${result.text}`);
    assert.deepStrictEqual(result.payload, { ok: true });

    result = await save(htmlName, htmlContent);
    assert.strictEqual(result.response.status, 200, `HTML save failed: ${result.text}`);
    assert.deepStrictEqual(result.payload, { ok: true });

    result = await request(`/html_paste/${encodeURIComponent(htmlName)}`);
    assert.strictEqual(result.response.status, 200, `HTML static read failed: ${result.text}`);
    assert.strictEqual(result.text, htmlContent);

    result = await save(htmlName, '<!doctype html><title>conflict</title>');
    assert.strictEqual(result.response.status, 409, 'default save must not overwrite an HTML file');
    assert.strictEqual(result.payload && result.payload.ok, false);

    result = await save(htmlName, htmlContent + '<p>overwritten</p>', true);
    assert.strictEqual(result.response.status, 200, `HTML overwrite failed: ${result.text}`);

    result = await request('/api/html-paste/list');
    assert.strictEqual(result.response.status, 200, `network list failed: ${result.text}`);
    assert.ok(result.payload.files.some(file => file.name === jsonName && file.type === 'json'));
    assert.ok(result.payload.files.some(file => file.name === htmlName && file.type === 'html'));

    result = await request(`/api/html-paste/delete?name=${encodeURIComponent(jsonName)}`, { method: 'DELETE' });
    assert.strictEqual(result.response.status, 200, `JSON cleanup failed: ${result.text}`);

    console.log('html paste bundle API integration tests passed');
  } finally {
    try {
      fs.unlinkSync(path.join(htmlPasteDir, jsonName));
    } catch (_) {
      // The API cleanup above is the normal path; tolerate a failed request.
    }
    try {
      fs.unlinkSync(path.join(htmlPasteDir, htmlName));
    } catch (_) {
      // The HTML companion is intentionally not deletable through the JSON API.
    }
  }
})().catch(error => {
  if (error && (error.code === 'ECONNREFUSED' || error.cause && error.cause.code === 'ECONNREFUSED')) {
    console.error(`Cannot reach ${baseUrl}; start simplewebserver or set HTML_PASTE_API_URL.`);
  }
  console.error(error);
  process.exit(1);
});
