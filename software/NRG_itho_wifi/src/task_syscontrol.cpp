

#include "task_syscontrol.h"

#define TASK_SYS_CONTROL_PRIO 5

// globals
uint32_t TaskSysControlHWmark = 0;
DNSServer dnsServer;
bool SHT3x_original = false;
bool SHT3x_alternative = false;
bool runscan = false;
bool dontSaveConfig = false;
bool saveSystemConfigflag = false;
bool saveWifiConfigflag = false;
bool resetWifiConfigflag = false;
bool resetSystemConfigflag = false;
bool clearQueue = false;
bool shouldReboot = false;
int8_t ithoInitResult = 0;
bool IthoInit = false;

// locals
StaticTask_t xTaskSysControlBuffer;
StackType_t xTaskSysControlStack[STACK_SIZE];
TaskHandle_t xTaskSysControlHandle = NULL;
SemaphoreHandle_t mutexI2Ctask;
unsigned long lastI2CinitRequest = 0;
bool ithoInitResultLogEntry = true;
Ticker scan;
Ticker getSettingsHack;
unsigned long lastWIFIReconnectAttempt = 0;
unsigned long APmodeTimeout = 0;
unsigned long wifiLedUpdate = 0;
unsigned long SHT3x_readout = 0;
unsigned long query2401tim = 0;
unsigned long mqttUpdatetim = 0;
unsigned long lastVersionCheck = 0;
bool wifiModeAP = false;
bool i2cStartCommands = false;
bool joinSend = false;

SHTSensor sht_org(SHTSensor::SHT3X);
SHTSensor sht_alt(SHTSensor::SHT3X_ALT);

void startTaskSysControl()
{
  xTaskSysControlHandle = xTaskCreateStaticPinnedToCore(
      TaskSysControl,
      "TaskSysControl",
      STACK_SIZE,
      (void *)1,
      TASK_SYS_CONTROL_PRIO,
      xTaskSysControlStack,
      &xTaskSysControlBuffer,
      CONFIG_ARDUINO_RUNNING_CORE);
}

void TaskSysControl(void *pvParameters)
{
  configASSERT((uint32_t)pvParameters == 1UL);
  Ticker TaskTimeout;
  Ticker queueUpdater;

  mutexI2Ctask = xSemaphoreCreateMutex();

  wifiInit();

  init_vRemote();

  initSensor();

  startTaskCC1101();

  esp_task_wdt_add(NULL);

  queueUpdater.attach_ms(QUEUE_UPDATE_MS, update_queue);

  for (;;)
  {
    yield();
    esp_task_wdt_reset();

    TaskTimeout.once_ms(35000, []()
                        { logInput("Warning: Task SysControl timed out!"); });

    execSystemControlTasks();

    if (shouldReboot)
    {
      TaskTimeout.detach();
      logInput("Reboot requested");
      if (!dontSaveConfig)
      {
        saveSystemConfig();
      }
      delay(1000);
      ACTIVE_FS.end();
      esp_task_wdt_init(1, true);
      esp_task_wdt_add(NULL);
      while (true)
        ;
    }

    TaskSysControlHWmark = uxTaskGetStackHighWaterMark(NULL);
    vTaskDelay(25 / portTICK_PERIOD_MS);
  }

  // else delete task
  vTaskDelete(NULL);
}

