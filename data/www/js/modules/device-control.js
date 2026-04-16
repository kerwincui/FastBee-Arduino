(function (){
'use strict';
console.log('[device-control] Module script loading...');
var DEVICE_ICON_SVG='<svg viewBox="0 0 80 80" width="80" height="80" fill="none" xmlns="http://www.w3.org/2000/svg">'+
'<rect x="20" y="15" width="40" height="50" rx="3" stroke="currentColor" stroke-width="2.5" fill="none"/>'+
'<rect x="26" y="21" width="28" height="38" rx="1" stroke="currentColor" stroke-width="1.5" fill="none"/>'+
'<circle cx="40" cy="58" r="2" fill="currentColor"/>'+
'<line x1="15" y1="22" x2="20" y2="22" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="28" x2="20" y2="28" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="34" x2="20" y2="34" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="40" x2="20" y2="40" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="46" x2="20" y2="46" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="52" x2="20" y2="52" stroke="currentColor" stroke-width="2"/>'+
'<line x1="15" y1="58" x2="20" y2="58" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="22" x2="65" y2="22" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="28" x2="65" y2="28" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="34" x2="65" y2="34" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="40" x2="65" y2="40" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="46" x2="65" y2="46" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="52" x2="65" y2="52" stroke="currentColor" stroke-width="2"/>'+
'<line x1="60" y1="58" x2="65" y2="58" stroke="currentColor" stroke-width="2"/>'+
'<line x1="28" y1="10" x2="28" y2="15" stroke="currentColor" stroke-width="2"/>'+
'<line x1="34" y1="10" x2="34" y2="15" stroke="currentColor" stroke-width="2"/>'+
'<line x1="40" y1="10" x2="40" y2="15" stroke="currentColor" stroke-width="2"/>'+
'<line x1="46" y1="10" x2="46" y2="15" stroke="currentColor" stroke-width="2"/>'+
'<line x1="52" y1="10" x2="52" y2="15" stroke="currentColor" stroke-width="2"/>'+
'<line x1="28" y1="65" x2="28" y2="70" stroke="currentColor" stroke-width="2"/>'+
'<line x1="34" y1="65" x2="34" y2="70" stroke="currentColor" stroke-width="2"/>'+
'<line x1="40" y1="65" x2="40" y2="70" stroke="currentColor" stroke-width="2"/>'+
'<line x1="46" y1="65" x2="46" y2="70" stroke="currentColor" stroke-width="2"/>'+
'<line x1="52" y1="65" x2="52" y2="70" stroke="currentColor" stroke-width="2"/>'+
'</svg>';
var POWER_ICON_SVG='<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><path d="M12 2v8"/><path d="M18.4 6.6a9 9 0 1 1-12.8 0"/></svg>';
AppState.registerModule('device-control',{
_controlData:null,
_modbusStatus:null,
_modbusDevices:[],
_dcCoilStates:{},
_dcCoilPending:{},
_dcBatchPending:false,
_dcPwmStates:{},
_dcPidValues:{},
_dcAutoRefreshTimers:{},
_sseConnection:null,
_deviceName:'FastBee Device',
_eventsBound:false,
setupDeviceControlEvents:function (){
console.log('[device-control] Setting up events...');
var self=this;
var content=document.getElementById('dc-content');
if(content&&!this._eventsBound){
content.addEventListener('click',function (e){
if(e.target.closest('.dc-layout-reset')){
self._dcResetLayout();
return ;
}
if(e.target.closest('#dc-refresh-btn')){
self.loadDeviceControlPage();
return ;
}
var btn=e.target.closest('.dc-ctrl-btn');
if(!btn)return ;
var ruleId=btn.getAttribute('data-id');
var isSystem=btn.getAttribute('data-system')==='true';
var ruleName=btn.getAttribute('data-name')||'';
if(isSystem){
if(confirm(self._t('device-control-confirm-system')+'\n'+ruleName)){
self._executeRule(ruleId,btn);
}
}else{
self._executeRule(ruleId,btn);
}
});
content.addEventListener('click',function (e){
var coilCard=e.target.closest('.dc-coil-card');
if(coilCard){
var devIdx=parseInt(coilCard.getAttribute('data-dev'));
var ch=parseInt(coilCard.getAttribute('data-ch'));
var key=devIdx+'_'+ch;
if(self._dcCoilPending[key])return ;
self._dcCoilPending[key]=true;
self._dcToggleCoil(devIdx,ch,coilCard);
return ;
}
var batchBtn=e.target.closest('.dc-coil-batch');
if(batchBtn){
var action=batchBtn.getAttribute('data-action');
var dIdx=parseInt(batchBtn.getAttribute('data-dev'));
if(self._dcBatchPending)return ;
self._dcBatchPending=true;
batchBtn.disabled=true;
self._dcBatchCoil(dIdx,action,batchBtn);
return ;
}
var refreshCoilBtn=e.target.closest('.dc-coil-refresh');
if(refreshCoilBtn){
var di=parseInt(refreshCoilBtn.getAttribute('data-dev'));
self._dcRefreshCoilStatus(di);
return ;
}
var delayStartBtn=e.target.closest('.dc-delay-start');
if(delayStartBtn){
var dIdx=parseInt(delayStartBtn.getAttribute('data-dev'));
var section=delayStartBtn.closest('.dc-relay-delay-section');
var channelSelect=section.querySelector('.dc-delay-channel');
var delayInput=section.querySelector('.dc-delay-input');
if(channelSelect&&delayInput){
var channel=parseInt(channelSelect.value);
var delayUnits=parseInt(delayInput.value);
if(delayUnits>=1&&delayUnits<=255){
self._dcStartCoilDelay(dIdx,channel,delayUnits);
}
}
return ;
}
var pwmBatchBtn=e.target.closest('.dc-pwm-batch');
if(pwmBatchBtn){
var pAction=pwmBatchBtn.getAttribute('data-action');
var pIdx=parseInt(pwmBatchBtn.getAttribute('data-dev'));
self._dcBatchPwm(pIdx,pAction);
return ;
}
var refreshPwmBtn=e.target.closest('.dc-pwm-refresh');
if(refreshPwmBtn){
var pi=parseInt(refreshPwmBtn.getAttribute('data-dev'));
self._dcRefreshPwmStatus(pi);
return ;
}
var pidSetBtn=e.target.closest('.dc-pid-set');
if(pidSetBtn){
var pidDevIdx=parseInt(pidSetBtn.getAttribute('data-dev'));
var pidParam=pidSetBtn.getAttribute('data-param');
var pidInput=pidSetBtn.parentNode.querySelector('.dc-pid-input');
if(pidInput){
self._dcSetPidParam(pidDevIdx,pidParam,pidInput.value);
}
return ;
}
var refreshPidBtn=e.target.closest('.dc-pid-refresh');
if(refreshPidBtn){
var pidDi=parseInt(refreshPidBtn.getAttribute('data-dev'));
self._dcRefreshPidStatus(pidDi);
return ;
}
});
content.addEventListener('input',function (e){
if(e.target.classList.contains('dc-pwm-slider')){
var devIdx=parseInt(e.target.getAttribute('data-dev'));
var ch=parseInt(e.target.getAttribute('data-ch'));
var val=parseInt(e.target.value);
self._dcOnPwmSliderInput(devIdx,ch,val);
}
});
content.addEventListener('change',function (e){
if(e.target.classList.contains('dc-pwm-slider')){
var devIdx=parseInt(e.target.getAttribute('data-dev'));
var ch=parseInt(e.target.getAttribute('data-ch'));
var val=parseInt(e.target.value);
self._dcSetPwmChannel(devIdx,ch,val);
}
if(e.target.classList.contains('dc-pwm-num')){
var devIdx=parseInt(e.target.getAttribute('data-dev'));
var ch=parseInt(e.target.getAttribute('data-ch'));
var val=parseInt(e.target.value)||0;
self._dcSetPwmChannel(devIdx,ch,val);
}
});
}
this._eventsBound=true;
console.log('[device-control] Events bound successfully');
},
loadDeviceControlPage:function (){
console.log('[device-control] loadDeviceControlPage called');
this._modbusDevices=[];
if(!this._eventsBound){
this.setupDeviceControlEvents();
}
var content=document.getElementById('dc-content');
if(!content){
console.error('[device-control] Content element "dc-content" not found!');
return ;
}
this._setContentState(content,'loading');
var self=this;
this._dcStopAllAutoRefresh();
this._fetchDeviceInfo().then(function (){
console.log('[device-control] Fetching controls...');
return apiGet('/api/periph-exec/controls');
}).then(function (controlsRes){
self._tempControlsRes=controlsRes;
return apiGetSilent('/api/modbus/status').catch(function (){return null;});
}).then(function (modbusRes){
self._tempModbusRes=modbusRes;
return apiGetSilent('/api/protocol/config').catch(function (){return null;});
}).then(function (protoRes){
var res=self._tempControlsRes;
var modbusRes=self._tempModbusRes;
delete self._tempControlsRes;
delete self._tempModbusRes;
if(modbusRes&&modbusRes.success&&modbusRes.data){
self._modbusStatus=modbusRes.data;
}else{
self._modbusStatus=null;
}
self._modbusDevices=[];
if(protoRes&&protoRes.success&&protoRes.data){
var rtu=protoRes.data.modbusRtu;
if(rtu&&rtu.enabled&&rtu.master&&rtu.master.devices){
self._modbusDevices=rtu.master.devices.filter(function (d){
return d.enabled!==false;
});
}
}
console.log('[device-control] Modbus devices count:',self._modbusDevices.length);
console.log('[device-control] API response:',JSON.stringify(res));
if(!res){
console.log('[device-control] No response, showing empty state');
self._setContentState(content,'empty');
return ;
}
if(res.success===false){
console.log('[device-control] API returned failure:',res.error||res.message);
self._setContentState(content,'error',res.error||res.message||'请求失败');
return ;
}
var data=res.data||res;
console.log('[device-control] Parsed data:',JSON.stringify(data));
if(!data||typeof data!=='object'){
console.log('[device-control] No valid data, showing empty state');
self._setContentState(content,'empty');
return ;
}
self._controlData=data;
try{
var html=self._renderControlPanel(data);
console.log('[device-control] Rendered HTML length:',html.length);
content.innerHTML=html;
self._dcApplyLayout();
self._dcInitFreeLayout();
self._dcInitModbusDeviceStates();
if(self.currentPage==='device-control'){
self._setupSSE();
}
if(self._modbusDevices.length===0){
setTimeout(function (){
if(self.currentPage!=='device-control')return ;
apiGetSilent('/api/protocol/config').then(function (retryRes){
if(!retryRes||!retryRes.success||!retryRes.data)return ;
var rtu=retryRes.data.modbusRtu;
if(rtu&&rtu.enabled&&rtu.master&&rtu.master.devices){
var newDevices=rtu.master.devices.filter(function (d){
return d.enabled!==false;
});
if(newDevices.length>0){
self._modbusDevices=newDevices;
console.log('[device-control] Retry: found',newDevices.length,'Modbus devices, re-rendering');
self.loadDeviceControlPage();
}
}
}).catch(function (){});
},2000);
}
}catch(renderErr){
console.error('[device-control] Render error:',renderErr);
self._setContentState(content,'error',renderErr.message||renderErr);
}
}).catch(function (err){
console.error('[device-control] API error:',err);
var errMsg='请求失败';
if(err&&err.data&&err.data.error){
errMsg=err.data.error;
}else if(err&&err.message){
errMsg=err.message;
}
self._setContentState(content,'error',errMsg);
});
},
_fetchDeviceInfo:function (){
var self=this;
return apiGetSilent('/api/device/config').then(function (res){
if(res&&res.success&&res.data){
self._deviceName=res.data.deviceName||res.data.name||'FastBee Device';
}
}).catch(function (){
self._deviceName='FastBee Device';
});
},
_renderControlPanel:function (data){
console.log('[device-control] _renderControlPanel called with data:',data);
data=data||{};
var html='';
try{
html+=this._renderControlSection(data);
}catch(e){
console.error('[device-control] Error in _renderControlPanel:',e);
html='<div class="dc-empty u-text-danger">渲染错误: '+this._esc(e.message||e)+'</div>';
}
return html;
},
_shouldShowModbusHealthBanner:function (health){
if(!health)return false;
var warnings=Array.isArray(health.warnings)?health.warnings:[];
var riskLevel=String(health.riskLevel||'low').toLowerCase();
return riskLevel!=='low'||warnings.length>0||Number(health.enabledTaskCount||0)>0;
},
_getDcRiskMeta:function (level){
switch(String(level||'low').toLowerCase()){
case 'high':
return {text:this._t('modbus-master-risk-high'),className:'modbus-risk-high'};
case 'medium':
return {text:this._t('modbus-master-risk-medium'),className:'modbus-risk-medium'};
default:
return {text:this._t('modbus-master-risk-low'),className:'modbus-risk-low'};
}
},
_formatDcAge:function (ageSec){
var age=Number(ageSec||0);
if(!age)return '--';
if(age<60)return age+'s';
if(age<3600)return Math.floor(age / 60)+'m';
return Math.floor(age / 3600)+'h';
},
_formatDcPercent:function (value){
var num=Number(value||0);
if(!isFinite(num))num=0;
return (Math.round(num*10)/ 10)+'%';
},
_renderModbusHealthBanner:function (){
var health=this._modbusStatus&&this._modbusStatus.health;
if(!this._shouldShowModbusHealthBanner(health))return '';
var riskMeta=this._getDcRiskMeta(health.riskLevel);
var warnings=Array.isArray(health.warnings)?health.warnings.slice(0,3):[];
var html='<div class="dc-risk-banner" data-dc-sort-key="health">'+
'<div class="dc-risk-title">'+this._t('device-control-modbus-health-title')+'</div>'+
'<div class="dc-risk-main">'+
'<span class="dc-risk-label">'+this._t('modbus-master-risk-level')+'</span>'+
'<span class="modbus-risk-badge '+riskMeta.className+'">'+this._esc(riskMeta.text)+'</span>'+
'</div>'+
'<div class="dc-risk-metrics">'+
'<span class="dc-risk-metric"><span class="dc-risk-metric-label">'+this._t('modbus-master-enabled-tasks')+'</span><strong>'+this._esc(health.enabledTaskCount??0)+'</strong></span>'+
'<span class="dc-risk-metric"><span class="dc-risk-metric-label">'+this._t('modbus-master-min-interval')+'</span><strong>'+this._esc(health.minPollInterval?(health.minPollInterval+'s'):'--')+'</strong></span>'+
'<span class="dc-risk-metric"><span class="dc-risk-metric-label">'+this._t('modbus-master-last-poll-age')+'</span><strong>'+this._esc(this._formatDcAge(health.lastPollAgeSec))+'</strong></span>'+
'<span class="dc-risk-metric"><span class="dc-risk-metric-label">'+this._t('modbus-master-timeout-rate')+'</span><strong>'+this._esc(this._formatDcPercent(health.timeoutRate))+'</strong></span>'+
'</div>';
if(warnings.length>0){
var warnHtml='<div class="dc-risk-warnings">';
for(var wi=0;wi<warnings.length;wi++){
warnHtml+='<div class="dc-risk-warning"><i class="fas fa-exclamation-triangle"></i><span>'+this._esc(warnings[wi]||'')+'</span></div>';
}
html+=warnHtml+'</div>';
}
html+='<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_renderMonitorSection:function (data){
var html='';
html+=this._renderHealthCard();
html+=this._renderMonitorDataCard(data);
return html;
},
_renderHealthCard:function (){
return this._renderModbusHealthBanner();
},
_renderMonitorDataCard:function (data){
var monitorGroups=this._buildMonitorGroupsFromModbus();
var hasMonitor=monitorGroups.length>0;
var monitorItems=[];
if(!hasMonitor){
var modbusItems=this._filterByActionType(data.modbus,[18]);
var sensorItems=this._filterByActionType(data.sensor,[19]);
monitorItems=modbusItems.concat(sensorItems);
hasMonitor=monitorItems.length>0;
}
if(!hasMonitor){
return '';
}
var html='<div class="dc-monitor-grid" data-dc-sort-key="monitor-data">';
if(monitorGroups.length>0){
for(var i=0;i<monitorGroups.length;i++){
var group=monitorGroups[i];
for(var j=0;j<group.items.length;j++){
html+=this._renderMonitorCard(group.items[j],group.label);
}
}
}else{
for(var i=0;i<monitorItems.length;i++){
html+=this._renderMonitorCard(monitorItems[i]);
}
}
html+='<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_buildMonitorGroupsFromModbus:function (){
var groups=[];
if(!this._modbusStatus||!this._modbusStatus.tasks){
return groups;
}
var tasks=this._modbusStatus.tasks;
for(var i=0;i<tasks.length;i++){
var task=tasks[i];
if(!task.enabled)continue;
if(!task.mappings||task.mappings.length===0)continue;
var group={
label:task.name||task.label||('设备 '+task.slaveAddress),
items:[]
};
var cachedData=task.cachedData;
var hasCache=cachedData&&cachedData.values;
for(var j=0;j<task.mappings.length;j++){
var mapping=task.mappings[j];
if(!mapping.sensorId)continue;
var item={
id:mapping.sensorId,
name:mapping.sensorId,
value:'--',
unit:''
};
if(hasCache&&mapping.regOffset<cachedData.values.length){
var rawValue=cachedData.values[mapping.regOffset];
var scaleFactor=mapping.scaleFactor||1;
var decimalPlaces=mapping.decimalPlaces||0;
var scaledValue=rawValue*scaleFactor;
item.value=scaledValue.toFixed(decimalPlaces);
}
group.items.push(item);
}
if(group.items.length>0){
groups.push(group);
}
}
return groups;
},
_renderMonitorCard:function (item,deviceLabel){
var name=this._esc(item.name||'Unknown');
var value=this._esc(item.value||item.lastValue||'--');
var unit=this._esc(item.unit||'');
var toneClass=this._getMonitorToneClass(item.name||'');
var icon=this._getMonitorIcon(item.name||'');
var html='<div class="dc-monitor-card" data-id="'+this._esc(item.id)+'">';
html+='<div class="dc-monitor-top">';
html+='<span class="dc-monitor-label">'+name+'</span>';
html+='<span class="dc-monitor-icon '+toneClass+'">'+icon+'</span>';
html+='</div>';
if(deviceLabel){
html+='<div class="dc-monitor-device">'+this._esc(deviceLabel)+'</div>';
}
html+='<div class="dc-monitor-val-row">';
html+='<span class="dc-monitor-value">'+value+'</span>';
if(unit)html+='<span class="dc-monitor-unit">'+unit+'</span>';
html+='</div>';
html+='</div>';
return html;
},
_getMonitorIcon:function (name){
var n=name.toLowerCase();
if(n.indexOf('hum')!==-1||n.indexOf('湿度')!==-1)return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2c-5.33 8-8 12.67-8 16a8 8 0 0016 0c0-3.33-2.67-8-8-16z"/></svg>';
if(n.indexOf('temp')!==-1||n.indexOf('温度')!==-1)return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M15 13V5a3 3 0 00-6 0v8a5 5 0 106 0zm-3-9a1 1 0 011 1v9.17a3 3 0 11-2 0V5a1 1 0 011-1z"/></svg>';
if(n.indexOf('pm')!==-1||n.indexOf('颗粒')!==-1)return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M17.5 18.25a1.25 1.25 0 110-2.5 1.25 1.25 0 010 2.5zm-5-3a1.75 1.75 0 110-3.5 1.75 1.75 0 010 3.5zm-5-4a1.5 1.5 0 110-3 1.5 1.5 0 010 3zm10-3a1 1 0 110-2 1 1 0 010 2zm-3 10a1 1 0 110-2 1 1 0 010 2zm-7 2a.75.75 0 110-1.5.75.75 0 010 1.5z"/></svg>';
return '<svg viewBox="0 0 24 24" width="16" height="16" fill="currentColor"><path d="M12 2a10 10 0 100 20 10 10 0 000-20zm1 15h-2v-2h2v2zm0-4h-2V7h2v6z"/></svg>';
},
_getMonitorToneClass:function (name){
var n=name.toLowerCase();
if(n.indexOf('temp')!==-1||n.indexOf('温度')!==-1)return 'dc-monitor-icon-tone-temp';
if(n.indexOf('hum')!==-1||n.indexOf('湿度')!==-1)return 'dc-monitor-icon-tone-humidity';
if(n.indexOf('pm')!==-1||n.indexOf('颗粒')!==-1||n.indexOf('粉尘')!==-1)return 'dc-monitor-icon-tone-particle';
if(n.indexOf('co2')!==-1||n.indexOf('二氧化碳')!==-1)return 'dc-monitor-icon-tone-co2';
if(n.indexOf('press')!==-1||n.indexOf('气压')!==-1)return 'dc-monitor-icon-tone-pressure';
if(n.indexOf('light')!==-1||n.indexOf('光照')!==-1||n.indexOf('lux')!==-1)return 'dc-monitor-icon-tone-light';
if(n.indexOf('wind')!==-1||n.indexOf('风')!==-1)return 'dc-monitor-icon-tone-weather';
if(n.indexOf('rain')!==-1||n.indexOf('雨')!==-1)return 'dc-monitor-icon-tone-weather';
return 'dc-monitor-icon-tone-default';
},
_renderDeviceIconSection:function (){
var html='<div class="dc-device-banner">';
html+=DEVICE_ICON_SVG;
html+='<div class="dc-device-banner-info">';
html+='<div class="dc-device-name">'+this._esc(this._deviceName)+'</div>';
html+='<div class="dc-device-status">'+this._t('device-control-online')+'</div>';
html+='</div>';
html+='</div>';
return html;
},
_renderControlSection:function (data){
var html='';
var gpioItems=this._filterByActionType(data.gpio,[0,1,2,3,4,5,13,14]);
var modbusCtrlItems=this._filterByActionType(data.modbus,[16,17]);
var systemItems=this._filterByActionType(data.system,[6,7,8,9,10,11,12]);
var scriptItems=this._filterByActionType(data.script,[15]);
var sensorReadItems=this._filterByActionType(data.sensor,[19]);
var otherItems=data.other||[];
gpioItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
modbusCtrlItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
systemItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
scriptItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
sensorReadItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
otherItems.sort(function (a,b){return (a.name||'').localeCompare(b.name||'');});
var hasModbusDevices=this._modbusDevices&&this._modbusDevices.length>0;
var modbusDeviceList=[];
if(hasModbusDevices){
var typeOrder={relay:0,pid:1,pwm:2};
for(var mi=0;mi<this._modbusDevices.length;mi++){
modbusDeviceList.push({idx:mi,dev:this._modbusDevices[mi]});
}
modbusDeviceList.sort(function (a,b){
var ta=typeOrder[a.dev.deviceType||'relay']||0;
var tb=typeOrder[b.dev.deviceType||'relay']||0;
return ta-tb;
});
}
var hasButtonControls=gpioItems.length>0||modbusCtrlItems.length>0||
systemItems.length>0||scriptItems.length>0||
sensorReadItems.length>0||otherItems.length>0;
var hasAnyControls=hasButtonControls||modbusDeviceList.length>0;
if(hasAnyControls){
html+='<div class="dc-layout-toolbar">';
html+='<span class="dc-layout-title">'+this._t('device-control-dashboard')+'</span>';
html+='<div class="dc-toolbar-actions">';
html+='<button class="dc-btn-sm dc-btn-refresh" id="dc-refresh-btn">'+this._t('dashboard-refresh')+'</button>';
html+='<button class="dc-btn-sm dc-btn-reset dc-layout-reset">'+this._t('device-control-reset-layout')+'</button>';
html+='</div>';
html+='</div>';
html+='<div class="dc-control-flow">';
html+=this._renderMonitorSection(data);
if(gpioItems.length>0){
html+=this._renderGpioGroup(gpioItems);
}
if(modbusCtrlItems.length>0){
html+=this._renderControlGroup('Modbus',modbusCtrlItems,'modbus',false);
}
if(scriptItems.length>0){
html+=this._renderControlGroup('Script',scriptItems,'script',false);
}
if(sensorReadItems.length>0){
html+=this._renderControlGroup('Sensor',sensorReadItems,'sensor',false);
}
if(otherItems.length>0){
html+=this._renderControlGroup('Other',otherItems,'other',false);
}
if(systemItems.length>0){
html+=this._renderSystemGroup(systemItems);
}
if(modbusDeviceList.length>0){
html+=this._renderModbusDevicePanels(modbusDeviceList);
}
html+='</div>';
}else{
html+='<div class="dc-empty">'+this._t('device-control-no-action')+'</div>';
}
return html;
},
_renderSystemGroup:function (items){
var title=this._t('device-control-group-system')||'System';
var html='<div class="dc-control-group dc-group-card" data-dc-sort-key="system">';
html+=this._renderDcCardHeader('dc-card-badge--system','SYS',title);
html+='<div class="dc-sys-grid">';
for(var i=0;i<items.length;i++){
var item=items[i];
html+='<button class="dc-ctrl-btn dc-sys-card" data-id="'+this._esc(item.id)+'" data-system="true" data-name="'+this._esc(item.name)+'">';
html+='<div class="dc-sys-card-name">'+this._esc(item.name)+'</div>';
html+='</button>';
}
html+='</div><div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_renderGpioGroup:function (items){
var title=this._t('device-control-group-gpio')||'GPIO';
var html='<div class="dc-control-group dc-group-card" data-dc-sort-key="gpio">';
html+=this._renderDcCardHeader('dc-card-badge--gpio','GPIO',title);
html+='<div class="dc-gpio-list">';
for(var i=0;i<items.length;i++){
var item=items[i];
html+='<div class="dc-gpio-row">';
html+='<span class="dc-gpio-name">'+this._esc(item.name)+'</span>';
html+='<button class="dc-ctrl-btn dc-gpio" data-id="'+this._esc(item.id)+'">'+this._t('device-control-execute')+'</button>';
html+='</div>';
}
html+='</div><div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_renderModbusDevicePanels:function (deviceList){
var list=deviceList||[];
if(list.length===0)return '';
var html='';
for(var i=0;i<list.length;i++){
html+=this._renderSingleModbusPanel(list[i].idx,list[i].dev);
}
return html;
},
_renderSingleModbusPanel:function (devIdx,dev){
var dt=dev.deviceType||'relay';
var typeLabel=this._t('modbus-type-'+dt)||dt;
var ctrlLabel=(dev.name||'')||(this._t('dc-modbus-ctrl-'+dt)||(typeLabel+' '+this._t('device-control-action-section')));
var typeClassMap={relay:'dc-card-badge--relay',pwm:'dc-card-badge--pwm',pid:'dc-card-badge--pid'};
var badgeClass=typeClassMap[dt]||'dc-card-badge--system';
var html='<div class="dc-modbus-device-panel" data-dev-idx="'+devIdx+'" data-dc-sort-key="modbus-'+dt+'-'+devIdx+'">';
html+=this._renderDcCardHeader(badgeClass,typeLabel,ctrlLabel,'dc-modbus-device-addr','Addr: '+(dev.slaveAddress||1));
if(dt==='relay'){
html+=this._renderDcRelayPanel(devIdx,dev);
}else if(dt==='pwm'){
html+=this._renderDcPwmPanel(devIdx,dev);
}else if(dt==='pid'){
html+=this._renderDcPidPanel(devIdx,dev);
}
html+='<div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_renderDcRelayPanel:function (devIdx,dev){
var channelCount=dev.channelCount||2;
var states=this._dcCoilStates[devIdx];
var ncMode=!!dev.ncMode;
var html='<div class="dc-modbus-device-body">';
if(!states){
states=new Array(channelCount).fill(false);
}
html+=this._renderDcCoilGrid(devIdx,channelCount,states,ncMode);
html+=this._renderDcRelayDelaySection(devIdx,dev);
html+=this._renderDcActionBar([
{className:'dc-coil-batch dc-btn-sm dc-btn-on',devIdx:devIdx,action:'allOn',label:this._t('modbus-ctrl-all-on')},
{className:'dc-coil-batch dc-btn-sm dc-btn-off',devIdx:devIdx,action:'allOff',label:this._t('modbus-ctrl-all-off')},
{className:'dc-coil-batch dc-btn-sm dc-btn-toggle',devIdx:devIdx,action:'allToggle',label:this._t('modbus-ctrl-all-toggle')},
{className:'dc-coil-refresh dc-btn-sm dc-btn-refresh',devIdx:devIdx,label:this._t('modbus-ctrl-refresh')}
]);
html+='</div>';
return html;
},
_renderDcLoadingPlaceholder:function (message){
return '<div class="dc-loading-placeholder" style="text-align:center;padding:30px 20px;color:#999;">'
+'<div style="font-size:24px;margin-bottom:10px;">⏳</div>'
+'<div style="font-size:13px;">'+this._esc(message||'加载中...')+'</div>'
+'</div>';
},
_renderDcRelayDelaySection:function (devIdx,dev){
var channelCount=dev.channelCount||2;
var html='<div class="dc-relay-delay-section">';
html+='<div class="dc-delay-controls">';
html+='<span class="dc-delay-label">'+this._t('modbus-delay-title')+'</span>';
html+='<select class="dc-delay-channel" data-dev="'+devIdx+'">';
for(var ch=0;ch<channelCount;ch++){
html+='<option value="'+ch+'">CH'+ch+'</option>';
}
html+='</select>';
html+='<input type="number" class="dc-delay-input" min="1" max="255" value="50" data-dev="'+devIdx+'">';
html+='<button class="dc-delay-start dc-btn-sm" data-dev="'+devIdx+'">'+this._t('modbus-delay-start')+'</button>';
html+='</div></div>';
return html;
},
_renderDcPwmPanel:function (devIdx,dev){
var channelCount=dev.channelCount||4;
var resolution=dev.pwmResolution||8;
var maxValue=(1<<resolution)-1;
var states=this._dcPwmStates[devIdx];
var html='<div class="dc-modbus-device-body">';
if(!states){
states=new Array(channelCount).fill(0);
}
html+=this._renderDcPwmGrid(devIdx,channelCount,states,maxValue);
html+=this._renderDcActionBar([
{className:'dc-pwm-batch dc-btn-sm dc-btn-on',devIdx:devIdx,action:'max',label:this._t('modbus-ctrl-pwm-set-all-max')},
{className:'dc-pwm-batch dc-btn-sm dc-btn-off',devIdx:devIdx,action:'off',label:this._t('modbus-ctrl-pwm-set-all-off')},
{className:'dc-pwm-refresh dc-btn-sm dc-btn-refresh',devIdx:devIdx,label:this._t('modbus-ctrl-pwm-refresh')}
]);
html+='</div>';
return html;
},
_renderDcPidPanel:function (devIdx,dev){
var decimals=dev.pidDecimals||1;
var sf=Math.pow(10,decimals);
var values=this._dcPidValues[devIdx];
var html='<div class="dc-modbus-device-body">';
if(!values){
this._dcPidValues[devIdx]={pv:0,sv:0,out:0,p:0,i:0,d:0};
}
html+=this._renderDcPidGrid(devIdx,sf,decimals);
html+=this._renderDcActionBar([
{className:'dc-pid-refresh dc-btn-sm dc-btn-refresh',devIdx:devIdx,label:this._t('modbus-ctrl-pid-refresh')}
]);
html+='</div>';
return html;
},
_renderDcCardHeader:function (badgeClass,badgeText,title,metaClass,metaText){
var html='<div class="dc-card-header">';
html+='<span class="dc-card-badge '+badgeClass+'">'+this._esc(badgeText)+'</span>';
html+='<span class="dc-card-title">'+this._esc(title)+'</span>';
if(metaText){
html+='<span class="'+(metaClass||'')+'">'+this._esc(metaText)+'</span>';
}
html+='</div>';
return html;
},
_renderDcActionBar:function (buttons){
var html='<div class="dc-modbus-actions">';
for(var i=0;i<buttons.length;i++){
html+=this._renderDcActionButton(buttons[i]);
}
html+='</div>';
return html;
},
_renderDcActionButton:function (config){
var attrs='data-dev="'+config.devIdx+'"';
if(config.action){
attrs+=' data-action="'+this._esc(config.action)+'"';
}
if(config.param){
attrs+=' data-param="'+this._esc(config.param)+'"';
}
return '<button class="'+config.className+'" '+attrs+'>'+this._esc(config.label||'')+'</button>';
},
_renderDcCoilGrid:function (devIdx,channelCount,states,ncMode){
var html='<div class="dc-coil-grid" id="dc-coil-grid-'+devIdx+'">';
var onText=this._t('modbus-ctrl-status-on')||'ON';
var offText=this._t('modbus-ctrl-status-off')||'OFF';
for(var ch=0;ch<channelCount;ch++){
var coilState=ch<states.length?states[ch]:false;
var isOn=ncMode?!coilState:coilState;
html+=this._renderDcCoilCard(devIdx,ch,isOn,onText,offText);
}
html+='</div>';
return html;
},
_renderDcCoilCard:function (devIdx,ch,isOn,onText,offText){
var cls=isOn?'dc-coil-on':'dc-coil-off';
var html='<div class="dc-coil-card '+cls+'" data-dev="'+devIdx+'" data-ch="'+ch+'">';
html+='<div class="dc-coil-icon">'+POWER_ICON_SVG+'</div>';
html+='<div class="dc-coil-ch">CH'+ch+'</div>';
html+='<div class="dc-coil-st">'+this._esc(isOn?onText:offText)+'</div>';
html+='</div>';
return html;
},
_renderDcPwmGrid:function (devIdx,channelCount,states,maxValue){
var html='<div class="dc-pwm-list" id="dc-pwm-grid-'+devIdx+'">';
for(var ch=0;ch<channelCount;ch++){
var val=ch<states.length?states[ch]:0;
html+=this._renderDcPwmRow(devIdx,ch,val,maxValue);
}
html+='</div>';
return html;
},
_renderDcPwmRow:function (devIdx,ch,val,maxValue){
var pct=maxValue>0?Math.round(val / maxValue*100):0;
var html='<div class="dc-pwm-row">';
html+='<span class="dc-pwm-ch">CH'+ch+'</span>';
html+='<input type="range" class="dc-pwm-slider" min="0" max="'+maxValue+'" value="'+val+'" data-dev="'+devIdx+'" data-ch="'+ch+'">';
html+='<span class="dc-pwm-pct" data-dev="'+devIdx+'" data-ch="'+ch+'">'+pct+'%</span>';
html+='<input type="number" class="dc-pwm-num" min="0" max="'+maxValue+'" value="'+val+'" data-dev="'+devIdx+'" data-ch="'+ch+'">';
html+='</div>';
return html;
},
_formatDcScaledValue:function (raw,scaleFactor,decimals,suffix){
if(raw===undefined||raw===null)return '--';
return (raw / scaleFactor).toFixed(decimals)+(suffix||'');
},
_getDcPidCards:function (devIdx,scaleFactor,decimals){
var values=this._dcPidValues[devIdx]||{};
return [
{key:'out',label:this._t('modbus-ctrl-pid-out-label'),value:this._formatDcScaledValue(values.out,scaleFactor,decimals,'%'),editable:false},
{key:'pv',label:this._t('modbus-ctrl-pid-pv-label'),value:this._formatDcScaledValue(values.pv,scaleFactor,decimals),editable:false},
{key:'sv',label:this._t('modbus-ctrl-pid-sv-label'),value:this._formatDcScaledValue(values.sv,scaleFactor,decimals),editable:true},
{key:'p',label:this._t('modbus-ctrl-pid-p-label'),value:this._formatDcScaledValue(values.p,scaleFactor,decimals),editable:true},
{key:'i',label:this._t('modbus-ctrl-pid-i-label'),value:this._formatDcScaledValue(values.i,scaleFactor,decimals),editable:true},
{key:'d',label:this._t('modbus-ctrl-pid-d-label'),value:this._formatDcScaledValue(values.d,scaleFactor,decimals),editable:true}
];
},
_renderDcPidGrid:function (devIdx,scaleFactor,decimals){
var cards=this._getDcPidCards(devIdx,scaleFactor,decimals);
var html='<div class="dc-pid-grid" id="dc-pid-grid-'+devIdx+'">';
for(var ci=0;ci<cards.length;ci++){
html+=this._renderDcPidCard(devIdx,cards[ci],scaleFactor,decimals);
}
html+='</div>';
return html;
},
_renderDcPidCard:function (devIdx,card,scaleFactor,decimals){
var values=this._dcPidValues[devIdx]||{};
var cls='dc-pid-card'+(card.big?' dc-pid-pv':'')+(card.editable?' dc-pid-editable':'');
var html='<div class="'+cls+'">';
html+='<div class="dc-pid-label">'+this._esc(card.label)+'</div>';
html+='<div class="dc-pid-value">'+this._esc(card.value)+'</div>';
if(card.editable){
var rawVal=(values[card.key]!==undefined&&values[card.key]!==null)
?(values[card.key]/ scaleFactor).toFixed(decimals)
:'';
html+='<div class="dc-pid-edit">';
html+='<input type="number" class="dc-pid-input" step="'+(1 / scaleFactor)+'" value="'+rawVal+'" data-dev="'+devIdx+'" data-param="'+this._esc(card.key)+'">';
html+=this._renderDcActionButton({
className:'dc-pid-set dc-btn-sm dc-btn-on',
devIdx:devIdx,
param:card.key,
label:this._t('modbus-ctrl-pid-set')
});
html+='</div>';
}
html+='</div>';
return html;
},
_renderControlGroup:function (titleKey,items,typeClass,isSystem){
var title=this._t('device-control-group-'+titleKey.toLowerCase())||titleKey;
var html='<div class="dc-control-group" data-dc-sort-key="'+typeClass+'">';
html+='<div class="dc-control-group-title">'+title+'</div>';
html+='<div class="dc-control-grid">';
for(var i=0;i<items.length;i++){
html+=this._renderControlButton(items[i],typeClass,isSystem);
}
html+='</div><div class="dc-resize-handle" title="拖拽调整大小"></div></div>';
return html;
},
_renderControlButton:function (item,typeClass,isSystem){
var btnClass='dc-ctrl-btn dc-'+typeClass;
var dataAttrs='data-id="'+this._esc(item.id)+'"';
if(isSystem){
dataAttrs+=' data-system="true" data-name="'+this._esc(item.name)+'"';
}
return '<button class="'+btnClass+'" '+dataAttrs+'>'+
this._esc(item.name)+
'</button>';
},
_filterByActionType:function (items,actionTypes){
if(!Array.isArray(items))return [];
if(!Array.isArray(actionTypes))actionTypes=[actionTypes];
var result=[];
for(var i=0;i<items.length;i++){
var item=items[i];
if(actionTypes.indexOf(item.actionType)!==-1){
result.push(item);
}
}
return result;
},
_executeRule:function (ruleId,btn){
if(!ruleId)return ;
var originalText=btn.textContent;
btn.textContent=this._t('device-control-executing');
btn.disabled=true;
btn.classList.add('dc-loading');
var self=this;
apiPost('/api/periph-exec/run?id='+ruleId).then(function (res){
if(res&&res.success){
Notification.success(self._t('device-control-exec-success'));
}else{
Notification.error(self._t('device-control-exec-fail'));
}
}).catch(function (){
Notification.error(self._t('device-control-exec-fail'));
}).then(function (){
btn.textContent=originalText;
btn.disabled=false;
btn.classList.remove('dc-loading');
});
},
_dcModbusInitRunning:false,
_dcInitCancelled:false,
_dcCancelInit:function (){
if(this._dcModbusInitRunning){
this._dcInitCancelled=true;
console.log('[device-control] Init refresh cancelled by user action');
}
},
_dcInitModbusDeviceStates:async function (){
if(this._dcModbusInitRunning){
console.log('[device-control] _dcInitModbusDeviceStates already running, skip');
return ;
}
this._dcModbusInitRunning=true;
this._dcInitCancelled=false;
var devices=this._modbusDevices||[];
try{
for(var i=0;i<devices.length;i++){
if(this.currentPage!=='device-control')break;
if(this._dcInitCancelled)break;
var dt=devices[i].deviceType||'relay';
if(dt==='relay'){
await this._dcRefreshCoilStatus(i);
}else if(dt==='pwm'){
await this._dcRefreshPwmStatus(i);
}else if(dt==='pid'){
await this._dcRefreshPidStatus(i);
}
if(i<devices.length-1){
await new Promise(function (resolve){setTimeout(resolve,150);});
}
}
}finally{
this._dcModbusInitRunning=false;
this._dcInitCancelled=false;
}
},
_setupSSE:function (){
AppState.connectSSE();
var self=this;
this._sseModbusHandler=function (e){
try{
var data=JSON.parse(e.data);
self._handleSSEModbusData(data);
}catch(err){
console.warn('[SSE] 数据解析失败:',err);
}
};
AppState.onSSEEvent('modbus-data',this._sseModbusHandler);
},
_closeSSE:function (){
if(this._sseModbusHandler){
AppState.offSSEEvent('modbus-data',this._sseModbusHandler);
this._sseModbusHandler=null;
}
AppState.disconnectSSE();
if(this._sseDebounceTimer){
clearTimeout(this._sseDebounceTimer);
this._sseDebounceTimer=null;
}
},
_findDevIdxBySlaveAddress:function (slaveAddress){
var devices=this._modbusDevices||[];
for(var i=0;i<devices.length;i++){
if(devices[i].slaveAddress===slaveAddress){
return i;
}
}
return -1;
},
_parseSensorId:function (sensorId){
if(!sensorId)return null;
var coilMatch=sensorId.match(/^(coil|relay)_(\d+)_ch(\d+)$/i);
if(coilMatch){
return {
type:'coil',
slaveAddress:parseInt(coilMatch[2],10),
channel:parseInt(coilMatch[3],10)
};
}
var pidMatch=sensorId.match(/^pid_(pv|sv|out|p|i|d)_(\d+)$/i);
if(pidMatch){
return {
type:'pid',
paramName:pidMatch[1].toLowerCase(),
slaveAddress:parseInt(pidMatch[2],10)
};
}
var pwmMatch=sensorId.match(/^pwm_(\d+)_ch(\d+)$/i);
if(pwmMatch){
return {
type:'pwm',
slaveAddress:parseInt(pwmMatch[1],10),
channel:parseInt(pwmMatch[2],10)
};
}
return null;
},
_updateCoilUIFromSSE:function (devIdx,channel,value){
var dev=this._modbusDevices[devIdx];
if(!dev)return false;
var ncMode=dev.ncMode||false;
var isOn=ncMode?!value:!!value;
var onText=this._t('modbus-ctrl-status-on')||'ON';
var offText=this._t('modbus-ctrl-status-off')||'OFF';
var states=this._dcCoilStates[devIdx]||[];
if(channel>=0&&channel<(dev.channelCount||8)){
states[channel]=!!value;
this._dcCoilStates[devIdx]=states;
}
var card=document.querySelector('.dc-coil-card[data-dev="'+devIdx+'"][data-ch="'+channel+'"]');
if(!card)return false;
card.classList.remove('dc-coil-on','dc-coil-off');
card.classList.add(isOn?'dc-coil-on':'dc-coil-off');
var stEl=card.querySelector('.dc-coil-st');
if(stEl){
stEl.textContent=isOn?onText:offText;
}
return true;
},
_updatePwmUIFromSSE:function (devIdx,channel,value){
var dev=this._modbusDevices[devIdx];
if(!dev)return false;
var resolution=dev.pwmResolution||8;
var maxValue=(1<<resolution)-1;
var numValue=parseInt(value,10);
if(isNaN(numValue))numValue=0;
numValue=Math.max(0,Math.min (numValue,maxValue));
var states=this._dcPwmStates[devIdx]||[];
if(channel>=0){
states[channel]=numValue;
this._dcPwmStates[devIdx]=states;
}
var grid=document.getElementById('dc-pwm-grid-'+devIdx);
if(!grid)return false;
var slider=grid.querySelector('.dc-pwm-slider[data-ch="'+channel+'"]');
if(slider)slider.value=numValue;
var numInput=grid.querySelector('.dc-pwm-num[data-ch="'+channel+'"]');
if(numInput)numInput.value=numValue;
var pctSpan=grid.querySelector('.dc-pwm-pct[data-ch="'+channel+'"]');
if(pctSpan){
var pct=maxValue>0?Math.round(numValue / maxValue*100):0;
pctSpan.textContent=pct+'%';
}
return true;
},
_updatePidUIFromSSE:function (devIdx,paramName,value){
var dev=this._modbusDevices[devIdx];
if(!dev)return false;
var decimals=dev.pidDecimals||1;
var scaleFactor=Math.pow(10,decimals);
var numValue=parseFloat(value);
if(isNaN(numValue))numValue=0;
var rawValue=Math.round(numValue*scaleFactor);
var values=this._dcPidValues[devIdx]||{};
values[paramName]=rawValue;
this._dcPidValues[devIdx]=values;
var grid=document.getElementById('dc-pid-grid-'+devIdx);
if(!grid)return false;
var cards=grid.querySelectorAll('.dc-pid-card');
for(var i=0;i<cards.length;i++){
var card=cards[i];
var labelEl=card.querySelector('.dc-pid-label');
if(!labelEl)continue;
var labelText=(labelEl.textContent||'').toLowerCase();
var paramLabels={
'pv':this._t('modbus-ctrl-pid-pv-label')||'pv',
'sv':this._t('modbus-ctrl-pid-sv-label')||'sv',
'out':this._t('modbus-ctrl-pid-out-label')||'out',
'p':this._t('modbus-ctrl-pid-p-label')||'p',
'i':this._t('modbus-ctrl-pid-i-label')||'i',
'd':this._t('modbus-ctrl-pid-d-label')||'d'
};
var expectedLabel=(paramLabels[paramName]||paramName).toLowerCase();
if(labelText.indexOf(expectedLabel)!==-1||labelText.indexOf(paramName)!==-1){
var valueEl=card.querySelector('.dc-pid-value');
if(valueEl){
valueEl.textContent=numValue.toFixed(decimals);
}
var inputEl=card.querySelector('.dc-pid-input');
if(inputEl){
inputEl.value=numValue.toFixed(decimals);
}
break;
}
}
return true;
},
_handleSSEModbusData:function (data){
var self=this;
var tasks=this._modbusStatus&&this._modbusStatus.tasks?this._modbusStatus.tasks:[];
var hasMonitorUpdate=false;
var needsFullRefresh=false;
if(!Array.isArray(data)||data.length===0){
return ;
}
for(var i=0;i<data.length;i++){
var item=data[i]||{};
var sensorId=item.sensorId||item.id;
var value=item.value;
var matched=false;
if(!sensorId){
continue;
}
var parsedInfo=this._parseSensorId(sensorId);
if(parsedInfo){
var devIdx=this._findDevIdxBySlaveAddress(parsedInfo.slaveAddress);
if(devIdx>=0){
if(parsedInfo.type==='coil'){
if(this._updateCoilUIFromSSE(devIdx,parsedInfo.channel,value)){
matched=true;
continue;
}
}else if(parsedInfo.type==='pwm'){
if(this._updatePwmUIFromSSE(devIdx,parsedInfo.channel,value)){
matched=true;
continue;
}
}else if(parsedInfo.type==='pid'){
if(this._updatePidUIFromSSE(devIdx,parsedInfo.paramName,value)){
matched=true;
continue;
}
}
}
if(!matched){
needsFullRefresh=true;
continue;
}
}
for(var ti=0;ti<tasks.length;ti++){
var task=tasks[ti];
var mappings=task&&task.mappings?task.mappings:[];
for(var mi=0;mi<mappings.length;mi++){
var mapping=mappings[mi];
if(mapping&&mapping.sensorId===sensorId){
matched=true;
if(!task.cachedData){
task.cachedData={values:[]};
}else if(!Array.isArray(task.cachedData.values)){
task.cachedData.values=[];
}
var valueNum=Number(value);
var scaleFactor=Number(mapping.scaleFactor||1);
if(isFinite(valueNum)&&scaleFactor){
task.cachedData.values[mapping.regOffset]=valueNum / scaleFactor;
}
break;
}
}
if(matched){
break;
}
}
var valueEl=document.querySelector('.dc-monitor-card[data-id="'+sensorId+'"] .dc-monitor-value');
if(valueEl){
valueEl.textContent=value===undefined||value===null||value===''?'--':String(value);
hasMonitorUpdate=true;
matched=true;
}
if(!matched){
needsFullRefresh=true;
}
}
if(needsFullRefresh){
this._dcAutoRefreshTimers=this._dcAutoRefreshTimers||{};
if(this._dcAutoRefreshTimers.sseDebounce){
clearTimeout(this._dcAutoRefreshTimers.sseDebounce);
}
this._dcAutoRefreshTimers.sseDebounce=setTimeout(function (){
self._dcAutoRefreshTimers.sseDebounce=null;
if(self.currentPage!=='device-control'){
return ;
}
self._dcInitModbusDeviceStates();
},100);
}
if(!needsFullRefresh&&!hasMonitorUpdate){
console.warn('[SSE] 未匹配到可更新的设备数据');
}
},
_dcStopAllAutoRefresh:function (){
var timers=this._dcAutoRefreshTimers||{};
this._closeSSE();
for(var key in timers){
if(timers.hasOwnProperty(key)&&timers[key]){
clearInterval(timers[key]);
clearTimeout(timers[key]);
}
}
this._dcAutoRefreshTimers={};
},
_dcGetCoilParams:function (devIdx){
var dev=this._modbusDevices[devIdx]||{};
return {
slaveAddress:dev.slaveAddress||1,
channelCount:dev.channelCount||2,
coilBase:dev.coilBase||0,
ncMode:!!dev.ncMode,
relayMode:(dev.controlProtocol===1)?'register':'coil'
};
},
_dcRefreshCoilStatus:function (devIdx){
var self=this;
var p=this._dcGetCoilParams(devIdx);
return apiGetSilent('/api/modbus/coil/status',{
slaveAddress:p.slaveAddress,channelCount:p.channelCount,
coilBase:p.coilBase,mode:p.relayMode
}).then(function (res){
if(res&&res.success&&res.data&&res.data.states){
self._dcCoilStates[devIdx]=res.data.states;
self._dcUpdateAllCoilUI(devIdx);
}
}).catch(function (){
if(!self._dcCoilStates[devIdx]){
self._dcCoilStates[devIdx]=new Array(p.channelCount).fill(false);
self._dcRerenderCoilGrid(devIdx);
}
});
},
_dcRerenderCoilGrid:function (devIdx){
var grid=document.getElementById('dc-coil-grid-'+devIdx);
if(!grid)return ;
var p=this._dcGetCoilParams(devIdx);
var states=this._dcCoilStates[devIdx]||[];
grid.outerHTML=this._renderDcCoilGrid(devIdx,p.channelCount,states,p.ncMode);
},
_dcUpdateSingleCoilUI:function (devIdx,ch){
var dev=this._modbusDevices[devIdx];
if(!dev)return false;
var ncMode=dev.ncMode||false;
var states=this._dcCoilStates[devIdx]||[];
var coilState=ch<states.length?states[ch]:false;
var isOn=ncMode?!coilState:coilState;
var card=document.querySelector('.dc-coil-card[data-dev="'+devIdx+'"][data-ch="'+ch+'"]');
if(!card){this._dcRerenderCoilGrid(devIdx);return false;}
card.classList.remove('dc-coil-on','dc-coil-off');
card.classList.add(isOn?'dc-coil-on':'dc-coil-off');
var stEl=card.querySelector('.dc-coil-st');
if(stEl)stEl.textContent=isOn
?(this._t('modbus-ctrl-status-on')||'ON')
:(this._t('modbus-ctrl-status-off')||'OFF');
return true;
},
_dcUpdateAllCoilUI:function (devIdx){
var dev=this._modbusDevices[devIdx];
if(!dev)return ;
var count=dev.channelCount||8;
var grid=document.getElementById('dc-coil-grid-'+devIdx);
if(!grid){this._dcRerenderCoilGrid(devIdx);return ;}
for(var ch=0;ch<count;ch++){
this._dcUpdateSingleCoilUI(devIdx,ch);
}
},
_dcToggleCoil:function (devIdx,ch,cardEl){
this._dcCancelInit();
var self=this;
var p=this._dcGetCoilParams(devIdx);
var states=this._dcCoilStates[devIdx]||[];
var prevState=(ch<states.length)?states[ch]:false;
if(ch<states.length)states[ch]=!prevState;
this._dcCoilStates[devIdx]=states;
this._dcUpdateSingleCoilUI(devIdx,ch);
if(cardEl){
cardEl.style.pointerEvents='none';
}
apiPost('/api/modbus/coil/control',{
slaveAddress:p.slaveAddress,channel:ch,coilBase:p.coilBase,
action:'toggle',mode:p.relayMode
}).then(function (res){
if(res&&res.success&&res.data){
var st=self._dcCoilStates[devIdx]||[];
if(ch<st.length&&st[ch]!==res.data.state){
st[ch]=res.data.state;
self._dcCoilStates[devIdx]=st;
self._dcUpdateSingleCoilUI(devIdx,ch);
}
}else{
var st=self._dcCoilStates[devIdx]||[];
if(ch<st.length)st[ch]=prevState;
self._dcCoilStates[devIdx]=st;
self._dcUpdateSingleCoilUI(devIdx,ch);
Notification.error((res&&res.error)||self._t('modbus-ctrl-fail'));
}
if(cardEl)cardEl.style.pointerEvents='';
self._dcCoilPending[devIdx+'_'+ch]=false;
},function (){
var st=self._dcCoilStates[devIdx]||[];
if(ch<st.length)st[ch]=prevState;
self._dcCoilStates[devIdx]=st;
self._dcUpdateSingleCoilUI(devIdx,ch);
Notification.error(self._t('modbus-ctrl-fail'));
if(cardEl)cardEl.style.pointerEvents='';
self._dcCoilPending[devIdx+'_'+ch]=false;
});
},
_dcBatchCoil:function (devIdx,action,btnEl){
this._dcCancelInit();
var self=this;
var p=this._dcGetCoilParams(devIdx);
var modbusAction=action;
if(p.ncMode){
if(action==='allOn')modbusAction='allOff';
else if(action==='allOff')modbusAction='allOn';
}
var prevStates=(this._dcCoilStates[devIdx]||[]).slice();
var targetVal;
if(action==='allOn')targetVal=true;
else if(action==='allOff')targetVal=false;
else targetVal=null;
var optimistic=prevStates.map(function (s){
return targetVal!==null?targetVal:!s;
});
this._dcCoilStates[devIdx]=optimistic;
this._dcUpdateAllCoilUI(devIdx);
apiPost('/api/modbus/coil/batch',{
slaveAddress:p.slaveAddress,channelCount:p.channelCount,
coilBase:p.coilBase,action:modbusAction,mode:p.relayMode
}).then(function (res){
if(res&&res.success&&res.data&&res.data.states){
self._dcCoilStates[devIdx]=res.data.states;
self._dcUpdateAllCoilUI(devIdx);
}else{
self._dcCoilStates[devIdx]=prevStates;
self._dcUpdateAllCoilUI(devIdx);
Notification.error((res&&res.error)||self._t('modbus-ctrl-fail'));
}
if(btnEl)btnEl.disabled=false;
self._dcBatchPending=false;
},function (){
self._dcCoilStates[devIdx]=prevStates;
self._dcUpdateAllCoilUI(devIdx);
Notification.error(self._t('modbus-ctrl-fail'));
if(btnEl)btnEl.disabled=false;
self._dcBatchPending=false;
});
},
_dcStartCoilDelay:function (devIdx,channel,delayUnits){
this._dcCancelInit();
var self=this;
var p=this._dcGetCoilParams(devIdx);
var dev=this._modbusDevices[devIdx]||{};
apiPost('/api/modbus/coil/delay',{
slaveAddress:p.slaveAddress,
channel:channel,
delayBase:0x0200,
delayUnits:delayUnits,
ncMode:!!dev.ncMode,
coilBase:p.coilBase,
mode:p.relayMode
}).then(function (res){
if(res&&res.success){
window.Notification&&Notification.success(
self._t('modbus-delay-success')+' CH'+channel+' '+(delayUnits*0.1).toFixed(1)+'s'
);
}else{
window.Notification&&Notification.error(
(res&&res.error)||self._t('modbus-delay-fail')
);
}
}).catch(function (){
window.Notification&&Notification.error(self._t('modbus-delay-fail'));
});
},
_dcGetPwmParams:function (devIdx){
var dev=this._modbusDevices[devIdx]||{};
var res=dev.pwmResolution||8;
return {
slaveAddress:dev.slaveAddress||1,
channelCount:dev.channelCount||4,
regBase:dev.pwmRegBase||0,
resolution:res,
maxValue:(1<<res)-1
};
},
_dcRefreshPwmStatus:function (devIdx){
var self=this;
var p=this._dcGetPwmParams(devIdx);
return apiGetSilent('/api/modbus/register/read',{
slaveAddress:p.slaveAddress,
startAddress:p.regBase,
quantity:p.channelCount,
functionCode:3
}).then(function (res){
if(res&&res.success&&res.data&&res.data.values){
self._dcPwmStates[devIdx]=res.data.values;
self._dcRerenderPwmGrid(devIdx);
}
}).catch(function (){
if(!self._dcPwmStates[devIdx]){
self._dcPwmStates[devIdx]=new Array(p.channelCount).fill(0);
self._dcRerenderPwmGrid(devIdx);
}
});
},
_dcRerenderPwmGrid:function (devIdx){
var grid=document.getElementById('dc-pwm-grid-'+devIdx);
if(!grid)return ;
var p=this._dcGetPwmParams(devIdx);
var states=this._dcPwmStates[devIdx]||[];
grid.outerHTML=this._renderDcPwmGrid(devIdx,p.channelCount,states,p.maxValue);
},
_dcOnPwmSliderInput:function (devIdx,ch,val){
var grid=document.getElementById('dc-pwm-grid-'+devIdx);
if(!grid)return ;
var numInput=grid.querySelector('.dc-pwm-num[data-ch="'+ch+'"]');
var pctSpan=grid.querySelector('.dc-pwm-pct[data-ch="'+ch+'"]');
var p=this._dcGetPwmParams(devIdx);
if(numInput)numInput.value=val;
if(pctSpan)pctSpan.textContent=(p.maxValue>0?Math.round(val / p.maxValue*100):0)+'%';
},
_dcSetPwmChannel:function (devIdx,ch,value){
this._dcCancelInit();
var self=this;
var p=this._dcGetPwmParams(devIdx);
value=Math.max(0,Math.min (value,p.maxValue));
var states=(this._dcPwmStates[devIdx]||[]).slice();
var prevVal=ch<states.length?states[ch]:0;
if(ch<states.length)states[ch]=value;
this._dcPwmStates[devIdx]=states;
this._dcRerenderPwmGrid(devIdx);
apiPost('/api/modbus/register/write',{
slaveAddress:p.slaveAddress,
registerAddress:p.regBase+ch,
value:value
}).then(function (res){
if(res&&res.success){
Notification.success(self._t('modbus-ctrl-success'));
}else{
var s=(self._dcPwmStates[devIdx]||[]).slice();
if(ch<s.length)s[ch]=prevVal;
self._dcPwmStates[devIdx]=s;
self._dcRerenderPwmGrid(devIdx);
Notification.error((res&&res.error)||self._t('modbus-ctrl-fail'));
}
}).catch(function (){
var s=(self._dcPwmStates[devIdx]||[]).slice();
if(ch<s.length)s[ch]=prevVal;
self._dcPwmStates[devIdx]=s;
self._dcRerenderPwmGrid(devIdx);
Notification.error(self._t('modbus-ctrl-fail'));
});
},
_dcBatchPwm:function (devIdx,action){
this._dcCancelInit();
var self=this;
var p=this._dcGetPwmParams(devIdx);
var prevStates=(this._dcPwmStates[devIdx]||[]).slice();
var values=[];
var fillVal=action==='max'?p.maxValue:0;
for(var i=0;i<p.channelCount;i++)values.push(fillVal);
this._dcPwmStates[devIdx]=values.slice();
this._dcRerenderPwmGrid(devIdx);
apiPost('/api/modbus/register/batch-write',{
slaveAddress:p.slaveAddress,
startAddress:p.regBase,
values:JSON.stringify(values)
}).then(function (res){
if(res&&res.success){
Notification.success(self._t('modbus-ctrl-success'));
}else{
self._dcPwmStates[devIdx]=prevStates;
self._dcRerenderPwmGrid(devIdx);
Notification.error((res&&res.error)||self._t('modbus-ctrl-fail'));
}
}).catch(function (){
self._dcPwmStates[devIdx]=prevStates;
self._dcRerenderPwmGrid(devIdx);
Notification.error(self._t('modbus-ctrl-fail'));
});
},
_dcGetPidParams:function (devIdx){
var dev=this._modbusDevices[devIdx]||{};
var pidA=dev.pidAddrs||[0,1,2,3,4,5];
var decimals=dev.pidDecimals||1;
return {
slaveAddress:dev.slaveAddress||1,
pvAddr:pidA[0]||0,svAddr:pidA[1]||1,outAddr:pidA[2]||2,
pAddr:pidA[3]||3,iAddr:pidA[4]||4,dAddr:pidA[5]||5,
decimals:decimals,
scaleFactor:Math.pow(10,decimals)
};
},
_dcRefreshPidStatus:function (devIdx){
var self=this;
var p=this._dcGetPidParams(devIdx);
if(!p.slaveAddress)return Promise.resolve();
var addrs=[p.pvAddr,p.svAddr,p.outAddr,p.pAddr,p.iAddr,p.dAddr];
var minAddr=Math.min .apply(null,addrs);
var maxAddr=Math.max.apply(null,addrs);
var quantity=maxAddr-minAddr+1;
if(quantity>125)return Promise.resolve();
return apiGetSilent('/api/modbus/register/read',{
slaveAddress:p.slaveAddress,startAddress:minAddr,
quantity:quantity,functionCode:3
}).then(function (res){
if(res&&res.success&&res.data&&res.data.values){
var vals=res.data.values;
self._dcPidValues[devIdx]={
pv:vals[p.pvAddr-minAddr],sv:vals[p.svAddr-minAddr],
out:vals[p.outAddr-minAddr],p:vals[p.pAddr-minAddr],
i:vals[p.iAddr-minAddr],d:vals[p.dAddr-minAddr]
};
self._dcRerenderPidGrid(devIdx);
}
}).catch(function (){
if(!self._dcPidValues[devIdx]){
self._dcPidValues[devIdx]={pv:0,sv:0,out:0,p:0,i:0,d:0};
self._dcRerenderPidGrid(devIdx);
}
});
},
_dcRerenderPidGrid:function (devIdx){
var grid=document.getElementById('dc-pid-grid-'+devIdx);
if(!grid)return ;
var p=this._dcGetPidParams(devIdx);
grid.outerHTML=this._renderDcPidGrid(devIdx,p.scaleFactor,p.decimals);
},
_dcSetPidParam:function (devIdx,paramName,displayValue){
this._dcCancelInit();
var self=this;
var p=this._dcGetPidParams(devIdx);
var addrMap={sv:p.svAddr,p:p.pAddr,i:p.iAddr,d:p.dAddr};
var addr=addrMap[paramName];
if(addr===undefined)return ;
var rawValue=Math.round(parseFloat(displayValue)*p.scaleFactor);
if(isNaN(rawValue))return ;
apiPost('/api/modbus/register/write',{
slaveAddress:p.slaveAddress,registerAddress:addr,value:rawValue
}).then(function (res){
if(res&&res.success){
var v=self._dcPidValues[devIdx]||{};
v[paramName]=rawValue;
self._dcPidValues[devIdx]=v;
self._dcRerenderPidGrid(devIdx);
Notification.success(self._t('modbus-ctrl-success'));
}else{
Notification.error((res&&res.error)||self._t('modbus-ctrl-fail'));
}
}).catch(function (){
Notification.error(self._t('modbus-ctrl-fail'));
});
},
_DC_LAYOUT_KEY:'dc-card-layout',
_dcDragEl:null,
_dcDragOffset:null,
_dcResizeEl:null,
_dcResizeStart:null,
_dcInitFreeLayout:function (){
var flow=document.querySelector('.dc-control-flow');
if(!flow||flow._dcLayoutBound)return ;
var self=this;
flow.addEventListener('mousedown',function (e){
var card=e.target.closest('[data-dc-sort-key]');
if(!card)return ;
var resizeHandle=e.target.closest('.dc-resize-handle');
if(resizeHandle){
e.preventDefault();
e.stopPropagation();
self._dcResizeEl=card;
self._dcResizeStart={
x:e.clientX,
y:e.clientY,
w:card.offsetWidth,
h:card.offsetHeight
};
card.classList.add('dc-resizing');
return ;
}
if(e.target.closest('button,input,select,textarea,.dc-pwm-slider'))return ;
e.preventDefault();
self._dcDragEl=card;
var rect=card.getBoundingClientRect();
self._dcDragOffset={x:e.clientX-rect.left,y:e.clientY-rect.top};
card.classList.add('dc-dragging');
card.style.zIndex='100';
});
document.addEventListener('mousemove',function (e){
if(self._dcResizeEl){
var dx=e.clientX-self._dcResizeStart.x;
var dy=e.clientY-self._dcResizeStart.y;
var newW=Math.max(200,Math.min (self._dcResizeStart.w+dx,flow.clientWidth));
var newH=Math.max(100,self._dcResizeStart.h+dy);
self._dcResizeEl.style.width=newW+'px';
self._dcResizeEl.style.height=newH+'px';
return ;
}
if(!self._dcDragEl)return ;
var flowRect=flow.getBoundingClientRect();
if(!flowRect)return ;
var x=e.clientX-flowRect.left-self._dcDragOffset.x;
var y=e.clientY-flowRect.top-self._dcDragOffset.y;
x=Math.max(0,Math.min (x,flow.clientWidth-self._dcDragEl.offsetWidth));
y=Math.max(0,y);
self._dcDragEl.style.left=x+'px';
self._dcDragEl.style.top=y+'px';
});
document.addEventListener('mouseup',function (){
if(self._dcResizeEl){
self._dcResizeEl.classList.remove('dc-resizing');
self._dcSaveLayout();
self._dcResizeEl=null;
self._dcResizeStart=null;
return ;
}
if(!self._dcDragEl)return ;
self._dcDragEl.classList.remove('dc-dragging');
self._dcDragEl.style.zIndex='';
self._dcDragEl=null;
self._dcSaveLayout();
});
flow._dcLayoutBound=true;
},
_dcApplyLayout:function (){
var flow=document.querySelector('.dc-control-flow');
if(!flow)return ;
var cards=flow.querySelectorAll('[data-dc-sort-key]');
if(cards.length===0)return ;
var saved=null;
try{saved=JSON.parse(localStorage.getItem(this._DC_LAYOUT_KEY));}catch(e){}
var maxBottom=0;
if(saved&&typeof saved==='object'){
for(var i=0;i<cards.length;i++){
var key=cards[i].getAttribute('data-dc-sort-key');
var pos=saved[key];
if(pos){
cards[i].style.left=pos.x+'px';
cards[i].style.top=pos.y+'px';
if(pos.w){
cards[i].style.width=pos.w+'px';
}
if(pos.h){
cards[i].style.height=pos.h+'px';
}
}else{
cards[i].style.left='0px';
cards[i].style.top=maxBottom+'px';
}
var bottom=parseInt(cards[i].style.top)+cards[i].offsetHeight;
if(bottom>maxBottom)maxBottom=bottom;
}
}else{
this._dcAutoGridLayout(flow,cards);
for(var i=0;i<cards.length;i++){
var bottom=parseInt(cards[i].style.top||0)+cards[i].offsetHeight;
if(bottom>maxBottom)maxBottom=bottom;
}
}
flow.style.minHeight=(maxBottom+20)+'px';
},
_dcAutoGridLayout:function (flow,cards){
var gap=15;
var colWidth=340+gap;
var cols=Math.max(1,Math.floor(flow.clientWidth / colWidth));
var colTops=[];
for(var c=0;c<cols;c++)colTops.push(0);
for(var i=0;i<cards.length;i++){
var key=cards[i].getAttribute('data-dc-sort-key');
var isWide=(key==='health'||key==='monitor-data');
var span=(isWide&&cols>=2)?2:1;
var bestCol=0;
var bestTop=Infinity;
for(var c=0;c<=cols-span;c++){
var maxTop=0;
for(var s=0;s<span;s++){
if(colTops[c+s]>maxTop)maxTop=colTops[c+s];
}
if(maxTop<bestTop){bestTop=maxTop;bestCol=c;}
}
cards[i].style.left=(bestCol*colWidth)+'px';
cards[i].style.top=bestTop+'px';
var cardHeight=cards[i].offsetHeight+gap;
for(var s=0;s<span;s++){
colTops[bestCol+s]=bestTop+cardHeight;
}
}
},
_dcSaveLayout:function (){
var flow=document.querySelector('.dc-control-flow');
if(!flow)return ;
var cards=flow.querySelectorAll('[data-dc-sort-key]');
var layout={};
var maxBottom=0;
for(var i=0;i<cards.length;i++){
var key=cards[i].getAttribute('data-dc-sort-key');
var layoutData={x:parseInt(cards[i].style.left)||0,y:parseInt(cards[i].style.top)||0};
if(cards[i].style.width){
layoutData.w=parseInt(cards[i].style.width,10);
}
if(cards[i].style.height){
layoutData.h=parseInt(cards[i].style.height,10);
}
layout[key]=layoutData;
var bottom=layoutData.y+cards[i].offsetHeight;
if(bottom>maxBottom)maxBottom=bottom;
}
flow.style.minHeight=(maxBottom+20)+'px';
try{localStorage.setItem(this._DC_LAYOUT_KEY,JSON.stringify(layout));}catch(e){}
},
_dcResetLayout:function (){
try{localStorage.removeItem(this._DC_LAYOUT_KEY);}catch(e){}
var flow=document.querySelector('.dc-control-flow');
if(!flow)return ;
var cards=flow.querySelectorAll('[data-dc-sort-key]');
for(var i=0;i<cards.length;i++){
cards[i].style.width='';
cards[i].style.height='';
}
this._dcAutoGridLayout(flow,cards);
this._dcSaveLayout();
},
_setContentState:function (container,type,message){
if(type==='empty'){
container.innerHTML=this._renderEmptyState();
}else if(type==='loading'){
var loadingText=message||(typeof i18n!=='undefined'?i18n.t('loading'):'加载中...');
container.innerHTML='<div class="dc-empty">'+this._esc(loadingText)+'</div>';
}else if(type==='error'){
var errorText=message||'请求失败';
container.innerHTML='<div class="dc-empty u-text-danger">'+this._esc(errorText)+'</div>';
}
},
_renderEmptyState:function (){
return '<div class="dc-device-banner">'+DEVICE_ICON_SVG+
'<div class="dc-device-banner-info">'+
'<div class="dc-device-name">'+this._esc(this._deviceName)+'</div>'+
'<div class="dc-device-status">'+this._t('device-control-online')+'</div>'+
'</div></div>'+
'<div class="dc-section">'+
'<div class="dc-section-title">'+this._t('device-control-dashboard')+'</div>'+
'<div class="dc-empty">'+this._t('device-control-no-monitor')+'</div>'+
'</div>'+
'<div class="dc-section">'+
'<div class="dc-section-title">'+this._t('device-control-action-section')+'</div>'+
'<div class="dc-empty">'+this._t('device-control-no-action')+'</div>'+
'</div>';
},
_t:function (key){
if(typeof i18n!=='undefined'&&typeof i18n.t==='function'){
return i18n.t(key)||key;
}
return key;
},
_esc:function (str){
if(str==null)return '';
return String(str).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g, '&#39;');
}
});
console.log('[device-control] Module loaded, checking setupDeviceControlEvents...');
setTimeout(function (){
if(typeof AppState.setupDeviceControlEvents==='function'){
AppState.setupDeviceControlEvents();
console.log('[device-control] Events bound successfully');
}else{
console.warn('[device-control] setupDeviceControlEvents not found after registerModule');
}
},0);
})();