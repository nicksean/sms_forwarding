#include "modem.h"
#include "web_handlers.h"
#include "inbox.h"
#include "sms_process.h"
#include "push.h"
#include <esp_system.h>

static bool modemSerialBusy = false;
static unsigned long lastModemAtEndMs = 0;
static bool modemRegistrationPending = false;
static unsigned long nextRegistrationCheckMs = 0;
static uint8_t registrationCheckCount = 0;
static uint8_t postRegisterStep = 0;
static bool dataModeRetryPending = false;
static unsigned long nextDataModeRetryMs = 0;
static uint8_t dataModeRetryCount = 0;
static unsigned long signalLastFastMs = 0;
static unsigned long signalLastDetailMs = 0;

static bool sampleSignalFastNow(unsigned long timeout = 1200);
static void noteNetworkRegistered();

bool modemSerialTryBegin(const char* label) {
  if (modemSerialBusy) {
    logCaptureLn(String("模组串口正忙，暂缓操作: ") + label);
    return false;
  }
  modemSerialBusy = true;
  return true;
}

void modemSerialEnd() {
  lastModemAtEndMs = millis();
  modemSerialBusy = false;
}

bool modemAtQuietFor(unsigned long gapMs) {
  if (modemSerialBusy) return false;
  return lastModemAtEndMs == 0 || millis() - lastModemAtEndMs >= gapMs;
}

static bool beginModemSerialOp(const char* label) {
  return modemSerialTryBegin(label);
}

static void endModemSerialOp() {
  modemSerialEnd();
}

static void pumpWebServerDuringWait() { pumpWebDuringWait(); }  // 统一实现见 globals.h

static void waitWithWebPump(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) pumpWebServerDuringWait();
}

static bool initSmsModeCommand(const char* cmd, const char* okMsg, const char* retryMsg, const char* failMsg) {
  for (int tries = 0; tries < MODEM_INIT_SMS_CMD_RETRIES; tries++) {
    if (sendATandWaitOK(cmd, 1000)) {
      logCaptureLn(okMsg);
      return true;
    }
    if (tries + 1 < MODEM_INIT_SMS_CMD_RETRIES) {
      logCaptureLn(retryMsg);
      waitWithWebPump(MODEM_INIT_SMS_RETRY_GAP_MS);
    }
  }
  logCaptureLn(failMsg);
  requestSmsReceiveWatchdogReassert();
  return false;
}

static bool drainSerial1PreservingSms(const char* label) {
  if (!Serial1.available()) return true;

  String pending;
  pending.reserve(160);
  unsigned long start = millis();
  while (millis() - start < 30) {
    while (Serial1.available()) {
      if (pending.length() < (MAX_PDU_LENGTH * 2 + 120)) pending += (char)Serial1.read();
      else Serial1.read();  // 异常残留限长，避免 String 膨胀
      start = millis();     // 字节仍在连续到达时多等一点，尽量收完整 +CMT/PDU 两行
    }
    if (!Serial1.available()) break;
    yield();
  }
  if (pending.length() == 0) return true;

  processSmsUrcText(pending);
  if (smsUrcReceiving()) {
    logCaptureLn(String("清理串口残留时遇到短信接收窗口，暂缓操作: ") + label);
    drainPendingSmsUrc(3000);
    if (smsUrcReceiving()) return false;
  }
  return true;
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  if (!beginModemSerialOp(cmd)) return "";
  static bool preDraining = false;
  if (!preDraining) {
    preDraining = true;
    drainPendingSmsUrc(3000);
    preDraining = false;
  }
  if (smsUrcReceiving()) {
    logCaptureLn(String("短信接收窗口未空闲，暂缓AT命令: ") + cmd);
    endModemSerialOp();
    return "";
  }
  if (!drainSerial1PreservingSms(cmd)) {
    endModemSerialOp();
    return "";
  }
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    if (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        // 读取剩余数据（最多 50ms）
        unsigned long t = millis();
        while (millis() - t < 50) {
          if (Serial1.available()) resp += (char)Serial1.read();
          pumpWebServerDuringWait();  // web 栈内不重入，内部泵也标记HTTP栈，避免嵌套AT抢串口
        }
        endModemSerialOp();
        if (resp.indexOf("OK") >= 0) lastModemOkMs = millis();
        processSmsUrcText(resp);
        return resp;
      }
    }
    pumpWebServerDuringWait();  // web 栈内不重入，内部泵也标记HTTP栈，避免嵌套AT抢串口
  }
  endModemSerialOp();
  processSmsUrcText(resp);
  return resp;
}

static int parseCeregStatValue(const String& resp) {
  int ce = resp.indexOf("+CEREG:");
  if (ce < 0) return -1;
  int colon = resp.indexOf(':', ce);
  if (colon < 0) return -1;
  int comma = resp.indexOf(',', colon + 1);
  int start = (comma >= 0) ? comma + 1 : colon + 1;
  int end = resp.indexOf(',', start);
  int nl = resp.indexOf('\n', start);
  if (end < 0 || (nl >= 0 && nl < end)) end = nl;
  if (end < 0) end = resp.length();
  String stat = resp.substring(start, end);
  stat.trim();
  if (!stat.length()) return -1;
  return stat.toInt();
}

static bool parseCsqResponse(const String& csq) {
  int ci = csq.indexOf("+CSQ:");
  if (ci < 0) return false;
  int colon = csq.indexOf(':', ci);
  int comma = csq.indexOf(',', colon);
  if (colon < 0 || comma <= colon) return false;
  modemCsq = csq.substring(colon + 1, comma).toInt();
  modemBer = csq.substring(comma + 1).toInt();   // 逗号后即 <ber>，toInt 自动忽略尾部 \r\nOK
  modemSignalFresh = true;
  return true;
}

static bool sampleSignalFastNow(unsigned long timeout) {
  String csq = sendATCommand("AT+CSQ", timeout);
  return parseCsqResponse(csq);
}

