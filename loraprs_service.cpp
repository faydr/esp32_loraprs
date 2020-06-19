#include "loraprs_service.h"

namespace LoraPrs {
  
Service::Service() 
  : serialBt_()
  , kissState_(KissState::Void)
  , kissCmd_(KissCmd::NoCmd)
{
}

void Service::setup(const Config &conf)
{
  previousBeaconMs_ = 0;

  // config
  isClient_ = conf.IsClientMode;
  loraFreq_ = conf.LoraFreq;
  ownCallsign_ = AX25::Callsign(conf.AprsLogin);
  if (!ownCallsign_.IsValid()) {
    Serial.println("Own callsign is not valid");
  }
  
  aprsLogin_ = String("user ") + conf.AprsLogin + String(" pass ") + 
    conf.AprsPass + String(" vers ") + CfgLoraprsVersion;
  if (conf.AprsFilter.length() > 0) {
    aprsLogin_ += String(" filter ") + conf.AprsFilter;
  }
  aprsLogin_ += String("\n");
  
  aprsHost_ = conf.AprsHost;
  aprsPort_ = conf.AprsPort;
  aprsBeacon_ = conf.AprsRawBeacon;
  aprsBeaconPeriodMinutes_ = conf.AprsRawBeaconPeriodMinutes;
  
  autoCorrectFreq_ = conf.EnableAutoFreqCorrection;
  addSignalReport_ = conf.EnableSignalReport;
  persistentConn_ = conf.EnablePersistentAprsConnection;
  enableRfToIs_ = conf.EnableRfToIs;
  enableIsToRf_ = conf.EnableIsToRf;
  enableRepeater_ = conf.EnableRepeater;
  enableBeacon_ = conf.EnableBeacon;

  // peripherals
  setupLora(conf.LoraFreq, conf.LoraBw, conf.LoraSf, conf.LoraCodingRate, conf.LoraPower, conf.LoraSync);
    
  if (needsWifi()) {
    setupWifi(conf.WifiSsid, conf.WifiKey);
  }

  if (needsBt()) {
    setupBt(conf.BtName);
  }
  
  if (needsAprsis() && persistentConn_) {
    reconnectAprsis();
  }
}

void Service::setupWifi(const String &wifiName, const String &wifiKey)
{
  if (!isClient_) {
    Serial.print("WIFI connecting to " + wifiName);

    WiFi.setHostname("loraprs");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiName.c_str(), wifiKey.c_str());

    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("ok");
    Serial.println(WiFi.localIP());
  }
}

void Service::reconnectWifi()
{
  Serial.print("WIFI re-connecting...");

  while (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0,0,0,0)) {
    WiFi.reconnect();
    delay(500);
    Serial.print(".");
  }

  Serial.println("ok");
}

bool Service::reconnectAprsis()
{
  Serial.print("APRSIS connecting...");
  
  if (!aprsisConn_.connect(aprsHost_.c_str(), aprsPort_)) {
    Serial.println("Failed to connect to " + aprsHost_ + ":" + aprsPort_);
    return false;
  }
  Serial.println("ok");

  aprsisConn_.print(aprsLogin_);
  return true;
}

void Service::setupLora(int loraFreq, int bw, byte sf, byte cr, byte pwr, byte sync)
{
  Serial.print("LoRa init...");
  
  LoRa.setPins(CfgPinSs, CfgPinRst, CfgPinDio0);
  
  while (!LoRa.begin(loraFreq)) {
    Serial.print(".");
    delay(500);
  }
  LoRa.setSyncWord(sync);
  LoRa.setSpreadingFactor(sf);
  LoRa.setSignalBandwidth(bw);
  LoRa.setCodingRate4(cr);
  LoRa.setTxPower(pwr);
  LoRa.enableCrc();
  
  Serial.println("ok");  
}

void Service::setupBt(const String &btName)
{
  Serial.print("BT init " + btName + "...");
  
  if (serialBt_.begin(btName)) {
    Serial.println("ok");
  }
  else
  {
    Serial.println("failed");
  }
}

void Service::loop()
{
  if (needsWifi() && WiFi.status() != WL_CONNECTED) {
    reconnectWifi();
  }
  if (needsAprsis() && !aprsisConn_.connected() && persistentConn_) {
    reconnectAprsis();
  }
  if (aprsisConn_.available() > 0) {
    onAprsisDataAvailable();
  }
  if (serialBt_.available()) {
    onBtDataAvailable();
  }
  if (int packetSize = LoRa.parsePacket()) {
    onLoraDataAvailable(packetSize);
  }
  if (needsBeacon()) {
    sendPeriodicBeacon();
  }
  delay(CfgPollDelayMs);
}

void Service::sendPeriodicBeacon()
{
  long currentMs = millis();
  
  if (previousBeaconMs_ == 0 || currentMs - previousBeaconMs_ >= aprsBeaconPeriodMinutes_ * 60 * 1000) {
      AX25::Payload payload(aprsBeacon_);
      if (payload.IsValid()) {
        sendToLora(payload);
        if (enableRfToIs_) {
          sendToAprsis(payload.ToString());
        }
        Serial.println("Periodic beacon is sent");
      }
      else {
        Serial.println("Beacon payload is invalid");
      }
      previousBeaconMs_ = currentMs;
  }
}

