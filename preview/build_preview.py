#!/usr/bin/env python3
# 从 code/web_src 组装本地预览页；仅用于看样式，不影响固件。
import io
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "code", "web_src")
OUT = os.path.join(ROOT, "preview", "index.html")
PANELS = ["overview", "sim", "inbox", "settings", "push", "keepalive", "diagnose", "atterm", "log"]


def read(path):
    return io.open(path, encoding="utf-8").read().replace("{{ASSET_HASH}}", "preview")


index = read(os.path.join(SRC, "index.html"))
css = read(os.path.join(SRC, "app.css"))
js = read(os.path.join(SRC, "app.js"))
panels = {name: read(os.path.join(SRC, "panels", name + ".html")) for name in PANELS}

config = {
    "assetHash": "preview",
    "uptimeText": "12:34:56",
    "webUser": "admin",
    "webPass": "admin123",
    "smtpServer": "smtp.qq.com",
    "smtpPort": 465,
    "smtpUser": "you@qq.com",
    "smtpPass": "",
    "smtpSendTo": "recv@example.com",
    "adminPhone": "+447700900456",
    "numberBlackList": "",
    "forwardRules": "kw\t验证码,银行\temail\t1\nfrom\t^(10086|10010)\tdrop\t1",
    "emailEnabled": True,
    "pushEnabled": True,
    "emailConfigured": True,
    "modemReady": True,
    "pushEnabledCount": 1,
    "inboxMax": 50,
    "ntpServer": "ntp.aliyun.com",
    "tzOffsetMin": 480,
    "rebootEnabled": False,
    "rebootHour": 4,
    "hbEnabled": True,
    "hbHour": 9,
    "dataEnabled": False,
    "apn": "",
    "phoneNumber": "+447700900123",
    "operatorPlmn": "",
    "pushChannels": [
        {"enabled": True, "type": 2, "name": "Bark 推送", "url": "https://api.day.app/yourkey", "key1": "", "key2": "", "customBody": ""},
        {"enabled": False, "type": 1, "name": "通道2", "url": "", "key1": "", "key2": "", "customBody": ""},
        {"enabled": False, "type": 1, "name": "通道3", "url": "", "key1": "", "key2": "", "customBody": ""},
        {"enabled": False, "type": 1, "name": "通道4", "url": "", "key1": "", "key2": "", "customBody": ""},
        {"enabled": False, "type": 1, "name": "通道5", "url": "", "key1": "", "key2": "", "customBody": ""},
    ],
}

stub = f"""<script>
/* PREVIEW ONLY — 桩 fetch，使动态面板以示例数据渲染，不影响真实固件 */
(function(){{
  var panels = {json.dumps(panels, ensure_ascii=False)};
  var config = {json.dumps(config, ensure_ascii=False)};
  function resp(obj, asText) {{ return Promise.resolve({{
    ok: true,
    json: function() {{ return Promise.resolve(obj); }},
    text: function() {{ return Promise.resolve(asText != null ? asText : JSON.stringify(obj)); }}
  }}); }}
  window.fetch = function(u) {{ u = String(u);
    if (u.indexOf('/config.json') >= 0) return resp(config);
    if (u.indexOf('/ui?') >= 0) {{ var m = u.match(/[?&]panel=([^&]+)/); var n = m ? decodeURIComponent(m[1]) : 'overview'; return resp({{}}, panels[n] || panels.overview); }}
    if (u.indexOf('/wifiscan') >= 0) return resp([{{ssid:'MyHomeWiFi',rssi:-48,enc:1}},{{ssid:'Office-5G',rssi:-67,enc:1}},{{ssid:'CMCC-Free',rssi:-72,enc:0}}]);
    if (u.indexOf('/status') >= 0) return resp({{version:'1.0.2',modemReady:true,wifiConnected:true,apMode:false,ssid:'MyHomeWiFi',rssi:-62,csq:18,ber:0,rsrp:-82,rsrq:-11,sinr:14,pci:415,plmn:'23410',tac:'71F9',tz:480,nowEpoch:1750000000,operator:'giffgaff',imei:'860000000000000',iccid:'89440000000000000000',imsi:'234100000000000',apnSim:'',mfr:'MeiG',model:'ML307R',fwver:'preview',phone:'+447700900123',dataEnabled:false,apn:'',cellIp:'',inboxCount:3,ip:'192.168.1.50',gw:'192.168.1.1',mask:'255.255.255.0',dns:'192.168.1.1',mac:'AA:BB:CC:DD:EE:FF',bssid:'11:22:33:44:55:66',chan:6,freeHeap:215000,minFreeHeap:180000,maxAllocHeap:110000,uptime:45230,queueDepth:0,fwdQueueDepth:0,outSmsQueueDepth:0,emailQueueDepth:0,slowBusy:false,emailEnabled:true,emailConfigured:true,pushEnabled:true,pushEnabledCount:1,adminPhone:'+447700900456',smsTotal:42,lastSmsEpoch:1750000000,resetReason:1,configValid:true,timeSynced:true,chipTemp:42.5}});
    if (u.indexOf('/messages') >= 0 && u.indexOf('box=sent') >= 0) return resp([{{id:3,sent:1750000200,target:'10086',text:'CXLL',ok:true}},{{id:2,sent:1749990500,target:'+447700900456',text:'测试转发：设备运行正常',ok:true}}]);
    if (u.indexOf('/messages') >= 0) return resp([{{id:42,recv:1750000000,sender:'10086',ts:'2026-06-25 00:01:02',text:'【中国移动】尊敬的客户，您的话费余额为 38.50 元。',fwd:true}},{{id:41,recv:1749990000,sender:'+447700900456',ts:'2026-06-24 21:15:00',text:'您的验证码是 482913，5分钟内有效，请勿泄露。',fwd:true}},{{id:40,recv:1749980000,sender:'giffgaff',ts:'2026-06-24 18:30:00',text:'Your balance is now active.',fwd:false}}]);
    if (u.indexOf('/keepalive') >= 0) return resp({{enabled:true,intervalDays:175,action:1,target:'',url:'http://gg.incrafttime.top/api/payload?size=64342',lastTime:1740000000,daysLeft:120,timeValid:true}});
    if (u.indexOf('/log') >= 0) return resp({{seq:4,lines:['[启动] 固件 1.0.2 启动','wifi已连接','IP地址: 192.168.1.50','模组AT响应正常','网络已注册']}});
    return resp({{success:true,message:'(预览模式：示例响应)',queued:false,running:false}});
  }};
}})();
</script>"""

index = index.replace('<link rel="stylesheet" href="/assets/app.css?v=preview">', "<style>\n" + css + "\n</style>")
index = index.replace('<script src="/assets/app.js?v=preview" defer></script>', stub + "\n<script>\n" + js + "\n</script>")

io.open(OUT, "w", encoding="utf-8", newline="").write(index)
print("已生成", OUT, "— 用浏览器打开即可预览样式")
