export async function inspectMainShellStyles(page) {
  return page.evaluate(() => {
    const marker = document.getElementById('fb-main-stylesheet');
    const sheets = Array.from(document.styleSheets);
    const mainSheet = sheets.find((sheet) => {
      if (sheet.ownerNode === marker) return true;
      if (!sheet.href) return false;
      try {
        return new URL(sheet.href, location.href).pathname.endsWith('/css/main.css');
      } catch (_) {
        return false;
      }
    });
    let rulesReadable = false;
    let ruleCount = 0;
    let rulesError = '';
    const selectors = new Set();
    if (mainSheet) {
      try {
        const pending = Array.from(mainSheet.cssRules || []);
        while (pending.length) {
          const rule = pending.shift();
          ruleCount++;
          if (rule.selectorText) {
            rule.selectorText.split(',').forEach((selector) => selectors.add(selector.trim()));
          }
          if (rule.cssRules) pending.push(...Array.from(rule.cssRules));
        }
        rulesReadable = true;
      } catch (error) {
        rulesError = error?.message || String(error);
      }
    }

    const app = document.getElementById('app-container');
    const wrapper = app?.querySelector('.app-wrapper');
    const sidebar = document.getElementById('sidebar');
    const main = app?.querySelector('.main-content');
    const content = app?.querySelector('.content');
    const style = (element) => element ? getComputedStyle(element) : null;
    const rect = (element) => {
      if (!element) return null;
      const value = element.getBoundingClientRect();
      return { left: value.left, right: value.right, width: value.width, height: value.height };
    };
    return {
      markerPresent: !!marker,
      markerReady: marker?.dataset.ready === '1',
      href: mainSheet?.href || marker?.getAttribute('href') || '',
      sheetPresent: !!mainSheet,
      rulesReadable,
      rulesError,
      ruleCount,
      criticalRules: Object.fromEntries(
        ['.app-container', '.app-wrapper', '.sidebar', '.main-content', '.content']
          .map((selector) => [selector, selectors.has(selector)]),
      ),
      computed: {
        appDisplay: style(app)?.display || '',
        wrapperDisplay: style(wrapper)?.display || '',
        wrapperDirection: style(wrapper)?.flexDirection || '',
        sidebarDisplay: style(sidebar)?.display || '',
        sidebarPosition: style(sidebar)?.position || '',
        sidebarFlexShrink: style(sidebar)?.flexShrink || '',
        mainDisplay: style(main)?.display || '',
        contentOverflowX: style(content)?.overflowX || '',
      },
      rects: { wrapper: rect(wrapper), sidebar: rect(sidebar), main: rect(main) },
      viewportWidth: innerWidth,
    };
  });
}

export function mainShellStyleIssues(evidence) {
  const issues = [];
  if (!evidence.markerPresent) issues.push('missing #fb-main-stylesheet');
  if (!evidence.markerReady) issues.push('main stylesheet load event not complete');
  if (!evidence.sheetPresent) issues.push('main CSSStyleSheet missing');
  if (!evidence.rulesReadable) issues.push(`main CSS rules unreadable${evidence.rulesError ? `: ${evidence.rulesError}` : ''}`);
  if (evidence.ruleCount < 5) issues.push(`main CSS rule count too small: ${evidence.ruleCount}`);
  for (const [selector, present] of Object.entries(evidence.criticalRules || {})) {
    if (!present) issues.push(`main CSS rule missing: ${selector}`);
  }
  if (evidence.computed?.wrapperDisplay !== 'flex') issues.push(`app wrapper display is ${evidence.computed?.wrapperDisplay || 'missing'}`);
  if (evidence.computed?.sidebarFlexShrink !== '0') issues.push(`sidebar flex-shrink is ${evidence.computed?.sidebarFlexShrink || 'missing'}`);
  if (evidence.computed?.mainDisplay !== 'flex') issues.push(`main content display is ${evidence.computed?.mainDisplay || 'missing'}`);
  if (evidence.computed?.contentOverflowX !== 'hidden') issues.push(`content overflow-x is ${evidence.computed?.contentOverflowX || 'missing'}`);
  if (evidence.viewportWidth > 768) {
    if (evidence.computed?.wrapperDirection !== 'row') issues.push(`desktop app wrapper direction is ${evidence.computed?.wrapperDirection || 'missing'}`);
    const sidebar = evidence.rects?.sidebar;
    const main = evidence.rects?.main;
    if (!sidebar || sidebar.width < 180 || sidebar.width > 260) issues.push(`desktop sidebar width is ${sidebar?.width ?? 'missing'}`);
    if (!sidebar || !main || main.left < sidebar.right - 1) issues.push('desktop main content does not follow sidebar');
  } else if (evidence.computed?.wrapperDirection !== 'column') {
    issues.push(`mobile app wrapper direction is ${evidence.computed?.wrapperDirection || 'missing'}`);
  }
  return issues;
}

export async function waitForMainShellStyles(page, { timeoutMs = 3000, pollMs = 100 } = {}) {
  const deadline = Date.now() + timeoutMs;
  let evidence;
  let issues;
  do {
    evidence = await inspectMainShellStyles(page);
    issues = mainShellStyleIssues(evidence);
    if (!issues.length) return evidence;
    if (Date.now() >= deadline) break;
    await page.waitForTimeout(Math.min(pollMs, Math.max(1, deadline - Date.now())));
  } while (true);
  const error = new Error(`Global main stylesheet is not applied: ${issues.join('; ')}; evidence=${JSON.stringify(evidence)}`);
  error.code = 'MAIN_STYLESHEET_NOT_APPLIED';
  error.issues = issues;
  error.evidence = evidence;
  throw error;
}
