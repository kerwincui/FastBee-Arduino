const { Document, Paragraph, Table, TableRow, TableCell, TextRun,
  HeadingLevel, AlignmentType, WidthType, Packer, ShadingType } = require('docx');
const fs = require('fs');
const path = require('path');

var WEIGHTS = { work: 0.35, decision: 0.25, team: 0.15, innovation: 0.15, prof: 0.10 };
var NAMES = ['\u5173\u6811\u6807', '\u674E\u58EE\u9E4F', '\u90D3\u5609\u660E', '\u5F20\u5FD7\u7FFC'];

var guan_w = {};
guan_w['\u5173\u6811\u6807'] = { work: 34, decision: 25, team: 13, innovation: 15, prof: 9, total: 96 };
guan_w['\u674E\u58EE\u9E4F'] = { work: 29, decision: 19, team: 12, innovation: 12, prof: 8, total: 80 };
guan_w['\u90D3\u5609\u660E'] = { work: 31, decision: 22, team: 14, innovation: 12, prof: 9, total: 88 };
guan_w['\u5F20\u5FD7\u7FFC'] = { work: 32, decision: 23, team: 15, innovation: 14, prof: 9, total: 93 };

var li_r = {};
li_r['\u5173\u6811\u6807'] = { work: 80, decision: 85, team: 80, innovation: 80, prof: 80 };
li_r['\u674E\u58EE\u9E4F'] = { work: 80, decision: 75, team: 80, innovation: 85, prof: 80 };
li_r['\u90D3\u5609\u660E'] = { work: 80, decision: 75, team: 80, innovation: 80, prof: 80 };
li_r['\u5F20\u5FD7\u7FFC'] = { work: 80, decision: 80, team: 80, innovation: 80, prof: 80 };

var deng_r = {};
deng_r['\u5173\u6811\u6807'] = { work: 90, decision: 90, team: 90, innovation: 91, prof: 90 };
deng_r['\u674E\u58EE\u9E4F'] = { work: 85, decision: 76, team: 80, innovation: 80, prof: 70 };
deng_r['\u90D3\u5609\u660E'] = { work: 88, decision: 88, team: 90, innovation: 86, prof: 90 };
deng_r['\u5F20\u5FD7\u7FFC'] = { work: 89, decision: 88, team: 90, innovation: 90, prof: 90 };

var zhang_r = {};
zhang_r['\u5173\u6811\u6807'] = { work: 86, decision: 90, team: 89, innovation: 90, prof: 89 };
zhang_r['\u674E\u58EE\u9E4F'] = { work: 75, decision: 70, team: 70, innovation: 75, prof: 75 };
zhang_r['\u90D3\u5609\u660E'] = { work: 80, decision: 80, team: 85, innovation: 70, prof: 89 };
zhang_r['\u5F20\u5FD7\u7FFC'] = { work: 89, decision: 86, team: 89, innovation: 89, prof: 89 };

var zhang_notes = {};
zhang_notes['\u5173\u6811\u6807'] = '\u5DE5\u4F5C\u8D28\u91CF\u7EC6\u8282\u5DEE\u4E00\u70B9\u70B9\uFF0C\u4E00\u76F4\u521B\u65B0\u5B66\u4E60\u5206\u4EAB\u65B0\u4E1C\u897F\uFF0C\u4E3B\u5BFC\u91CD\u5927\u51B3\u7B56';
zhang_notes['\u674E\u58EE\u9E4F'] = '\u5DE5\u4F5C\u6548\u7387\u8D28\u91CF\u4E00\u822C\uFF0C\u672A\u79EF\u6781\u8D21\u732E\u51B3\u7B56\uFF0C\u56E2\u961F\u5408\u4F5C\u5C11\uFF0C\u672A\u4E3B\u52A8\u5F00\u53D1\uFF0C\u521B\u65B0\u5C11\u672A\u4F53\u73B0\u6210\u679C';
zhang_notes['\u90D3\u5609\u660E'] = '\u5DE5\u4F5C\u8D28\u91CF\u4E00\u822C\uFF0C\u5B58\u5728\u4E9B\u8BB8\u5C0F\u95EE\u9898\uFF0C\u672A\u79EF\u6781\u53C2\u8003\u5E02\u573A\u5BF9\u4EA7\u54C1\u63D0\u51FA\u51B3\u7B56\u6539\u53D8\uFF0C\u6309\u90E8\u5C31\u73ED\u521B\u65B0\u80FD\u529B\u4E0D\u8DB3';
zhang_notes['\u5F20\u5FD7\u7FFC'] = '\u5DE5\u4F5C\u6548\u7387\u8D28\u91CF\u9AD8\uFF0C\u5F15\u9886\u53D1\u5E03\u4E24\u4E2A\u65B0\u7248\u672C\uFF0C\u79EF\u6781\u63D0\u51B3\u7B56\u5EFA\u8BAE\uFF0C\u79EF\u6781\u534F\u540C\u56E2\u961F\uFF0C\u5FEB\u901F\u63A5\u5165AI\u8D4B\u80FD\u5F15\u6D41\u5BA2\u6237';