void execSystemControlTasks()
{

  if (IthoInit && millis() > 250)
  {
    IthoInit = ithoInitCheck();
  }

  if (!i2cStartCommands && millis() > 15000 && (millis() - lastI2CinitRequest > 5000))
  {
    lastI2CinitRequest = millis();
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)1000 / portTICK_PERIOD_MS) == pdTRUE)
    {
      sendQueryDevicetype(false);
      xSemaphoreGive(mutexI2Ctask);
    }
    if (currentItho_fwversion() > 0)
    {
      char logBuff[LOG_BUF_SIZE] = "";
      sprintf(logBuff, "I2C init: QueryDevicetype - fw:%d hw:%d", currentItho_fwversion(), currentIthoDeviceID());
      logInput(logBuff);

      ithoInitResult = 1;
      i2cStartCommands = true;
#if defined(CVE)
      digitalWrite(ITHOSTATUS, HIGH);
#elif defined(NON_CVE)
      digitalWrite(STATUSPIN, HIGH);
#endif

      if (systemConfig.syssht30 > 0)
      {
        if (currentIthoDeviceID() == 0x1B)
        {

          if (xSemaphoreTake(mutexI2Ctask, (TickType_t)1000 / portTICK_PERIOD_MS) == pdTRUE)
          {
            i2c_result_updateweb = false;

            index2410 = 0;

            if (systemConfig.syssht30 == 1)
            {
              /*
                 switch itho hum setting off on every boot because
                 hum sensor setting gets restored in itho firmware after power cycle
              */
              value2410 = 0;
            }

            else if (systemConfig.syssht30 == 2)
            {
              /*
                 if value == 2 setting changed from 1 -> 0
                 then restore itho hum setting back to "on"
              */
              value2410 = 1;
            }
            if (currentItho_fwversion() == 25)
            {
              index2410 = 63;
            }
            else if (currentItho_fwversion() == 26 || currentItho_fwversion() == 27)
            {
              index2410 = 71;
            }
            if (index2410 > 0)
            {
              sendQuery2410(i2c_result_updateweb);
              setSetting2410(i2c_result_updateweb);
              char logBuff[LOG_BUF_SIZE] = "";
              sprintf(logBuff, "I2C init: set hum sensor in itho firmware to: %s", value2410 ? "on" : "off");
              logInput(logBuff);
            }
            xSemaphoreGive(mutexI2Ctask);
          }
        }
        if (systemConfig.syssht30 == 2)
        {
          systemConfig.syssht30 = 0;
          saveSystemConfig();
        }
      }
      if (xSemaphoreTake(mutexI2Ctask, (TickType_t)1000 / portTICK_PERIOD_MS) == pdTRUE)
      {
        sendQueryStatusFormat(false);
        char logBuff[LOG_BUF_SIZE] = "";
        sprintf(logBuff, "I2C init: QueryStatusFormat - items:%d", ithoStatus.size());
        logInput(logBuff);
        xSemaphoreGive(mutexI2Ctask);
      }
      if (xSemaphoreTake(mutexI2Ctask, (TickType_t)1000 / portTICK_PERIOD_MS) == pdTRUE)
      {
        sendQueryStatus(false);
        logInput("I2C init: QueryStatus");
        xSemaphoreGive(mutexI2Ctask);
      }

      sendHomeAssistantDiscovery = true;
    }
    else
    {
      ithoInitResult = -1;
      if (ithoInitResultLogEntry)
      {
        ithoInitResultLogEntry = false;
        logInput("I2C init: QueryDevicetype - failed");
      }
    }
  }

  if (systemConfig.itho_sendjoin > 0 && !joinSend && ithoInitResult == 1)
  {

    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      joinSend = true;
      sendRemoteCmd(0, IthoJoin, virtualRemotes);
      xSemaphoreGive(mutexI2Ctask);
      logInput("I2C init: Virtual remote join command send");

      if (systemConfig.itho_sendjoin == 1)
      {
        systemConfig.itho_sendjoin = 0;
        saveSystemConfig();
      }
    }
  }

  // Itho queue
  if (clearQueue)
  {
    clearQueue = false;
    ithoQueue.clear_queue();
  }
  if (ithoQueue.ithoSpeedUpdated)
  {
    ithoQueue.ithoSpeedUpdated = false;
    uint16_t speed = ithoQueue.get_itho_speed();
    char buf[32]{};
    sprintf(buf, "speed:%d", speed);

    //#if defined (CVE)
    if (writeIthoVal(speed, &ithoCurrentVal, &updateIthoMQTT))
    {
      if (lastCmd.source == nullptr)
        logLastCommand(buf, "ithoQueue");
      sysStatReq = true;
    }
    else
    {
      logLastCommand(buf, "ithoQueue_error");
    }
    //#endif
  }
  // System control tasks
  if ((WiFi.status() != WL_CONNECTED) && !wifiModeAP)
  {
    if (millis() - lastWIFIReconnectAttempt > 60000)
    {
      logInput("Attempt to reconnect WiFi");
      lastWIFIReconnectAttempt = millis();
      // Attempt to reconnect
      if (connectWiFiSTA())
      {
        logInput("Reconnect WiFi successful");
        lastWIFIReconnectAttempt = 0;
      }
      else
      {
        logInput("Reconnect WiFi failed!");
      }
    }
  }
  if (runscan)
  {
    runscan = false;
    scan.once_ms(10, wifiScan);
  }

  if (wifiModeAP)
  {
    if (millis() - APmodeTimeout > 900000)
    { // reboot after 15 min in AP mode
      shouldReboot = true;
    }
    dnsServer.processNextRequest();

    if (millis() - wifiLedUpdate >= 500)
    {
      wifiLedUpdate = millis();
      if (digitalRead(WIFILED) == LOW)
      {
        digitalWrite(WIFILED, HIGH);
      }
      else
      {
        digitalWrite(WIFILED, LOW);
      }
    }
  }

  if (!(currentItho_fwversion() > 0) && millis() - lastVersionCheck > 60000)
  {
    lastVersionCheck = millis();
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)1000 / portTICK_PERIOD_MS) == pdTRUE)
    {
      sendQueryDevicetype(false);
      xSemaphoreGive(mutexI2Ctask);
    }
  }
  // request itho status every systemConfig.itho_updatefreq sec.
  if (millis() - query2401tim >= systemConfig.itho_updatefreq * 1000UL && i2cStartCommands)
  {
    query2401tim = millis();
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      sendQueryStatus(false);
      xSemaphoreGive(mutexI2Ctask);
    }
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      sendQuery31DA(false);
      xSemaphoreGive(mutexI2Ctask);
    }
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      sendQuery31D9(false);
      xSemaphoreGive(mutexI2Ctask);
    }
    updateMQTTihtoStatus = true;
  }
  if (ithoInitResult == -1)
  {
    if (millis() - mqttUpdatetim >= systemConfig.itho_updatefreq * 1000UL)
    {
      mqttUpdatetim = millis();
      updateMQTTihtoStatus = true;
    }
  }
  if (get2410)
  {
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      get2410 = false;
      resultPtr2410 = sendQuery2410(i2c_result_updateweb);

      xSemaphoreGive(mutexI2Ctask);
    }
  }
  if (set2410)
  {
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
      setSetting2410(i2c_result_updateweb);
      set2410 = false;

      xSemaphoreGive(mutexI2Ctask);
    }
    getSettingsHack.once_ms(1, []()
                            { getSetting(index2410, true, false, false); });
  }

  if (systemConfig.syssht30 == 1)
  {
    if (millis() - SHT3x_readout >= systemConfig.itho_updatefreq * 1000UL && (SHT3x_original || SHT3x_alternative))
    {
      SHT3x_readout = millis();

      if (SHT3x_original || SHT3x_alternative)
      {
        if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
        {
          if (SHT3x_original)
          {
            if (sht_org.readSample())
            {
              ithoHum = sht_org.getHumidity();
              ithoTemp = sht_org.getTemperature();
            }
          }
          if (SHT3x_alternative)
          {
            if (sht_alt.readSample())
            {
              ithoHum = sht_alt.getHumidity();
              ithoTemp = sht_alt.getTemperature();
            }
          }
          xSemaphoreGive(mutexI2Ctask);
        }
      }
    }
  }
}