void Service::sendToAprsis(String aprsMessage)
{
  if (needsWifi() && WiFi.status() != WL_CONNECTED) {
    reconnectWifi();
  }
  if (needsAprsis() && !aprsisConn_.connected()) {
    reconnectAprsis();
  }
  aprsisConn_.println(aprsMessage);

  if (!persistentConn_) {
    aprsisConn_.stop();
  }
}

void Service::onAprsisDataAvailable()
{
  String aprsisData;
  
  while (aprsisConn_.available() > 0) {
    char c = aprsisConn_.read();
    if (c == '\r') continue;
    Serial.print(c);
    if (c == '\n') break;
    aprsisData += c;
  }

  if (enableIsToRf_ && aprsisData.length() > 0) {
    AX25::Payload payload(aprsisData);
    if (payload.IsValid()) {
      sendToLora(payload);
    }
    else {
      Serial.println("Invalid payload from APRSIS");
    }
  }
}

bool Service::sendToLora(const AX25::Payload &payload) 
{
  byte buf[512];
  int bytesWritten = payload.ToBinary(buf, sizeof(buf));
  if (bytesWritten <= 0) {
    Serial.println("Failed to serialize payload");
    return false;
  
  }
  LoRa.beginPacket();
  LoRa.write(buf, bytesWritten);
  LoRa.endPacket();
  return true;
}

void Service::onLoraDataAvailable(int packetSize)
{
  int rxBufIndex = 0;
  byte rxBuf[packetSize];

  serialBt_.write(KissMarker::Fend);
  serialBt_.write(KissCmd::Data);

  while (LoRa.available()) {
    byte rxByte = LoRa.read();

    if (rxByte == KissMarker::Fend) {
      serialBt_.write(KissMarker::Fesc);
      serialBt_.write(KissMarker::Tfend);
    }
    else if (rxByte == KissMarker::Fesc) {
      serialBt_.write(KissMarker::Fesc);
      serialBt_.write(KissMarker::Tfesc);
    }
    else {
      rxBuf[rxBufIndex++] = rxByte;
      serialBt_.write(rxByte);
    }
  }

  serialBt_.write(KissMarker::Fend);

  float snr = LoRa.packetSnr();
  float rssi = LoRa.packetRssi();
  long frequencyError = LoRa.packetFrequencyError();

  String signalReport = String(" ") +
    String("rssi: ") +
    String(snr < 0 ? rssi + snr : rssi) +
    String("dBm, ") +
    String("snr: ") +
    String(snr) +
    String("dB, ") +
    String("err: ") +
    String(frequencyError) +
    String("Hz");

  if (autoCorrectFreq_) {
    loraFreq_ -= frequencyError;
    LoRa.setFrequency(loraFreq_);
  }

  AX25::Payload payload(rxBuf, rxBufIndex);

  if (payload.IsValid()) {
    String textPayload = payload.ToString(addSignalReport_ ? signalReport : String());
    Serial.println(textPayload);

    if (enableRfToIs_ && !isClient_) {
      sendToAprsis(textPayload);
      Serial.println("Packet sent to APRS-IS");
    }
    if (enableRepeater_ && !isClient_ && payload.Digirepeat(ownCallsign_)) {
      sendToLora(payload);
      Serial.println("Packet digirepeated");
    }
  }
  else {
    Serial.println("Invalid or unsupported payload from LoRA");
  }
}

void Service::kissResetState()
{
  kissCmd_ = KissCmd::NoCmd;
  kissState_ = KissState::Void;
}

void Service::onBtDataAvailable() 
{ 
  while (serialBt_.available()) {
    byte txByte = serialBt_.read();

    switch (kissState_) {
      case KissState::Void:
        if (txByte == KissMarker::Fend) {
          kissCmd_ = KissCmd::NoCmd;
          kissState_ = KissState::GetCmd;
        }
        break;
      case KissState::GetCmd:
        if (txByte != KissMarker::Fend) {
          if (txByte == KissCmd::Data) {
            LoRa.beginPacket();
            kissCmd_ = (KissCmd)txByte;
            kissState_ = KissState::GetData;
          }
          else {
            kissResetState();
          }
        }
        break;
      case KissState::GetData:
        if (txByte == KissMarker::Fesc) {
          kissState_ = KissState::Escape;
        }
        else if (txByte == KissMarker::Fend) {
          if (kissCmd_ == KissCmd::Data) {
            LoRa.endPacket();
          }
          kissResetState();
        }
        else {
          LoRa.write(txByte);
        }
        break;
      case KissState::Escape:
        if (txByte == KissMarker::Tfend) {
          LoRa.write(KissMarker::Fend);
          kissState_ = KissState::GetData;
        }
        else if (txByte == KissMarker::Tfesc) {
          LoRa.write(KissMarker::Fesc);
          kissState_ = KissState::GetData;
        }
        else {
          kissResetState();
        }
        break;
      default:
        break;
    }
  }
}

} // LoraPrs
