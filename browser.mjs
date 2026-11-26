import assert from 'node:assert/strict';
import { chromium } from 'playwright';

const browser = await chromium.launch({ executablePath: process.env.CHROME_BIN, headless: true });
try {
  const page = await browser.newPage();
  page.on('console', (message) => console.log(message.type(), message.text()));
  page.on('pageerror', (error) => console.log('pageerror', error));
  await page.goto('http://127.0.0.1:8128/browser.html');
  try {
    await page.waitForFunction(() => /PASS|FAIL|ABORT|ERROR/.test(document.querySelector('#result').textContent), undefined, { timeout: 60000 });
  } finally {
    const result = await page.locator('#result').textContent();
    console.log(result);
    assert.match(result, /PASS .*callback_switches=0 restored=0 alloc=0 free=0 limit_rc=-4019/);
    assert.doesNotMatch(result, /FAIL|ABORT|ERROR/);
  }
} finally {
  await browser.close();
}