void wifiInit()
{
  if (!loadWifiConfig())
  {
    logInput("Setup: Wifi config load failed");
    setupWiFiAP();
  }
  else if (!connectWiFiSTA())
  {
    logInput("Setup: Wifi connect STA failed");
    setupWiFiAP();
  }
  configTime(0, 0, wifiConfig.ntpserver);

  WiFi.scanDelete();
  if (WiFi.scanComplete() == -2)
  {
    WiFi.scanNetworks(true);
  }
  if (!wifiModeAP)
  {
    logWifiInfo();
  }
  else
  {
    logInput("Setup: AP mode active");
  }
}

void setupWiFiAP()
{

  /* Soft AP network parameters */
  IPAddress apIP(192, 168, 4, 1);
  IPAddress netMsk(255, 255, 255, 0);

  WiFi.persistent(false); // Do not use SDK storage of SSID/WPA parameters
  esp_wifi_set_storage(WIFI_STORAGE_RAM);

  WiFi.disconnect(true);
  WiFi.setAutoReconnect(false);
  delay(200);
  WiFi.mode(WIFI_AP);

  esp_wifi_set_ps(WIFI_PS_NONE);

  delay(100);

  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(hostName(), WiFiAPPSK);

  delay(500);

  /* Setup the DNS server redirecting all the domains to the apIP */
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);

  wifiModeAP = true;

  APmodeTimeout = millis();
  logInput("wifi AP mode started");
}

