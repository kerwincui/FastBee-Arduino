const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../../web-src/js/state-session.js'), 'utf8');

function setup() {
  const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return {promise, resolve, reject}; };
  const session = deferred(), login = deferred();
  const nodes = {username: {value: 'offline-user'}, password: {value: 'offline-password'}, remember: {checked: false}};
  const store = {removeItem() {}, setItem() {}, getItem() { return null; }};
  const context = {AppState: {currentUser: {}}, localStorage: store, sessionStorage: store,
    document: {getElementById: id => nodes[id], querySelector: () => null},
    Notification: {warning() {}, success() {}, error() {}},
    apiGet: () => session.promise, apiPost: () => login.promise};
  vm.runInNewContext(source, context);
  let screen = 'login';
  context.AppState._showLoginPage = () => { screen = 'login'; };
  context.AppState._bootDashboardAfterAuth = () => { screen = 'app'; };
  return {app: context.AppState, session, login, screen: () => screen};
}
const settle = () => new Promise(resolve => setImmediate(resolve));

for (const result of ['invalid', 'rejected']) test('late ' + result + ' startup probe cannot hide a completed login', async () => {
  const run = setup(); run.app.refreshPage(); run.app.handleLogin();
  run.login.resolve({success: true}); await settle(); assert.equal(run.screen(), 'app');
  if (result === 'invalid') run.session.resolve({success: true, data: {sessionValid: false}});
  else run.session.reject(new Error('old session request failed'));
  await settle(); assert.equal(run.screen(), 'app');
});

test('current invalid session still shows the login page', async () => {
  const run = setup(); run.app.refreshPage();
  run.session.resolve({success: true, data: {sessionValid: false}});
  await settle(); assert.equal(run.screen(), 'login');
});

test('current valid session still restores the application', async () => {
  const run = setup(); run.app.refreshPage();
  run.session.resolve({success: true, data: {sessionValid: true, username: 'offline-user'}});
  await settle(); assert.equal(run.screen(), 'app');
});