static bool applyConfiguredDataModeOnce(unsigned long activeTimeout, unsigned long inactiveTimeout) {
  if (config.dataEnabled) {
    if (config.apn.length() > 0)
      sendATandWaitOK(("AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"").c_str(), 3000);
    return sendATandWaitOK("AT+CGACT=1,1", activeTimeout);
  }
  return sendATandWaitOK("AT+CGACT=0,1", inactiveTimeout);
}

static void scheduleDataModeRetry() {
  dataModeRetryPending = true;
  dataModeRetryCount = 0;
  nextDataModeRetryMs = millis() + MODEM_DATA_MODE_RETRY_GAP_MS;
}

// 新增"模组断电重启"函数
void modemPowerCycle() {
  if (!beginModemSerialOp("模组断电重启")) return;
  modemInitPhase = MODEM_INIT_PHASE_POWERING;
  modemReady = false;
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn("EN 拉低：关闭模组");
  digitalWrite(MODEM_EN_PIN, LOW);
  waitWithWebPump(MODEM_POWERDOWN_MS);  // 关机时间给够（按型号可调），同时保持网页可响应

  logCaptureLn("EN 拉高：开启模组");
  digitalWrite(MODEM_EN_PIN, HIGH);
  // 上电探活：最小安定延时后轮询 AT，模组应答即提前结束，最长仍不超过 MODEM_POWERUP_MS。
  // 省每次开机 3-4s（原固定等 6s，阻塞在 WiFi 之前）。
  // 极简上电探针，不复用 sendATCommand：这里已持有串口锁，只需要探测 OK 并持续泵 Web。
  // 过早发 AT 个别 ML307 变体会失败，换模组先调 MODEM_POWERUP_MIN_MS。
  waitWithWebPump(MODEM_POWERUP_MIN_MS);
  unsigned long budget = (MODEM_POWERUP_MS > MODEM_POWERUP_MIN_MS) ? (MODEM_POWERUP_MS - MODEM_POWERUP_MIN_MS) : 0;
  for (unsigned long t0 = millis(); millis() - t0 < budget; ) {
    while (Serial1.available()) Serial1.read();
    Serial1.println("AT");
    String r;
    for (unsigned long s = millis(); millis() - s < 250; ) {
      if (Serial1.available()) r += (char)Serial1.read();
      pumpWebServerDuringWait();
    }
    if (r.indexOf("OK") >= 0) { logCaptureLn("模组上电已应答，提前结束等待"); break; }
  }
  endModemSerialOp();
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  logCaptureLn("正在硬重启模组（EN 断电重启）...");
  modemPowerCycle();
  modemInit();
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
// 所有原"无限 while 重试"改为有限重试，握手彻底失败时断电重启一次再试，
// 仍失败则标记 modemReady=false 退出，绝不永久阻塞（后续由 modemHealthTick 自动重试恢复）。
void modemInit() {
  // 清掉上电噪声/残留
  while (Serial1.available()) Serial1.read();
  modemReady = false;
  modemCeregStat = -1;
  modemSignalFresh = false;
  modemIdentityFresh = false;
  modemRegistrationPending = false;
  registrationCheckCount = 0;
  postRegisterStep = 0;
  dataModeRetryPending = false;
  signalLastFastMs = 0;
  signalLastDetailMs = 0;
  modemInitPhase = MODEM_INIT_PHASE_POWERING;

  bool atOk = false;
  for (int round = 0; round < 2 && !atOk; round++) {
    for (int i = 0; i < MODEM_INIT_AT_RETRIES; i++) {
      if (sendATandWaitOK("AT", 1000)) { atOk = true; break; }
      logCaptureLn("AT未响应，重试...");
      blink_short();
    }
    if (!atOk && round == 0) {
      logCaptureLn("AT多次无响应，断电重启模组后再试...");
      modemPowerCycle();
    }
  }
  if (!atOk) {
    logCaptureLn("模组AT无响应，标记未就绪（健康检查将稍后自动重试）");
    modemReady = false;
    modemInitPhase = MODEM_INIT_PHASE_FAILED;
    return;
  }
  logCaptureLn("模组AT响应正常");
  modemInitPhase = MODEM_INIT_PHASE_AT_READY;
  lastModemOkMs = millis();
  if (sampleSignalFastNow(1200)) {
    signalLastFastMs = millis();
    logCaptureF("首次 CSQ 快速采样: %d\n", modemCsq);
  }

  //判断型号，做一些特定操作
  bool need_set_CGACT = true;
  String resp = sendATCommand("ATI", 2000);
  logCaptureLn(String("ATI响应: " + resp));
  if (resp.indexOf("OK") >= 0) {
    String manufacturer = "未知", model = "未知", version = "未知";
    parseATI(resp, manufacturer, model, version);
    if (manufacturer.length() && manufacturer != "未知") modemMfr = manufacturer;
    if (model.length() && model != "未知") modemModel = model;
    if (version.length() && version != "未知") modemFwVer = version;
    //这个模组这条命令有bug
    if (model == "ML307Y") need_set_CGACT = false;
  }

  if(need_set_CGACT) {
    bool dataModeOk = applyConfiguredDataModeOnce(6000, 2500);
    if (dataModeOk) {
      if (config.dataEnabled) logCaptureLn("已按配置启用蜂窝数据(AT+CGACT=1,1)");
      else logCaptureLn("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗");
    } else {
      logCaptureLn(config.dataEnabled ? "启动时激活数据连接未成功，转入后台重试" :
                                       "启动时禁用数据连接未确认，转入后台重试");
      scheduleDataModeRetry();
    }
  } else {
    logCaptureLn("该型号无法配置(AT+CGACT=0,1)，跳过该命令，会不会消耗流量？自求多福");
  }
  if (sendATandWaitOK("AT+CPMS=\"MT\",\"MT\",\"MT\"", 2000)) {
    logCaptureLn("短信存储已设为 MT(合并存储)");
  } else if (sendATandWaitOK("AT+CPMS=\"SM\",\"SM\",\"SM\"", 2000)) {
    logCaptureLn("短信存储已设为 SM(SIM卡)");
  } else {
    logCaptureLn("短信存储设置失败，沿用模组默认存储");
  }
  // mt=1：短信先落到 SM/MT 存储并发 +CMTI 通知；即使通知被 AT 事务打断，
  // 60s CMGL 兜底轮询也能捞回，避免 mt=2 直推 +CMT 在极端串口竞争下丢正文。
  initSmsModeCommand("AT+CNMI=2,1,0,0,0",
                     "CNMI参数设置完成(存储通知模式)",
                     "设置CNMI失败，快速重试...",
                     "设置CNMI暂未成功，交给接收看门狗后台补设");
  initSmsModeCommand("AT+CMGF=0",
                     "PDU模式设置完成",
                     "设置PDU模式失败，快速重试...",
                     "设置PDU模式暂未成功，交给接收看门狗后台补设");
  requestModemIdentitySample();  // 仅安排一次后台采样；不阻塞注册和开机补短信
  modemInitPhase = MODEM_INIT_PHASE_REGISTERING;
  modemRegistrationPending = true;
  registrationCheckCount = 0;
  nextRegistrationCheckMs = millis();
  logCaptureLn("模组基础初始化完成，网络注册转入后台等待");
}

bool queryModemImei(String& imeiOut, String* rawResp, unsigned long timeout) {
  imeiOut = "";
  if (rawResp) *rawResp = "";
  const char* imeiCmds[] = {"AT+CGSN=1", "AT+GSN=1", "AT+CGSN", "AT+GSN"};
  for (unsigned i = 0; i < sizeof(imeiCmds) / sizeof(imeiCmds[0]); i++) {
    String resp = sendATCommand(imeiCmds[i], timeout);
    if (rawResp) {
      if (rawResp->length() > 0) *rawResp += "\n";
      *rawResp += imeiCmds[i];
      *rawResp += ": ";
      String one = resp;
      one.trim();
      if (one.length() > 180) one = one.substring(0, 180) + "...";
      *rawResp += one;
    }
    String found = extractImei(resp);
    if (found.length() > 0) {
      imeiOut = found;
      return true;
    }
  }
  return false;
}

static bool identitySampleQueued = false;
static bool identitySampleDone = false;
static uint8_t identityStep = 0;
static uint8_t identityImeiCmdIndex = 0;
static unsigned long identityNextStepMs = 0;
static String identityOldImei;
static String identityOldIccid;

static void parseCnumIdentity(const String& n) {
  int c = n.indexOf("+CNUM:");
  if (c < 0) return;
  int q1 = n.indexOf('"', n.indexOf(',', c));  // 第二个字段是号码
  int q2 = (q1 >= 0) ? n.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1 + 1) modemPhone = n.substring(q1 + 1, q2);
}

static bool parseIccidIdentity(const String& ic) {
  int icp = ic.indexOf("+MCCID:");
  if (icp < 0) icp = ic.indexOf("+ICCID:");
  if (icp < 0) return false;
  int st = ic.indexOf(':', icp) + 1;
  String v = ic.substring(st);
  v.replace("\"", "");
  int nl = v.indexOf('\r');
  if (nl < 0) nl = v.indexOf('\n');
  if (nl >= 0) v = v.substring(0, nl);
  v.trim();
  if (v.length() < 15) return false;
  modemIccid = v;
  return true;
}

static void parseImsiIdentity(String im) {
  im.replace("\r", "\n");
  for (int from = 0; from < (int)im.length(); ) {
    int nl = im.indexOf('\n', from);
    String line = (nl < 0) ? im.substring(from) : im.substring(from, nl);
    line.trim();
    bool allDigit = (line.length() >= 14 && line.length() <= 16);
    for (int k = 0; allDigit && k < (int)line.length(); k++) {
      if (line[k] < '0' || line[k] > '9') allDigit = false;
    }
    if (allDigit) { modemImsi = line; return; }
    if (nl < 0) break;
    from = nl + 1;
  }
}

static void parseApnIdentity(const String& cg) {
  int cgp = cg.indexOf("+CGDCONT: 1,");
  if (cgp < 0) return;
  int q1 = cg.indexOf('"', cgp), q2 = cg.indexOf('"', q1 + 1);
  int q3 = cg.indexOf('"', q2 + 1), q4 = cg.indexOf('"', q3 + 1);
  if (q3 >= 0 && q4 > q3 + 1) modemApn = cg.substring(q3 + 1, q4);
}

static void parseAtiIdentity(const String& ati) {
  if (ati.indexOf("OK") < 0) return;
  String mfr = "未知", model = "未知", ver = "未知";
  parseATI(ati, mfr, model, ver);
  if (mfr.length() && mfr != "未知") modemMfr = mfr;
  if (model.length() && model != "未知") modemModel = model;
  if (ver.length() && ver != "未知") modemFwVer = ver;
}

static void sampleOperatorName() {
  String cops = sendATCommand("AT+COPS?", 3000);
  int q1 = cops.indexOf('"');
  int q2 = cops.indexOf('"', q1 + 1);
  if (q1 >= 0 && q2 > q1) modemOperator = cops.substring(q1 + 1, q2);
}

void requestModemIdentitySample() {
  if (identitySampleQueued) return;
  identitySampleDone = false;
  modemIdentityFresh = false;
  identitySampleQueued = true;
  identityStep = 0;
  identityImeiCmdIndex = 0;
  identityNextStepMs = millis() + MODEM_IDENTITY_FIRST_DELAY_MS;
}

// 开机一次性后台采样身份信息，纯本地 AT，不产生蜂窝流量。
// 每次 tick 最多一条 AT，网页轮询不会饿死它，短信/发信/保号忙时它会主动退后。
void modemIdentityTick() {
  if (!identitySampleQueued) return;
  if (!modemReady && lastModemOkMs == 0) return;
  if (smsUrcReceiving() || smsStoredWorkPending()) return;
  if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS) return;
  if (gSlowWorkBusy || forwardQueueDepth() > 0 || emailQueueDepth() > 0 ||
      outgoingSmsQueueDepth() > 0) return;
  if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return;

  unsigned long now = millis();
  if ((int32_t)(now - identityNextStepMs) < 0) return;

  if (identityStep == 0) {
    identityOldImei = modemImei;
    identityOldIccid = modemIccid;
    logCaptureLn("后台一次性补采样模组身份信息...");
    identityStep = 1;
  }

  static const char* imeiCmds[] = {"AT+CGSN=1", "AT+GSN=1", "AT+CGSN", "AT+GSN"};
  if (identityStep == 1) {
    if (modemImei.length() >= 14 || identityImeiCmdIndex >= sizeof(imeiCmds) / sizeof(imeiCmds[0])) {
      identityStep = 2;
    } else {
      String resp = sendATCommand(imeiCmds[identityImeiCmdIndex], 1200);
      String found = extractImei(resp);
      if (found.length() > 0) modemImei = found;
      identityImeiCmdIndex++;
      identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
      return;
    }
  }

  if (identityStep == 2) {
    if (modemPhone.length() == 0) parseCnumIdentity(sendATCommand("AT+CNUM", 1200));
    identityStep = 3;
    identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
    return;
  }

  if (identityStep == 3) {
    bool got = parseIccidIdentity(sendATCommand("AT+MCCID", 1500));
    identityStep = got || modemIccid.length() >= 15 ? 5 : 4;
    identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
    return;
  }

  if (identityStep == 4) {
    parseIccidIdentity(sendATCommand("AT+ICCID", 1500));
    identityStep = 5;
    identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
    return;
  }

  if (identityStep == 5) {
    if (modemImsi.length() == 0) parseImsiIdentity(sendATCommand("AT+CIMI", 1500));
    identityStep = 6;
    identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
    return;
  }

  if (identityStep == 6) {
    if (modemApn.length() == 0) parseApnIdentity(sendATCommand("AT+CGDCONT?", 1500));
    identityStep = 7;
    identityNextStepMs = millis() + MODEM_IDENTITY_STEP_GAP_MS;
    return;
  }

  if (identityStep == 7) {
    if (modemModel.length() == 0 || modemFwVer.length() == 0) parseAtiIdentity(sendATCommand("ATI", 1500));
    if (modemImei != identityOldImei || modemIccid != identityOldIccid) saveModemIdentityCache();
    identitySampleQueued = false;
    identitySampleDone = true;
    modemIdentityFresh = true;
    logCaptureLn("模组身份信息后台采样完成");
  }
}

void saveModemIdentityCache() {
  if (modemImei.length() == 0 && modemIccid.length() == 0) return;
  preferences.begin("sms_config", false);
  if (modemImei.length() >= 14) preferences.putString("modemImei", modemImei);
  if (modemIccid.length() >= 15) preferences.putString("modemIccid", modemIccid);
  preferences.end();
  logCaptureLn("模组身份信息已写入缓存");
}

// 读取蜂窝 PDP 地址(仅 dataEnabled 时有意义)。不带 cid 返回所有已定义上下文，取首个有效 IP。
void sampleCellIp() {
  String r = sendATCommand("AT+CGPADDR", 3000);  // +CGPADDR: <cid>,"10.x.x.x"
  modemCellIp = "";
  int c = r.indexOf("+CGPADDR:");
  while (c >= 0) {
    int comma = r.indexOf(',', c);
    int eol = r.indexOf('\n', c); if (eol < 0) eol = r.length();
    if (comma >= 0 && comma < eol) {
      String ip = r.substring(comma + 1, eol);
      ip.replace("\"", ""); ip.trim();
      if (ip.length() >= 7 && ip != "0.0.0.0") { modemCellIp = ip; break; }
    }
    c = r.indexOf("+CGPADDR:", eol);
  }
}

static bool parseMuestatsCell(const String& r) {
  int line = r.indexOf("\"scell\"");
  if (line < 0) return false;
  int eol = r.indexOf('\n', line); if (eol < 0) eol = r.length();
  String ln = r.substring(line, eol);   // "scell",4,460,00,...
  String parts[12]; int np = 0, from = 0;
  while (np < 12) {
    int comma = ln.indexOf(',', from);
    if (comma < 0) { parts[np++] = ln.substring(from); break; }
    parts[np++] = ln.substring(from, comma);
    from = comma + 1;
  }
  // parts[0]="scell" parts[1]=rat parts[2]=mcc parts[3]=mnc parts[6]=pci parts[7]=rsrp parts[8]=rsrq parts[10]=sinr
  if (np <= 10) return false;
  long mcc = parts[2].toInt(), mnc = parts[3].toInt();
  long pci = parts[6].toInt();
  String prsrp = parts[7]; prsrp.trim();
  String prsrq = parts[8]; prsrq.trim();
  String psinr = parts[10]; psinr.trim();
  if (parts[2].length() && parts[3].length()) {
    char b[12]; snprintf(b, sizeof(b), "%ld%02ld", mcc, mnc); modemPlmn = b;
  }
  if (parts[6].length() && pci >= 0 && pci != 65535) modemPci = (int)pci;
  if (prsrp.length()) { long v = prsrp.toInt(); if (v > -32768) modemRsrp = (int)(v / 10); }
  if (prsrq.length()) { long v = prsrq.toInt(); if (v > -32768) modemRsrq = (int)(v / 10); }
  if (psinr.length()) { long v = psinr.toInt(); if (v > -32768) modemSinr = (int)(v / 10); }
  return true;
}

static void parseCesqSignal(const String& ceResp) {
  int cei = ceResp.indexOf("+CESQ:");
  if (cei < 0) return;
  int e = ceResp.indexOf('\n', cei); if (e < 0) e = ceResp.length();
  String l = ceResp.substring(cei, e);
  int lc = l.lastIndexOf(',');
  int pc = l.lastIndexOf(',', lc - 1);
  if (lc >= 0) { int idx = l.substring(lc + 1).toInt(); if (idx >= 0 && idx <= 97) modemRsrp = idx - 141; }
  if (pc >= 0 && lc > pc) { int idx = l.substring(pc + 1, lc).toInt(); if (idx >= 0 && idx <= 34) modemRsrq = idx / 2 - 20; }
}

static void parseCeregTac(const String& c) {
  int ce = c.indexOf("+CEREG:");
  if (ce < 0) return;
  int q1 = c.indexOf('"', ce);
  int q2 = (q1 >= 0) ? c.indexOf('"', q1 + 1) : -1;
  if (q1 >= 0 && q2 > q1 + 1) modemTac = c.substring(q1 + 1, q2);
}

// 详细服务小区信息：ML307 AT+MUESTATS="cell"。RSRP/RSRQ/RSSI/SINR 为 0.1 单位需 /10。
// scell 行: +MUESTATS: "scell",<rat>,<mcc>,<mnc>,<earfcn>,<offset>,<pci>,<rsrp>,<rsrq>,<rssi>,<sinr>[,<bw>]
// TAC 不在此行 → 从 AT+CEREG? 的十六进制 <tac> 字段取。-32768 为无效哨兵。
void sampleSignalDetail() {
  String r = sendATCommand("AT+MUESTATS=\"cell\"", 2000);
  if (!parseMuestatsCell(r)) {
    // 回退：标准 AT+CESQ 取 RSRP/RSRQ 索引并换算(无 SINR/PCI)
    parseCesqSignal(sendATCommand("AT+CESQ", 2000));
  }
  // TAC：+CEREG: <n>,<stat>,"<tac>","<ci>",<AcT>
  parseCeregTac(sendATCommand("AT+CEREG?", 2000));
}

// 信号采样调度(与 SMS 接收轮询解耦，避免一次 watchdog tick 串行堆叠多条 AT 造成 ~9s 长阻塞)：
// CSQ 信号条高频采样(快, 首页数值跟手)；RSRP/SINR/PCI 等详细指标变化慢，低频采样省阻塞。
void signalSampleTick() {
  if (!modemReady && modemInitPhase != MODEM_INIT_PHASE_AT_READY &&
      modemInitPhase != MODEM_INIT_PHASE_REGISTERING) return;
  if (smsRecvGuardUntil && millis() < smsRecvGuardUntil) return;  // 短信接收窗口内不采样，避免AT清缓冲冲掉URC
  if (smsStoredWorkPending()) return;
  static unsigned long nextDetailStepMs = 0;
  static uint8_t detailStep = 0;  // 0=空闲, 1=MUESTATS, 2=CESQ回退, 3=CEREG/TAC
  static bool detailNeedCesq = false;
  unsigned long now = millis();
  if (lastWebRequestMs != 0 && now - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS) return;
  if (gSlowWorkBusy || forwardQueueDepth() > 0 || emailQueueDepth() > 0 ||
      outgoingSmsQueueDepth() > 0) return;
  if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return;
  bool fastDue = (signalLastFastMs == 0) ? (now >= SIGNAL_FAST_FIRST_DELAY_MS) : (now - signalLastFastMs >= SIGNAL_FAST_INTERVAL_MS);
  if (detailStep == 0 && fastDue) {
    signalLastFastMs = now;
    sampleSignalFastNow(1500);
    return;  // 每帧最多一条 AT，避免信号采样把网页排队时间拉长
  }
  if (!modemReady) return;  // 注册完成前只做 CSQ，详细小区信息等 CEREG 成功后再采
  bool detailDue = (signalLastDetailMs == 0) ? (now >= SIGNAL_DETAIL_FIRST_DELAY_MS) : (now - signalLastDetailMs >= SIGNAL_DETAIL_INTERVAL_MS);
  if (detailStep == 0 && detailDue) {
    detailStep = 1;
    detailNeedCesq = false;
    nextDetailStepMs = now;
  }
  if (detailStep != 0 && (int32_t)(now - nextDetailStepMs) >= 0) {
    if (detailStep == 1) {
      String r = sendATCommand("AT+MUESTATS=\"cell\"", 2000);
      detailNeedCesq = !parseMuestatsCell(r);
      detailStep = detailNeedCesq ? 2 : 3;
    } else if (detailStep == 2) {
      parseCesqSignal(sendATCommand("AT+CESQ", 2000));
      detailStep = 3;
    } else {
      parseCeregTac(sendATCommand("AT+CEREG?", 2000));
      signalLastDetailMs = millis();
      detailStep = 0;
    }
    nextDetailStepMs = millis() + SIGNAL_DETAIL_STEP_GAP_MS;
  }
}

// 应用运营商选择(AT+COPS)。空=自动注册(COPS=0)；否则锁定 PLMN(COPS=1,2,"<plmn>")。
// 注意：只能选 SIM 允许接入的网络；锁定不可达 PLMN 会失网。COPS 可能耗时较长(最长 ~30s)。
void modemApplyOperator() {
  if (!modemReady) return;
  if (config.operatorPlmn.length() == 0) {
    sendATandWaitOK("AT+COPS=0", 30000);
    logCaptureLn("运营商: 自动注册(COPS=0)");
  } else {
    String cmd = "AT+COPS=1,2,\"" + config.operatorPlmn + "\"";
    bool ok = sendATandWaitOK(cmd.c_str(), 30000);
    logCaptureLn(String("运营商: 锁定 PLMN " + config.operatorPlmn + (ok ? " 成功" : " 失败(可能不可达)")));
  }
}

// 按当前配置应用蜂窝数据模式(供 SIM 页保存后即时生效，无需重启)。
// 单线程模型下与 checkSerial1URC 不会并发，缓冲中的 URC 下个 loop 仍会被读取。
void modemApplyDataMode() {
  if (!modemReady) return;
  if (config.dataEnabled) {
    if (config.apn.length() > 0)
      sendATandWaitOK(("AT+CGDCONT=1,\"IP\",\"" + config.apn + "\"").c_str(), 3000);
    sendATandWaitOK("AT+CGACT=1,1", 10000);
    sampleCellIp();
    logCaptureLn(String("蜂窝数据已启用(APN=" + (config.apn.length() ? config.apn : String("自动")) +
                        ", IP=" + (modemCellIp.length() ? modemCellIp : String("获取中")) + ")"));
  } else {
    sendATandWaitOK("AT+CGACT=0,1", 5000);
    modemCellIp = "";
    logCaptureLn("蜂窝数据已禁用(零流量)");
  }
}

static bool processDataModeRetry() {
  if (!dataModeRetryPending) return false;
  if ((int32_t)(millis() - nextDataModeRetryMs) < 0) return false;
  if (smsUrcReceiving() || smsStoredWorkPending()) return false;
  if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return false;

  dataModeRetryCount++;
  bool ok = applyConfiguredDataModeOnce(8000, 3000);
  if (ok) {
    dataModeRetryPending = false;
    logCaptureLn(config.dataEnabled ? "后台重试：蜂窝数据已启用" : "后台重试：蜂窝数据已禁用");
  } else if (dataModeRetryCount >= MODEM_DATA_MODE_RETRY_MAX) {
    dataModeRetryPending = false;
    logCaptureLn("后台重试 CGACT 仍失败，保留当前模组状态，后续健康检查继续兜底");
  } else {
    nextDataModeRetryMs = millis() + MODEM_DATA_MODE_RETRY_GAP_MS;
    logCaptureLn("后台重试 CGACT 未成功，稍后再试");
  }
  return true;
}

static void noteNetworkRegistered() {
  bool firstReady = !modemReady || modemInitPhase != MODEM_INIT_PHASE_READY;
  if (firstReady) logCaptureLn("网络已注册");
  modemReady = true;
  modemInitPhase = MODEM_INIT_PHASE_READY;
  modemRegistrationPending = false;
  registrationCheckCount = 0;
  lastModemOkMs = millis();
  if (firstReady) postRegisterStep = 1;
}

// 开机注册后台状态机：把原 setup() 内最长几十秒的 CEREG 等待拆到 loop 里。
// 每次 tick 最多执行一个 AT 类动作，注册完成后再依次做运营商/IP/暂存短信补收。
void modemRegistrationTick() {
  if (processDataModeRetry()) return;

  if (postRegisterStep != 0) {
    if (smsUrcReceiving() || smsStoredWorkPending()) return;
    if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS) return;
    if (gSlowWorkBusy || forwardQueueDepth() > 0 || emailQueueDepth() > 0 ||
        outgoingSmsQueueDepth() > 0) return;
    if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return;

    if (postRegisterStep == 1) {
      if (config.operatorPlmn.length()) modemApplyOperator();  // 仅在用户显式锁定运营商时下发
      postRegisterStep = 2;
      return;
    }
    if (postRegisterStep == 2) {
      sampleOperatorName();
      postRegisterStep = 3;
      return;
    }
    if (postRegisterStep == 3) {
      if (config.dataEnabled) sampleCellIp();
      postRegisterStep = 4;
      return;
    }
    if (postRegisterStep == 4) {
      backfillStoredSms(true);
      postRegisterStep = 0;
      return;
    }
  }

  if (!modemRegistrationPending) return;
  if (smsUrcReceiving()) return;
  if ((int32_t)(millis() - nextRegistrationCheckMs) < 0) return;
  if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return;

  if (waitCEREG()) {
    noteNetworkRegistered();
    return;
  }

  registrationCheckCount++;
  if (registrationCheckCount >= MODEM_REG_MAX_CHECKS) {
    modemRegistrationPending = false;
    modemInitPhase = MODEM_INIT_PHASE_FAILED;
    modemReady = false;
    logCaptureLn("网络注册后台等待超时（无SIM卡或信号差），后续健康检查继续恢复");
  } else {
    nextRegistrationCheckMs = millis() + MODEM_REG_CHECK_INTERVAL_MS;
    logCaptureLn("等待网络注册...");
  }
}