bool connectWiFiSTA()
{
  wifiModeAP = false;
  D_LOG("Connecting to wireless network...\n");

  WiFi.persistent(false);
  if (!WiFi.setAutoReconnect(false))
    D_LOG("Unable to set auto reconnect\n");
  if (!WiFi.mode(WIFI_MODE_NULL))
    D_LOG("Unable to set WiFi mode\n");
  if (!WiFi.setHostname(hostName()))
    D_LOG("Unable to set host name ('%s')\n", hostName());
  if (!WiFi.mode(WIFI_STA))
    D_LOG("Unable to set WiFi mode\n");
  if (!WiFi.disconnect(true))
    D_LOG("WiFi disconnect failed\n");
  // ESP32 doesn't reliably set the status to WL_DISCONNECTED despite disconnect() call.
  WiFiSTAClass::_setStatus(WL_DISCONNECTED);

  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (strcmp(wifiConfig.dhcp, "off") == 0)
  {
    bool configOK = true;
    IPAddress staticIP;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;

    if (!staticIP.fromString(wifiConfig.ip))
    {
      configOK = false;
    }
    if (!gateway.fromString(wifiConfig.gateway))
    {
      configOK = false;
    }
    if (!subnet.fromString(wifiConfig.subnet))
    {
      configOK = false;
    }
    if (!dns1.fromString(wifiConfig.dns1))
    {
      configOK = false;
    }
    if (!dns2.fromString(wifiConfig.dns2))
    {
      configOK = false;
    }
    if (configOK)
    {
      if (!WiFi.config(staticIP, gateway, subnet, dns1, dns2))
      {
        logInput("Static IP config NOK");
      }
      else
      {
        logInput("Static IP config OK");
      }
    }
  }

  delay(200);

  WiFi.begin(wifiConfig.ssid, wifiConfig.passwd);

  auto timeoutmillis = millis() + 30000;
  auto status = WiFi.status();

  while (millis() < timeoutmillis)
  {

    esp_task_wdt_reset();

    status = WiFi.status();
    if (status == WL_CONNECTED)
    {
      digitalWrite(WIFILED, LOW);
      return true;
    }
    if (digitalRead(WIFILED) == LOW)
    {
      digitalWrite(WIFILED, HIGH);
    }
    else
    {
      digitalWrite(WIFILED, LOW);
    }

    delay(100);
  }

  digitalWrite(WIFILED, HIGH);

  return false;
}

void initSensor()
{

  if (systemConfig.syssht30 == 1)
  {
    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
    }
    else
    {
      logInput("Error: SHT i2c semaphore not available");
      return;
    }

    if (sht_org.init() && sht_org.readSample())
    {
      SHT3x_original = true;
    }
    else if (sht_alt.init() && sht_alt.readSample())
    {
      SHT3x_alternative = true;
    }
    if (SHT3x_original)
    {
      logInput("Setup: Original SHT30 sensor found");
    }
    else if (SHT3x_alternative)
    {
      logInput("Setup: Alternative SHT30 sensor found");
    }
    if (SHT3x_original || SHT3x_alternative)
    {
      xSemaphoreGive(mutexI2Ctask);
      return;
    }

    delay(200);

    if (sht_org.init() && sht_org.readSample())
    {
      SHT3x_original = true;
    }
    else if (sht_alt.init() && sht_alt.readSample())
    {
      SHT3x_alternative = true;
    }
    if (SHT3x_original)
    {
      logInput("Setup: Original SHT30 sensor found (2nd try)");
    }
    else if (SHT3x_alternative)
    {
      logInput("Setup: Alternative SHT30 sensor found (2nd try)");
    }
    else
    {
      systemConfig.syssht30 = 0;
      logInput("Setup: SHT30 sensor not present");
    }
    xSemaphoreGive(mutexI2Ctask);
  }
}