function calcW(r) {
  return +(r.work * 0.35 + r.decision * 0.25 + r.team * 0.15 + r.innovation * 0.15 + r.prof * 0.10).toFixed(2);
}

var scores = {};
NAMES.forEach(function(ev) {
  scores[ev] = {};
  NAMES.forEach(function(n) {
    if (ev === NAMES[0]) scores[ev][n] = guan_w[n].total;
    else if (ev === NAMES[1]) scores[ev][n] = calcW(li_r[n]);
    else if (ev === NAMES[2]) scores[ev][n] = calcW(deng_r[n]);
    else scores[ev][n] = calcW(zhang_r[n]);
  });
});

var summary = {};
NAMES.forEach(function(n) {
  var all = NAMES.map(function(e) { return scores[e][n]; });
  var avg = +(all.reduce(function(a, b) { return a + b; }, 0) / all.length).toFixed(2);
  summary[n] = { scores: {}, avg: avg };
  NAMES.forEach(function(e) { summary[n].scores[e] = scores[e][n]; });
});

var ranking = NAMES.slice().sort(function(a, b) { return summary[b].avg - summary[a].avg; });

console.log('=== Verification ===');
NAMES.forEach(function(ev) {
  NAMES.forEach(function(tgt) {
    var raw, w;
    if (ev === NAMES[0]) { console.log(ev + '->' + tgt + ': w=' + guan_w[tgt].total); return; }
    if (ev === NAMES[1]) raw = li_r[tgt];
    else if (ev === NAMES[2]) raw = deng_r[tgt];
    else raw = zhang_r[tgt];
    w = calcW(raw);
    console.log(ev + '->' + tgt + ': ' + raw.work + 'x.35+' + raw.decision + 'x.25+' + raw.team + 'x.15+' + raw.innovation + 'x.15+' + raw.prof + 'x.10=' + w);
  });
});
console.log('\n=== Ranking ===');
ranking.forEach(function(n, i) { console.log('#' + (i + 1) + ': ' + n + ' avg=' + summary[n].avg); });

function mkText(t, opts) {
  opts = opts || {};
  return new TextRun({ text: t, font: 'Microsoft YaHei', size: opts.size || 21, bold: opts.bold, color: opts.color });
}
function mkPara(content, opts) {
  opts = opts || {};
  var runs = Array.isArray(content) ? content : [mkText(content, opts)];
  return new Paragraph({ children: runs, heading: opts.heading, alignment: opts.align || AlignmentType.LEFT, spacing: { after: 120 } });
}
function hCell(t, w) {
  return new TableCell({
    children: [new Paragraph({ children: [mkText(t, { bold: true, color: 'FFFFFF', size: 20 })], alignment: AlignmentType.CENTER })],
    shading: { type: ShadingType.SOLID, color: '2F5496' },
    width: w ? { size: w, type: WidthType.PERCENTAGE } : undefined
  });
}
function dCell(t, w, opts) {
  opts = opts || {};
  return new TableCell({
    children: [new Paragraph({ children: [mkText(String(t), { size: 20, bold: opts.bold })], alignment: opts.align || AlignmentType.CENTER })],
    width: w ? { size: w, type: WidthType.PERCENTAGE } : undefined,
    shading: opts.bg ? { type: ShadingType.SOLID, color: opts.bg } : undefined
  });
}