// 模组健康探测：loop() 周期调用。仅在"距上次确认存活已超过探测周期"时才主动发
// AT+CEREG? —— 任何收到的串口行(含短信URC)都会刷新 lastModemOkMs(见 checkSerial1URC)，
// 因此有真实短信流量或近期 AT 操作时不会主动探测，最大限度降低探测 AT 抢占到达短信 URC 的概率。
// 连续失败达阈值则自动断电重启并重新初始化恢复。探测仅走本地 AT，不产生流量/资费。
void modemHealthTick() {
  static int failCount = 0;
  static bool inTick = false;
  if (inTick) return;                                   // 防重入（恢复期间 modemInit 会 pump handleClient）
  if (modemInitPhase == MODEM_INIT_PHASE_POWERING ||
      modemInitPhase == MODEM_INIT_PHASE_AT_READY ||
      modemInitPhase == MODEM_INIT_PHASE_REGISTERING) return;  // 注册中由 modemRegistrationTick 负责
  if (millis() - lastModemOkMs < MODEM_HEALTH_INTERVAL_MS) return;
  if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < SLOW_WORK_WEB_GRACE_MS) return;
  if (!modemAtQuietFor(MODEM_BACKGROUND_AT_GAP_MS)) return;
  inTick = true;

  if (waitCEREG()) {
    noteNetworkRegistered();
    failCount = 0;
  } else {
    failCount++;
    logCaptureF("模组健康探测失败 %d/%d\n", failCount, MODEM_HEALTH_FAIL_LIMIT);
    if (failCount >= MODEM_HEALTH_FAIL_LIMIT) {
      logCaptureLn("模组连续无响应，自动断电重启恢复...");
      modemReady = false;
      modemPowerCycle();
      modemInit();
      failCount = 0;
    }
    lastModemOkMs = millis();                            // 失败也刷新，避免下一帧立即重复探测
  }
  inTick = false;
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  waitWithWebPump(50);
  digitalWrite(LED_BUILTIN, HIGH);
  waitWithWebPump(gap_time);
}