void init_vRemote()
{
  // setup virtual remote
  char buff[128];
  sprintf(buff, "Setup: Virtual remotes, start ID: %d,%d,%d", sys.getMac(6 - 3), sys.getMac(6 - 2), sys.getMac(6 - 2));
  logInput(buff);

  virtualRemotes.setMaxRemotes(systemConfig.itho_numvrem);
  loadVirtualRemotesConfig();
}

bool ithoInitCheck()
{
#if defined(CVE)
  if (digitalRead(STATUSPIN) == LOW)
  {
    return false;
  }
  sendI2CPWMinit();
#endif
  return false;
}

void update_queue()
{
  ithoQueue.update_queue();
}

// Update itho Value
bool writeIthoVal(uint16_t value, volatile uint16_t *ithoCurrentVal, bool *updateIthoMQTT)
{
  if (value > 254)
    return false;

  if (*ithoCurrentVal != value)
  {

    if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
    {
    }
    else
    {
      return false;
    }

    IthoPWMcommand(value, ithoCurrentVal, updateIthoMQTT);

    xSemaphoreGive(mutexI2Ctask);

    return true;
  }
  return false;
}

bool ithoI2CCommand(uint8_t remoteIndex, const char *command, cmdOrigin origin)
{
  D_LOG("EXEC VREMOTE BUTTON COMMAND:%s remote:%d\n", command, remoteIndex);

  if (xSemaphoreTake(mutexI2Ctask, (TickType_t)500 / portTICK_PERIOD_MS) == pdTRUE)
  {
  }
  else
  {
    return false;
  }

  bool updateweb = false;
  if (origin == WEB)
    updateweb = true;

  if (strcmp(command, "away") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoAway, virtualRemotes);
  }
  else if (strcmp(command, "low") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoLow, virtualRemotes);
  }
  else if (strcmp(command, "medium") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoMedium, virtualRemotes);
  }
  else if (strcmp(command, "high") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoHigh, virtualRemotes);
  }
  else if (strcmp(command, "timer1") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoTimer1, virtualRemotes);
  }
  else if (strcmp(command, "timer2") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoTimer2, virtualRemotes);
  }
  else if (strcmp(command, "timer3") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoTimer3, virtualRemotes);
  }
  else if (strcmp(command, "cook30") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoCook30, virtualRemotes);
  }
  else if (strcmp(command, "cook60") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoCook60, virtualRemotes);
  }
  else if (strcmp(command, "auto") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoAuto, virtualRemotes);
  }
  else if (strcmp(command, "autonight") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoAutoNight, virtualRemotes);
  }
  else if (strcmp(command, "join") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoJoin, virtualRemotes);
  }
  else if (strcmp(command, "leave") == 0)
  {
    sendRemoteCmd(remoteIndex, IthoLeave, virtualRemotes);
  }
  else if (strcmp(command, "type") == 0)
  {
    sendQueryDevicetype(updateweb);
  }
  else if (strcmp(command, "status") == 0)
  {
    sendQueryStatus(updateweb);
  }
  else if (strcmp(command, "statusformat") == 0)
  {
    sendQueryStatusFormat(updateweb);
  }
  else if (strcmp(command, "31DA") == 0)
  {
    sendQuery31DA(updateweb);
  }
  else if (strcmp(command, "31D9") == 0)
  {
    sendQuery31D9(updateweb);
  }
  else if (strcmp(command, "10D0") == 0)
  {
    filterReset(0, virtualRemotes);
  }
  else
  {

    xSemaphoreGive(mutexI2Ctask);

    return false;
  }

  const char *source;
  auto it = cmdOriginMap.find(origin);
  if (it != cmdOriginMap.end())
    source = it->second;
  else
    source = cmdOriginMap.rbegin()->second;

  char originchar[30]{};
  sprintf(originchar, "%s-vremote-%d", source, remoteIndex);

  logLastCommand(command, originchar);

  xSemaphoreGive(mutexI2Ctask);

  return true;
}