var children = [];
children.push(mkPara('2026\u5E747\u6708 \u80A1\u4E1C\u7EFC\u5408\u8003\u8BC4\u6C47\u603B\u62A5\u544A', { heading: HeadingLevel.TITLE, align: AlignmentType.CENTER }));
children.push(mkPara([mkText('\u62A5\u544A\u65E5\u671F\uFF1A2026\u5E747\u6708', { size: 20, color: '666666' })], { align: AlignmentType.CENTER }));
children.push(mkPara(''));
children.push(mkPara('\u4E00\u3001\u8003\u8BC4\u6982\u8FF0', { heading: HeadingLevel.HEADING_1 }));
children.push(mkPara('\u672C\u6B21\u8003\u8BC4\u4E3A\u80A1\u4E1C\u4E92\u8BC4\u5236\uFF0C\u51714\u4F4D\u80A1\u4E1C\u53C2\u4E0E\uFF1A\u5173\u6811\u6807\u3001\u674E\u58EE\u9E4F\u3001\u90D3\u5609\u660E\u3001\u5F20\u5FD7\u7FFC\u3002\u6BCF\u4F4D\u80A1\u4E1C\u5BF9\u5176\u4ED6\u4E09\u4F4D\u80A1\u4E1C\u53CA\u81EA\u8EAB\u8FDB\u884C\u8BC4\u5206\uFF0C\u91C7\u7528\u884C\u4E3A\u951A\u5B9A\u8BC4\u5206\u6807\u51C6\uFF08BARS\uFF09\u3002'));
children.push(mkPara(''));

var gRows = [];
gRows.push(new TableRow({ children: [hCell('\u7B49\u7EA7', 20), hCell('\u5206\u6570\u533A\u95F4', 25), hCell('\u542B\u4E49', 55)] }));
gRows.push(new TableRow({ children: [dCell('\u5353\u8D8A', 20), dCell('90~100', 25), dCell('\u8868\u73B0\u8FDC\u8D85\u5C97\u4F4D\u8981\u6C42', 55, { align: AlignmentType.LEFT })] }));
gRows.push(new TableRow({ children: [dCell('\u826F\u597D', 20), dCell('70~89', 25), dCell('\u8868\u73B0\u7A33\u5B9A\u8FBE\u6807\uFF0C\u90E8\u5206\u7565\u8D85\u9884\u671F', 55, { align: AlignmentType.LEFT })] }));
gRows.push(new TableRow({ children: [dCell('\u5F85\u6539\u8FDB', 20), dCell('50~69', 25), dCell('\u57FA\u672C\u80DC\u4EFB\u4F46\u5B58\u5728\u660E\u663E\u77ED\u677F', 55, { align: AlignmentType.LEFT })] }));
gRows.push(new TableRow({ children: [dCell('\u4E0D\u5408\u683C', 20), dCell('<50', 25), dCell('\u672A\u80FD\u8FBE\u5230\u57FA\u672C\u8981\u6C42', 55, { align: AlignmentType.LEFT })] }));
children.push(mkPara([mkText('\u8BC4\u5206\u6807\u51C6\uFF1A', { bold: true })]));
children.push(new Table({ width: { size: 100, type: WidthType.PERCENTAGE }, rows: gRows }));
children.push(mkPara(''));