// 发 AT 并判 OK：复用 sendATCommand 的读循环(含防重入泵/yield)，避免重复读逻辑
bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  return sendATCommand(cmd, timeout).indexOf("OK") >= 0;
}

// 检测网络注册状态（LTE/4G）。CEREG状态: 1=已注册本地, 5=已注册漫游，其余(0/2/3/4)视为未注册
bool waitCEREG() {
  String resp = sendATCommand("AT+CEREG?", 2000);
  modemCeregStat = parseCeregStatValue(resp);
  if (modemCeregStat < 0) return false;
  return modemCeregStat == 1 || modemCeregStat == 5;
}

// 发送短信（PDU模式）
static bool sendSMSImpl(const char* phoneNumber, const char* message) {
  logCaptureLn("准备发送短信...");
  logCapture("目标号码: "); logCaptureLn(maskPhone(String(phoneNumber)));
  logCapture("短信内容: "); logCaptureLn(bodyPreview(String(message), SMS_LOG_VERBOSE));

  if (!beginModemSerialOp("发送短信")) return false;
  drainPendingSmsUrc(3000);
  if (smsUrcReceiving()) {
    logCaptureLn("短信接收窗口未空闲，取消本次短信发送以避免冲掉接收PDU");
    endModemSerialOp();
    return false;
  }

  // 使用pdulib编码PDU；pdu 是全局对象，必须在串口锁内使用，防止嵌套发送覆盖缓冲。
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    logCapture("PDU编码失败，错误码: ");
    logCaptureLn(String(pduLen));
    endModemSerialOp();
    return false;
  }
  
#if SMS_LOG_VERBOSE
  logCapture("PDU数据: "); logCaptureLn(String(pdu.getSMS()));
#endif
  logCapture("PDU长度: "); logCaptureLn(String(pduLen));
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;

  if (!drainSerial1PreservingSms("发送短信")) {
    endModemSerialOp();
    return false;
  }
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
#if SMS_LOG_VERBOSE
      logCapture(String(c));
#endif
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
    pumpWebServerDuringWait();  // 发送等待期间允许网页轻量响应，但禁止嵌套AT抢串口
  }
  
  if (!gotPrompt) {
    logCaptureLn("未收到>提示符");
    endModemSerialOp();
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
#if SMS_LOG_VERBOSE
      logCapture(String(c));
#endif
      if (resp.indexOf("OK") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        logCaptureLn("短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        endModemSerialOp();
        processSmsUrcText(resp);
        logCaptureLn("短信发送失败");
        return false;
      }
    }
    pumpWebServerDuringWait();  // 发送等待期间允许网页轻量响应，但禁止嵌套AT抢串口
    yield();  // 长等待中让出 CPU，喂看门狗
  }
  endModemSerialOp();
  processSmsUrcText(resp);
  logCaptureLn("短信发送超时");
  return false;
}

// 公开入口：统一记录到“已发送”环形缓冲(供网页查看)，覆盖网页发送/管理员指令/保号短信
bool sendSMS(const char* phoneNumber, const char* message) {
  bool ok = sendSMSImpl(phoneNumber, message);
  sentAdd(phoneNumber, message, ok);
  return ok;
}