var dmRows = [];
dmRows.push(new TableRow({ children: [hCell('\u7EF4\u5EA6', 40), hCell('\u6743\u91CD', 15), hCell('\u8BF4\u660E', 45)] }));
dmRows.push(new TableRow({ children: [dCell('\u5DE5\u4F5C\u4E1A\u7EE9', 40, { align: AlignmentType.LEFT }), dCell('35%', 15), dCell('\u8D28\u91CF/\u6548\u7387/\u76EE\u6807\u8FBE\u6210', 45, { align: AlignmentType.LEFT })] }));
dmRows.push(new TableRow({ children: [dCell('\u6218\u7565\u51B3\u7B56\u4E0E\u8D44\u6E90\u8D21\u732E', 40, { align: AlignmentType.LEFT }), dCell('25%', 15), dCell('\u80A1\u4E1C\u7279\u6709\u7EF4\u5EA6', 45, { align: AlignmentType.LEFT })] }));
dmRows.push(new TableRow({ children: [dCell('\u56E2\u961F\u534F\u4F5C\u4E0E\u6C9F\u901A', 40, { align: AlignmentType.LEFT }), dCell('15%', 15), dCell('\u534F\u540C\u914D\u5408/\u4FE1\u606F\u5171\u4EAB', 45, { align: AlignmentType.LEFT })] }));
dmRows.push(new TableRow({ children: [dCell('\u521B\u65B0\u80FD\u529B\u4E0E\u5B66\u4E60\u80FD\u529B', 40, { align: AlignmentType.LEFT }), dCell('15%', 15), dCell('\u5F15\u5165\u65B0\u65B9\u6CD5/\u5DE5\u5177/\u6A21\u5F0F', 45, { align: AlignmentType.LEFT })] }));
dmRows.push(new TableRow({ children: [dCell('\u804C\u4E1A\u7D20\u517B', 40, { align: AlignmentType.LEFT }), dCell('10%', 15), dCell('\u8003\u52E4/\u54C1\u5FB7/\u516C\u5171\u536B\u751F', 45, { align: AlignmentType.LEFT })] }));
children.push(mkPara([mkText('\u8003\u8BC4\u7EF4\u5EA6\u53CA\u6743\u91CD\uFF1A', { bold: true })]));
children.push(new Table({ width: { size: 100, type: WidthType.PERCENTAGE }, rows: dmRows }));
children.push(mkPara(''));

children.push(mkPara('\u4E8C\u3001\u5404\u8BC4\u59D4\u8BC4\u5206\u660E\u7EC6', { heading: HeadingLevel.HEADING_1 }));

function buildETbl(ev, raw, isW) {
  var rs = [];
  rs.push(new TableRow({ children: [hCell('\u88AB\u8BC4\u4EBA', 16), hCell('\u4E1A\u7EE9', 14), hCell('\u51B3\u7B56', 14), hCell('\u534F\u4F5C', 14), hCell('\u521B\u65B0', 14), hCell('\u7D20\u517B', 14), hCell('\u603B\u5206', 14)] }));
  NAMES.forEach(function(n) {
    var w = scores[ev][n];
    var src = isW ? guan_w[n] : raw[n];
    rs.push(new TableRow({ children: [dCell(n, 16, { bold: true }), dCell(src.work, 14), dCell(src.decision, 14), dCell(src.team, 14), dCell(src.innovation, 14), dCell(src.prof, 14), dCell(isW ? src.total : w, 14, { bold: true })] }));
  });
  return new Table({ width: { size: 100, type: WidthType.PERCENTAGE }, rows: rs });
}

children.push(mkPara([mkText('1. \u5173\u6811\u6807 \u8BC4\u5206', { bold: true, size: 22 })]));
children.push(mkPara([mkText('\u8BF4\u660E\uFF1A\u8BE5\u8BC4\u59D4\u4EE5\u52A0\u6743\u5206\u503C\u5F62\u5F0F\u586B\u5199\u5404\u7EF4\u5EA6\u5F97\u5206\uFF0C\u603B\u5206\u76F4\u63A5\u4E3A\u5404\u52A0\u6743\u9879\u4E4B\u548C\u3002', { size: 18, color: '666666' })]));
children.push(buildETbl(NAMES[0], guan_w, true));
children.push(mkPara(''));
children.push(mkPara([mkText('2. \u674E\u58EE\u9E4F \u8BC4\u5206', { bold: true, size: 22 })]));
children.push(buildETbl(NAMES[1], li_r, false));
children.push(mkPara(''));
children.push(mkPara([mkText('3. \u90D3\u5609\u660E \u8BC4\u5206', { bold: true, size: 22 })]));
children.push(buildETbl(NAMES[2], deng_r, false));
children.push(mkPara(''));
children.push(mkPara([mkText('4. \u5F20\u5FD7\u7FFC \u8BC4\u5206', { bold: true, size: 22 })]));
children.push(buildETbl(NAMES[3], zhang_r, false));
children.push(mkPara(''));

children.push(mkPara([mkText('\u5F20\u5FD7\u7FFC\u8BC4\u4F30\u4F9D\u636E\uFF1A', { bold: true })]));
NAMES.forEach(function(n) { children.push(mkPara([mkText(n + '\uFF1A', { bold: true, size: 19 }), mkText(zhang_notes[n], { size: 19 })])); });
children.push(mkPara(''));

children.push(mkPara('\u4E09\u3001\u52A0\u6743\u603B\u5206\u6C47\u603B\u4E0E\u6392\u540D', { heading: HeadingLevel.HEADING_1 }));
var sRows = [];
sRows.push(new TableRow({ children: [hCell('\u6392\u540D', 8), hCell('\u59D3\u540D', 16), hCell('\u5173\u8BC4', 16), hCell('\u674E\u8BC4', 16), hCell('\u90D3\u8BC4', 16), hCell('\u5F20\u8BC4', 16), hCell('\u5747\u5206', 12), hCell('\u7B49\u7EA7', 10)] }));
ranking.forEach(function(n, i) {
  var s = summary[n];
  var g = s.avg >= 90 ? '\u5353\u8D8A' : (s.avg >= 70 ? '\u826F\u597D' : (s.avg >= 50 ? '\u5F85\u6539\u8FDB' : '\u4E0D\u5408\u683C'));
  var bg = (i === 0) ? 'D4EDDA' : ((i === ranking.length - 1) ? 'FFF3CD' : undefined);
  sRows.push(new TableRow({ children: [dCell(i + 1, 8, { bold: true }), dCell(n, 16, { bold: true, bg: bg }), dCell(s.scores[NAMES[0]], 16), dCell(s.scores[NAMES[1]], 16), dCell(s.scores[NAMES[2]], 16), dCell(s.scores[NAMES[3]], 16), dCell(s.avg, 12, { bold: true }), dCell(g, 10)] }));
});
children.push(new Table({ width: { size: 100, type: WidthType.PERCENTAGE }, rows: sRows }));
children.push(mkPara(''));

children.push(mkPara('\u56DB\u3001\u5404\u7EF4\u5EA6\u4EA4\u53C9\u5BF9\u6BD4', { heading: HeadingLevel.HEADING_1 }));
children.push(mkPara([mkText('\u8BF4\u660E\uFF1A\u5173\u6811\u6807\u8BC4\u5206\u4EE5\u52A0\u6743\u5206\u503C\u586B\u5199\uFF0C\u65E0\u6CD5\u63D0\u53D6\u539F\u59CB\u5206\uFF0C\u6B64\u8868\u4EC5\u53D6\u674E\u3001\u90D3\u3001\u5F20\u4E09\u4EBA\u5747\u503C\u4F9B\u53C2\u8003\u3002', { size: 18, color: '666666' })]));
var ddRows = [];
ddRows.push(new TableRow({ children: [hCell('\u88AB\u8BC4\u4EBA', 16), hCell('\u4E1A\u7EE9', 14), hCell('\u51B3\u7B56', 14), hCell('\u534F\u4F5C', 14), hCell('\u521B\u65B0', 14), hCell('\u7D20\u517B', 14)] }));
var rSets = [li_r, deng_r, zhang_r];
var dKeys = ['work', 'decision', 'team', 'innovation', 'prof'];
NAMES.forEach(function(n) {
  var cs = [dCell(n, 16, { bold: true })];
  dKeys.forEach(function(d) {
    var vals = rSets.map(function(r) { return r[n][d]; });
    cs.push(dCell(+(vals.reduce(function(a, b) { return a + b; }, 0) / vals.length).toFixed(1), 14));
  });
  ddRows.push(new TableRow({ children: cs }));
});
children.push(new Table({ width: { size: 100, type: WidthType.PERCENTAGE }, rows: ddRows }));
children.push(mkPara(''));