static char hexNibble(uint8_t v) {
  v &= 0x0F;
  return v < 10 ? (char)('0' + v) : (char)('A' + v - 10);
}

static String hexEncodeAscii(const String& s) {
  String out;
  out.reserve(s.length() * 2);
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t b = (uint8_t)s.charAt(i);
    out += hexNibble(b >> 4);
    out += hexNibble(b);
  }
  return out;
}

static bool parseHttpUrl(const String& rawUrl, String& protocol, String& host, String& path) {
  String url = rawUrl;
  url.trim();
  if (url.length() == 0) url = CELLULAR_KEEPALIVE_DEFAULT_URL;
  if (url.length() > 240) {
    logCaptureLn("蜂窝HTTP URL过长");
    return false;
  }
  int protoEnd = url.indexOf("://");
  if (protoEnd <= 0) {
    logCaptureLn("蜂窝HTTP URL格式无效，需要 http:// 或 https://");
    return false;
  }
  protocol = url.substring(0, protoEnd);
  protocol.toLowerCase();
  if (protocol != "http" && protocol != "https") {
    logCaptureLn("蜂窝HTTP URL仅支持 http/https");
    return false;
  }

  int hostStart = protoEnd + 3;
  int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) {
    host = url.substring(hostStart);
    path = "/";
  } else {
    host = url.substring(hostStart, pathStart);
    path = url.substring(pathStart);
  }
  int hash = path.indexOf('#');
  if (hash >= 0) path.remove(hash);
  host.trim();
  if (host.length() == 0 || host.indexOf('"') >= 0 || host.indexOf(' ') >= 0 ||
      path.indexOf('"') >= 0 || path.indexOf(' ') >= 0) {
    logCaptureLn("蜂窝HTTP URL包含非法字符");
    return false;
  }
  return true;
}

static String appendNoCacheQuery(String path) {
  path += (path.indexOf('?') >= 0) ? '&' : '?';
  path += "t=";
  path += String((unsigned long)millis());
  path += "&r=";
  path += String((uint32_t)esp_random(), HEX);
  return path;
}

static void normalizeKeepAlivePayloadSize(String& host, String& path) {
  if (host != "gg.incrafttime.top") return;
  if (!path.startsWith("/api/payload?")) return;
  path.replace("size=128684", "size=64342");
}

static String sendATCommandBusy(const String& cmd, unsigned long timeout, unsigned long extraReadMs = 50) {
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp;
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (resp.length() < 900) resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        unsigned long t = millis();
        while (millis() - t < extraReadMs) {
          while (Serial1.available()) {
            char e = Serial1.read();
            if (resp.length() < 1200) resp += e;
            t = millis();
          }
          pumpWebServerDuringWait();
        }
        processSmsUrcText(resp);
        return resp;
      }
    }
    pumpWebServerDuringWait();
  }
  processSmsUrcText(resp);
  return resp;
}

static int parseMhttpCreateId(const String& resp) {
  int p = resp.indexOf("+MHTTPCREATE:");
  if (p < 0) return -1;
  p += 13;
  while (p < (int)resp.length() && !isDigit((unsigned char)resp.charAt(p)) && resp.charAt(p) != '-') p++;
  if (p >= (int)resp.length()) return -1;
  return resp.substring(p).toInt();
}

static bool sendMhttpHeaderBusy(int httpId, bool more, const String& line) {
  String cmd = String("AT+MHTTPHEADER=") + String(httpId) + "," + (more ? "1" : "0") +
               "," + String(line.length()) + ",\"" + line + "\"";
  String resp = sendATCommandBusy(cmd, 3000, 50);
  return resp.indexOf("OK") >= 0;
}

static bool parseCommaLongs(const String& s, long* values, int maxValues, int& count) {
  count = 0;
  int start = 0;
  while (start < (int)s.length() && count < maxValues) {
    int comma = s.indexOf(',', start);
    String part = (comma < 0) ? s.substring(start) : s.substring(start, comma);
    part.trim();
    if (part.length() > 0 && (isDigit((unsigned char)part.charAt(0)) || part.charAt(0) == '-')) {
      values[count++] = part.toInt();
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return count > 0;
}

static void parseMhttpHead(const String& head, int httpId, unsigned long& bytesRead,
                           unsigned long& expectedBytes, int& statusCode,
                           int& mhttpError, bool& complete, bool& error) {
  if (head.startsWith("+MHTTPURC: \"header\"")) {
    long nums[4]; int n = 0;
    if (parseCommaLongs(head.substring(head.indexOf(',') + 1), nums, 4, n) && n >= 2 &&
        nums[0] == httpId) {
      statusCode = (int)nums[1];
      logCaptureF("蜂窝HTTP响应状态: %d\n", statusCode);
    }
  } else if (head.startsWith("+MHTTPURC: \"content\"")) {
    long nums[5]; int n = 0;
    if (parseCommaLongs(head.substring(head.indexOf(',') + 1), nums, 5, n) && n >= 4 &&
        nums[0] == httpId) {
      expectedBytes = (unsigned long)nums[1];
      bytesRead = (unsigned long)nums[2];
      unsigned long cur = (unsigned long)nums[3];
      if ((expectedBytes > 0 && bytesRead >= expectedBytes) || cur == 0) complete = true;
    }
  } else if (head.startsWith("+MHTTPURC: \"err\"")) {
    long nums[3]; int n = 0;
    if (parseCommaLongs(head.substring(head.indexOf(',') + 1), nums, 3, n) && n >= 2 &&
        nums[0] == httpId) {
      mhttpError = (int)nums[1];
      logCaptureF("蜂窝HTTP错误码: %ld%s\n", nums[1],
                  mhttpError == 4 ? "(SSL握手失败)" : "");
      error = true;
      complete = true;
    }
  }
}

bool waitCellularPdpReady(unsigned long timeout) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    String r = sendATCommand("AT+CGPADDR=1", 3000);
    int c = r.indexOf("+CGPADDR:");
    if (c >= 0) {
      int comma = r.indexOf(',', c);
      int eol = r.indexOf('\n', c); if (eol < 0) eol = r.length();
      if (comma >= 0 && comma < eol) {
        String ip = r.substring(comma + 1, eol);
        ip.replace("\"", "");
        ip.trim();
        if (ip.length() >= 7 && ip != "0.0.0.0") {
          modemCellIp = ip;
          logCaptureLn(String("蜂窝PDP已就绪，IP: ") + ip);
          return true;
        }
      }
    }
    unsigned long gap = millis();
    while (millis() - gap < 700) pumpWebServerDuringWait();
  }
  logCaptureLn("蜂窝PDP等待超时：未取得有效IP");
  return false;
}

static bool waitMhttpDownload(int httpId, unsigned long timeout, unsigned long& bytesRead,
                              unsigned long& expectedBytes, int& statusCode, int& mhttpError) {
  unsigned long start = millis();
  String head;
  head.reserve(180);
  bool skippingData = false;
  bool complete = false;
  bool error = false;
  while (millis() - start < timeout && !complete) {
    while (Serial1.available() && !complete) {
      char c = Serial1.read();
      if (skippingData) {
        if (c == '\n') {
          skippingData = false;
          head = "";
        }
        continue;
      }
      if (c == '\r' || c == '\n') {
        if (head.startsWith("+MHTTPURC: \"err\"")) {
          parseMhttpHead(head, httpId, bytesRead, expectedBytes, statusCode, mhttpError, complete, error);
        }
        head = "";
        continue;
      }
      if (head.length() < 260) head += c;

      int needCommas = 0;
      if (head.startsWith("+MHTTPURC: \"content\"")) needCommas = 5;
      else if (head.startsWith("+MHTTPURC: \"header\"")) needCommas = 4;
      if (needCommas > 0) {
        int commas = 0;
        for (unsigned i = 0; i < head.length(); i++) if (head.charAt(i) == ',') commas++;
        if (commas >= needCommas) {
          parseMhttpHead(head, httpId, bytesRead, expectedBytes, statusCode, mhttpError, complete, error);
          skippingData = true;  // 后面是 header/content 的 HEX 数据，直接排空，避免占堆
        }
      }
    }
    pumpWebServerDuringWait();
  }
  if (!complete) logCaptureLn("蜂窝HTTP下载等待超时");
  return !error && complete && statusCode >= 200 && statusCode < 400 &&
         bytesRead >= CELLULAR_KEEPALIVE_MIN_BYTES;
}