children.push(mkPara('\u4E94\u3001\u8C03\u6574\u5EFA\u8BAE\u6C47\u603B', { heading: HeadingLevel.HEADING_1 }));
children.push(mkPara([mkText('\u90D3\u5609\u660E \u5EFA\u8BAE\uFF1A', { bold: true })]));
children.push(mkPara('\u5EFA\u8BAE\u9500\u552E\u6A21\u5F0F\u7075\u6D3B\u53D8\u901A\uFF0C\u4E0D\u8981\u5355\u4E00\u56FA\u5316\uFF0C\u5076\u5C14\u4E0E\u65F6\u4FF1\u8FDB\u7684\u7B56\u5212\u5F15\u6D41\u62D3\u5BA2\u6D3B\u52A8\uFF0C\u62D3\u5BBD\u5BA2\u6E90\u3002'));
children.push(mkPara(''));
children.push(mkPara([mkText('\u5173\u6811\u6807 \u5EFA\u8BAE\uFF1A', { bold: true })]));
children.push(mkPara('AI\u65F6\u4EE3\u521B\u65B0\u548C\u65B0\u5B66\u4E60\u975E\u5E38\u91CD\u8981\uFF0C\u5E0C\u671B\u56E2\u961F\u6BCF\u4EBA\u638C\u63E1\uFF0C\u4E0D\u8981\u518D\u7528\u4F20\u7EDF\u6280\u672F\u5F00\u53D1\u3002\u56E2\u961F\u7F3A\u5C11\u6316\u6398\u65B0\u65B9\u5411\u3001\u70ED\u95E8\u573A\u666F\u7684\u89D2\u8272\uFF0C\u53EF\u7ED3\u5408AI\u6316\u6398\u66F4\u591A\u98CE\u53E3\u3002'));
children.push(mkPara(''));
children.push(mkPara([mkText('\u5F20\u5FD7\u7FFC \u5EFA\u8BAE\uFF1A', { bold: true })]));
children.push(mkPara('\u5956\u91D1\u6216\u63D0\u6210\u8C03\u6574\u3002\u5F20\u5FD7\u7FFC\u63D0\u6210\u4E0A\u8C032.6~3\u8BE5\u533A\u95F4\u3002'));
children.push(mkPara(''));
children.push(mkPara([mkText('\u674E\u58EE\u9E4F\uFF1A', { bold: true })]));
children.push(mkPara('\u672A\u586B\u5199\u8C03\u6574\u5EFA\u8BAE\u3002'));

children.push(mkPara(''));
children.push(mkPara('\u516D\u3001\u6570\u636E\u51C6\u786E\u6027\u8BF4\u660E', { heading: HeadingLevel.HEADING_1 }));
children.push(mkPara([mkText('1. \u5173\u6811\u6807\u7684\u8BC4\u5206\u8868\u4EE5\u52A0\u6743\u5206\u503C\u5F62\u5F0F\u586B\u5199\uFF0C\u52A0\u6743\u603B\u5206\u76F4\u63A5\u4F5C\u4E3A\u5176\u5BF9\u5404\u4EBA\u7684\u8BC4\u4EF7\u7ED3\u679C\uFF0C\u4E0D\u505A\u989D\u5916\u8F6C\u6362\u3002', { size: 19 })]));
children.push(mkPara([mkText('2. \u674E\u58EE\u9E4F\u3001\u90D3\u5609\u660E\u3001\u5F20\u5FD7\u7FFC\u4E09\u4EBA\u586B\u5199\u539F\u59CB\u5206\u5E76\u9644\u5E26\u52A0\u6743\u8BA1\u7B97\u516C\u5F0F\uFF0C\u6570\u636E\u53EF\u5B8C\u6574\u9A8C\u8BC1\u3002', { size: 19 })]));
children.push(mkPara([mkText('3. \u5404\u8BC4\u59D4\u586B\u8868\u683C\u5F0F\u7565\u6709\u5DEE\u5F02\uFF0C\u5EFA\u8BAE\u540E\u7EED\u7EDF\u4E00\u4E3A\u539F\u59CB\u5206\u586B\u5199\u65B9\u5F0F\uFF0C\u4EE5\u4FBF\u4E8E\u7EF4\u5EA6\u7EA7\u5BF9\u6BD4\u5206\u6790\u3002', { size: 19 })]));

var doc = new Document({ sections: [{ properties: { page: { margin: { top: 1000, bottom: 1000, left: 1200, right: 1200 } } }, children: children }] });
var outPath = path.join(__dirname, '..', 'docs', '\u80A1\u4E1C\u7EFC\u5408\u8003\u8BC4\u6C47\u603B\u62A5\u544A-202607.docx');
Packer.toBuffer(doc).then(function(buf) {
  fs.writeFileSync(outPath, buf);
  console.log('\nGenerated: ' + outPath);
  console.log('Size: ' + (buf.length / 1024).toFixed(1) + ' KB');
}).catch(function(err) { console.error('Failed:', err); process.exit(1); });