static bool fetchCellularKeepAliveParsed(const String& protocol, const String& host, const String& path,
                                         unsigned long* bytesRead, int* httpStatus, int* mhttpError) {
  if (bytesRead) *bytesRead = 0;
  if (httpStatus) *httpStatus = -1;
  if (mhttpError) *mhttpError = 0;
  logCaptureF("准备通过蜂窝HTTP下载保号payload: %s://%s%s\n",
              protocol.c_str(), host.c_str(), path.c_str());

  if (!beginModemSerialOp("蜂窝HTTP保号")) return false;
  if (!drainSerial1PreservingSms("蜂窝HTTP保号")) {
    endModemSerialOp();
    return false;
  }

  for (int i = 0; i < 4; i++) sendATCommandBusy(String("AT+MHTTPDEL=") + String(i), 1000, 10);

  String createCmd = String("AT+MHTTPCREATE=\"") + protocol + "://" + host + "\"";
  String createResp = sendATCommandBusy(createCmd, 10000, 1200);
  int httpId = parseMhttpCreateId(createResp);
  if (httpId < 0) {
    logCaptureLn(String("蜂窝HTTP创建失败: ") + createResp);
    endModemSerialOp();
    return false;
  }

  if (protocol == "https") {
    sendATCommandBusy(String("AT+MHTTPCFG=\"ssl\",") + String(httpId) + ",1,0", 5000, 50);
  }
  sendATCommandBusy(String("AT+MHTTPCFG=\"encoding\",") + String(httpId) + ",0,0", 3000, 50);
  sendMhttpHeaderBusy(httpId, true, "Cache-Control: no-cache, no-store, must-revalidate");
  sendMhttpHeaderBusy(httpId, false, "Pragma: no-cache");
  sendATCommandBusy(String("AT+MHTTPCFG=\"encoding\",") + String(httpId) + ",1,1", 3000, 50);

  String reqCmd = String("AT+MHTTPREQUEST=") + String(httpId) + ",1,0," + hexEncodeAscii(path);
  String reqResp = sendATCommandBusy(reqCmd, 10000, 50);
  if (reqResp.indexOf("OK") < 0) {
    logCaptureLn(String("蜂窝HTTP请求发送失败: ") + reqResp);
    sendATCommandBusy(String("AT+MHTTPDEL=") + String(httpId), 2000, 20);
    endModemSerialOp();
    return false;
  }

  unsigned long got = 0, expected = 0;
  int status = -1;
  int err = 0;
  bool ok = waitMhttpDownload(httpId, CELLULAR_HTTP_TIMEOUT_MS, got, expected, status, err);
  sendATCommandBusy(String("AT+MHTTPDEL=") + String(httpId), 3000, 20);
  endModemSerialOp();

  if (bytesRead) *bytesRead = got;
  if (httpStatus) *httpStatus = status;
  if (mhttpError) *mhttpError = err;
  if (ok) {
    logCaptureF("蜂窝HTTP保号完成: HTTP %d, 已下载约 %luKB\n",
                status, (unsigned long)(got / 1024UL));
  } else {
    logCaptureF("蜂窝HTTP保号失败: HTTP %d, 已下载约 %luKB/期望%luKB\n",
                status, (unsigned long)(got / 1024UL), (unsigned long)(expected / 1024UL));
  }
  return ok;
}

bool fetchCellularKeepAliveUrl(const String& url, unsigned long* bytesRead, int* httpStatus) {
  if (bytesRead) *bytesRead = 0;
  if (httpStatus) *httpStatus = -1;

  String protocol, host, path;
  if (!parseHttpUrl(url, protocol, host, path)) return false;
  normalizeKeepAlivePayloadSize(host, path);
  path = appendNoCacheQuery(path);

  int mhttpError = 0;
  bool ok = fetchCellularKeepAliveParsed(protocol, host, path, bytesRead, httpStatus, &mhttpError);
  if (!ok && protocol == "https" && mhttpError == 4) {
    logCaptureLn("HTTPS握手失败，改用HTTP重试一次；若返回301，请在ESA关闭强制HTTPS跳转");
    ok = fetchCellularKeepAliveParsed("http", host, path, bytesRead, httpStatus, &mhttpError);
  }
  return ok;
}

// ---- 网页端待发短信队列 ----
// HTTP 请求只入队并立即返回；真正发送在 loop() 中执行，避免浏览器长等 AT+CMGS 导致超时/崩溃。
struct OutgoingSmsItem {
  String phone;
  String text;
  unsigned long queuedMs;  // 入队时刻：用于限制网页避让(grace)对本条的最长拖延，防被持续轮询饿死
};

static OutgoingSmsItem outSmsQueue[OUT_SMS_QUEUE_MAX];
static int outSmsHead = 0;
static int outSmsCount = 0;

static void clearOutgoingSmsSlot(int i) {
  outSmsQueue[i].phone = "";
  outSmsQueue[i].text = "";
}

int outgoingSmsQueueDepth() {
  return outSmsCount;
}

bool enqueueOutgoingSms(const char* phoneNumber, const char* message) {
  if (outSmsCount >= OUT_SMS_QUEUE_MAX) {
    logCaptureLn("网页待发短信队列已满，拒绝新的发送请求");
    return false;
  }
  int tail = (outSmsHead + outSmsCount) % OUT_SMS_QUEUE_MAX;
  outSmsQueue[tail].phone = phoneNumber;
  outSmsQueue[tail].text = message;
  outSmsQueue[tail].queuedMs = millis();
  outSmsCount++;
  logCaptureF("网页短信已入队，当前待发=%d\n", outSmsCount);
  return true;
}

void processOutgoingSmsQueue() {
  if (outSmsCount == 0) return;
  if (!modemReady) return;       // 模组未就绪时保留队列，待健康检查恢复后再发
  if (smsUrcReceiving()) return; // 正在接收短信时避让，防止冲掉 PDU
  if (smsStoredWorkPending()) return;
  // 网页刚活跃则避让模组AT，但有上限：避让超过 SLOW_WORK_MAX_DEFER_MS 仍强制发出，防 SPA 持续轮询饿死本条。
  if (lastWebRequestMs != 0 && millis() - lastWebRequestMs < OUT_SMS_WEB_GRACE_MS &&
      millis() - outSmsQueue[outSmsHead].queuedMs < OUT_SMS_MAX_DEFER_MS) return;
  if (!modemAtQuietFor(MODEM_FOREGROUND_AT_GAP_MS)) return;

  OutgoingSmsItem it = outSmsQueue[outSmsHead];
  clearOutgoingSmsSlot(outSmsHead);
  outSmsHead = (outSmsHead + 1) % OUT_SMS_QUEUE_MAX;
  outSmsCount--;

  logCaptureLn(String("开始处理网页待发短信，目标: ") + maskPhone(it.phone));
  sendSMS(it.phone.c_str(), it.text.c_str());
}
