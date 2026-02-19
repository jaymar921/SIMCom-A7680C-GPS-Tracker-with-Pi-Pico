/*
  Pi Pico (Arduino IDE) - SIMCom A7680C GPS Tracker
  ===================================================
  Board Selection:
    Tools > Board > Raspberry Pi RP2040 Boards > Raspberry Pi Pico

  Wiring:
    A7680C TX     → Pi Pico GP1  (Pin 2  - Serial1 RX)
    A7680C RX     → Pi Pico GP0  (Pin 1  - Serial1 TX)
    A7680C PWRKEY → Pi Pico GP2  (Pin 4)
    A7680C GND    → Pi Pico GND  (Pin 3)
    A7680C VCC    → External 3.7V-4.2V supply
    External GND  → Pi Pico GND  (share ground!)

  Features:
    - Auto boot via PWRKEY
    - Satellite count display while waiting for fix
    - SMS on device start + first GPS fix + every N fixes
    - HTTP POST every N seconds to update server

  Serial Monitor baud: 115200
*/

// ── Configuration ───────────────────────────────────────────────
#define MODULE_SERIAL Serial1
#define DEBUG_SERIAL Serial
#define BAUD_RATE 115200
#define PWRKEY_PIN 2
#define READ_TIMEOUT 10000
#define POLL_INTERVAL 5000 // ms between GPS reads
#define MODULE_RETRY_MAX 10
#define MODULE_RETRY_DELAY 2000
#define SMS_FIX_INTERVAL 5       // send SMS every N fixes
#define HTTP_POST_INTERVAL 30000 // ms between HTTP POST (30 seconds)

// ── SMS Configuration ────────────────────────────────────────────
#define SMS_ENABLED true
const String PHONE_NUMBER = "+639XXXXXXXXX"; // ← your number here

// ── HTTP API Configuration ───────────────────────────────────────
// ACTUALLY DEPENDS ON THE API YOU SETUP
// Mine requires a device uuid for database check
// and will send the { lat, long, and speed } to the server
#define HTTP_ENABLED true
const String API_URL = "<server url>";     // ← your API endpoint
const String DEVICE_ID = "uuid-of-device"; // ← your device UUID

// ── APN Configuration (for Smart Philippines) ────────────────────
// Set to true if HTTP fails and you need to manually configure APN
#define MANUAL_APN_CONFIG true
const String APN_NAME = "internet"; // Smart PH: "internet" or "smartlte"
// ────────────────────────────────────────────────────────────────

struct GpsData
{
  float latitude;
  float longitude;
  float altitude;
  float speedKnots;
  float course;
  String date;
  String utcTime;
  bool valid;
};

bool moduleReady = false;
bool smsSentBoot = false;     // SMS 1: sent on module alive
bool smsSentStart = false;    // SMS 2: sent on first satellite detected
bool smsSentFix = false;      // SMS 3: sent on first GPS fix
bool httpInitialized = false; // track if HTTP service is initialized
int attempt = 0;
int fixCount = 0; // counts valid fixes for SMS_FIX_INTERVAL
long lastPollTime = -(long)POLL_INTERVAL;
long lastHttpPostTime = -(long)HTTP_POST_INTERVAL;

// ── Format helpers (declare early for use in other functions) ────
String formatDate(const String &d)
{
  if (d.length() < 6)
    return d;
  return d.substring(0, 2) + "/" + d.substring(2, 4) + "/20" + d.substring(4, 6);
}

String formatTime(const String &t)
{
  if (t.length() < 6)
    return t;
  return t.substring(0, 2) + ":" + t.substring(2, 4) + ":" + t.substring(4, 6) + " UTC";
}

// ── Power on the module via PWRKEY ───────────────────────────────
void powerOnModule()
{
  DEBUG_SERIAL.println("Pulsing PWRKEY to boot A7680C...");
  pinMode(PWRKEY_PIN, OUTPUT);
  digitalWrite(PWRKEY_PIN, LOW);
  delay(500);
  digitalWrite(PWRKEY_PIN, HIGH);
  delay(1500);
  digitalWrite(PWRKEY_PIN, LOW);
  DEBUG_SERIAL.println("PWRKEY released. Waiting for module to boot...");
  delay(3000);
}

// ── Send AT command — waits for line silence ─────────────────────
String sendAT(const String &command, unsigned long timeout = READ_TIMEOUT)
{
  const unsigned long SILENCE_MS = 1500;

  while (MODULE_SERIAL.available())
    MODULE_SERIAL.read();
  MODULE_SERIAL.println(command);
  delay(500);

  String response = "";
  unsigned long lastByteTime = millis();
  unsigned long start = millis();

  while (millis() - start < timeout)
  {
    if (MODULE_SERIAL.available())
    {
      while (MODULE_SERIAL.available())
        response += (char)MODULE_SERIAL.read();
      lastByteTime = millis();
    }
    if (response.length() > 0 && millis() - lastByteTime >= SILENCE_MS)
      break;
    delay(10);
  }

  response.trim();
  return response;
}

// ── Send SMS ─────────────────────────────────────────────────────
bool sendSMS(const String &number, const String &message)
{
  if (!SMS_ENABLED)
    return false;

  DEBUG_SERIAL.println("Sending SMS to " + number + "...");

  String r = sendAT("AT+CMGF=1", 5000);
  if (r.indexOf("OK") == -1)
  {
    DEBUG_SERIAL.println("SMS ERROR: Could not set text mode.");
    return false;
  }

  MODULE_SERIAL.println("AT+CMGS=\"" + number + "\"");
  delay(2000);

  MODULE_SERIAL.print(message);
  MODULE_SERIAL.write(26); // Ctrl+Z to send

  String resp = "";
  unsigned long start = millis();
  while (millis() - start < 10000)
  {
    while (MODULE_SERIAL.available())
      resp += (char)MODULE_SERIAL.read();
    if (resp.indexOf("+CMGS:") != -1)
      break;
    if (resp.indexOf("ERROR") != -1)
      break;
    delay(100);
  }

  if (resp.indexOf("+CMGS:") != -1)
  {
    DEBUG_SERIAL.println("SMS sent OK.");
    return true;
  }
  else
  {
    DEBUG_SERIAL.println("SMS failed. Response: " + resp);
    return false;
  }
}

// ── Initialize HTTP service ──────────────────────────────────────
bool initHTTP()
{
  if (httpInitialized)
    return true;

  DEBUG_SERIAL.println("Initializing HTTP service...");

  // Step 1: Check network registration
  DEBUG_SERIAL.println("Checking network registration...");
  String netResp = sendAT("AT+CGREG?", 3000);
  if (netResp.indexOf("+CGREG: 0,1") == -1 && netResp.indexOf("+CGREG: 0,5") == -1)
  {
    DEBUG_SERIAL.println("Not registered on network: " + netResp);
    return false;
  }
  DEBUG_SERIAL.println("Network registered OK.");

  // Step 2: Configure APN manually if enabled (for carriers that need it)
  if (MANUAL_APN_CONFIG)
  {
    DEBUG_SERIAL.println("Configuring APN: " + APN_NAME);
    String apnResp = sendAT("AT+CGDCONT=1,\"IP\",\"" + APN_NAME + "\"", 3000);
    DEBUG_SERIAL.println("APN config: " + apnResp);
    delay(500);
  }

  // Step 3: Verify PDP context status (for diagnostics only)
  String pdpResp = sendAT("AT+CGACT?", 2000);
  DEBUG_SERIAL.println("PDP status: " + pdpResp);

  // Step 4: Terminate any existing HTTP session
  DEBUG_SERIAL.println("Terminating existing HTTP session...");
  sendAT("AT+HTTPTERM", 2000);
  delay(1000);

  // Step 5: Initialize HTTP
  DEBUG_SERIAL.println("Sending AT+HTTPINIT...");
  String initResp = sendAT("AT+HTTPINIT", 5000);
  DEBUG_SERIAL.println("HTTPINIT response: " + initResp);

  if (initResp.indexOf("ERROR") != -1)
  {
    DEBUG_SERIAL.println("HTTP init failed or already initialized.");
    return false;
  }

  if (initResp.indexOf("OK") != -1)
  {
    DEBUG_SERIAL.println("HTTP service initialized successfully.");
    httpInitialized = true;
    return true;
  }

  DEBUG_SERIAL.println("Unexpected HTTPINIT response.");
  return false;
}

// ── HTTP POST ────────────────────────────────────────────────────
bool httpPost(const String &url, const String &jsonBody)
{
  if (!HTTP_ENABLED)
    return false;
  if (!httpInitialized)
    return false;

  DEBUG_SERIAL.println("HTTP POST to: " + url);
  DEBUG_SERIAL.println("Body: " + jsonBody);

  // Step 1: Set URL
  String resp = sendAT("AT+HTTPPARA=\"URL\",\"" + url + "\"", 3000);
  if (resp.indexOf("OK") == -1)
  {
    DEBUG_SERIAL.println("Failed to set URL: " + resp);
    return false;
  }

  // Step 2: Set content type
  resp = sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"", 2000);
  if (resp.indexOf("OK") == -1)
  {
    DEBUG_SERIAL.println("Failed to set content type: " + resp);
    return false;
  }

  // Step 3: Send data size and wait for DOWNLOAD prompt
  int bodyLen = jsonBody.length();
  DEBUG_SERIAL.print("Sending ");
  DEBUG_SERIAL.print(bodyLen);
  DEBUG_SERIAL.println(" bytes...");

  MODULE_SERIAL.println("AT+HTTPDATA=" + String(bodyLen) + ",10000");

  // Wait for DOWNLOAD prompt
  unsigned long start = millis();
  String downloadResp = "";
  while (millis() - start < 5000)
  {
    while (MODULE_SERIAL.available())
    {
      downloadResp += (char)MODULE_SERIAL.read();
    }
    if (downloadResp.indexOf("DOWNLOAD") != -1)
      break;
    delay(50);
  }

  if (downloadResp.indexOf("DOWNLOAD") == -1)
  {
    DEBUG_SERIAL.println("No DOWNLOAD prompt: " + downloadResp);
    return false;
  }

  DEBUG_SERIAL.println("Got DOWNLOAD prompt, sending data...");

  // Step 4: Send the JSON body
  MODULE_SERIAL.print(jsonBody);

  // Wait for OK after data upload
  start = millis();
  String dataResp = "";
  while (millis() - start < 3000)
  {
    while (MODULE_SERIAL.available())
    {
      dataResp += (char)MODULE_SERIAL.read();
    }
    if (dataResp.indexOf("OK") != -1)
      break;
    delay(50);
  }

  DEBUG_SERIAL.println("Data upload response: " + dataResp);

  // Step 5: Execute POST (method=1)
  DEBUG_SERIAL.println("Executing POST...");
  resp = sendAT("AT+HTTPACTION=1", 3000);
  if (resp.indexOf("OK") == -1)
  {
    DEBUG_SERIAL.println("HTTPACTION failed: " + resp);
    return false;
  }

  // Step 6: Wait for +HTTPACTION URC with result
  DEBUG_SERIAL.println("Waiting for response...");
  start = millis();
  String actionResp = "";
  while (millis() - start < 15000)
  { // 15 second timeout for server response
    while (MODULE_SERIAL.available())
    {
      actionResp += (char)MODULE_SERIAL.read();
    }
    if (actionResp.indexOf("+HTTPACTION:") != -1)
      break;
    delay(100);
  }

  DEBUG_SERIAL.println("Server response: " + actionResp);

  // Check status code (format: +HTTPACTION: 1,<statuscode>,<datalen>)
  if (actionResp.indexOf("+HTTPACTION: 1,200") != -1 ||
      actionResp.indexOf("+HTTPACTION: 1,201") != -1)
  {
    DEBUG_SERIAL.println("HTTP POST successful (200/201).");
    return true;
  }
  else if (actionResp.indexOf("+HTTPACTION: 1,") != -1)
  {
    // Extract status code for debugging
    int codeStart = actionResp.indexOf("+HTTPACTION: 1,") + 15;
    int codeEnd = actionResp.indexOf(",", codeStart);
    String statusCode = actionResp.substring(codeStart, codeEnd);
    DEBUG_SERIAL.println("HTTP POST returned status: " + statusCode);
    return false;
  }
  else
  {
    DEBUG_SERIAL.println("No response from server or timeout.");
    return false;
  }
}

// ── Parse AT+CGPSINFO ────────────────────────────────────────────
GpsData parseCgpsInfo(const String &raw)
{
  GpsData data = {0, 0, 0, 0, 0, "", "", false};

  int start = raw.indexOf("+CGPSINFO:");
  if (start == -1)
    return data;

  String payload = raw.substring(start + 10);
  int lineEnd = payload.indexOf('\n');
  if (lineEnd != -1)
    payload = payload.substring(0, lineEnd);
  payload.trim();

  String fields[9];
  int fieldCount = 0, prev = 0;
  for (int i = 0; i <= (int)payload.length() && fieldCount < 9; i++)
  {
    if (i == (int)payload.length() || payload[i] == ',')
    {
      fields[fieldCount++] = payload.substring(prev, i);
      fields[fieldCount - 1].trim();
      prev = i + 1;
    }
  }

  if (fieldCount < 9 || fields[0].length() == 0)
    return data;

  float rawLat = fields[0].toFloat();
  int latDeg = (int)(rawLat / 100);
  float lat = latDeg + (rawLat - latDeg * 100) / 60.0;
  if (fields[1] == "S")
    lat = -lat;

  float rawLon = fields[2].toFloat();
  int lonDeg = (int)(rawLon / 100);
  float lon = lonDeg + (rawLon - lonDeg * 100) / 60.0;
  if (fields[3] == "W")
    lon = -lon;

  data.latitude = lat;
  data.longitude = lon;
  data.date = fields[4];
  data.utcTime = fields[5];
  data.altitude = fields[6].toFloat();
  data.speedKnots = fields[7].toFloat();
  data.course = fields[8].toFloat();
  data.valid = true;

  return data;
}

// ── Get satellite count from AT+CGNSSINFO ────────────────────────
int getSatelliteCount()
{
  String raw = sendAT("AT+CGNSSINFO");
  int idx = raw.indexOf("+CGNSSINFO:");
  if (idx == -1)
    return -1;

  String payload = raw.substring(idx + 11);
  payload.trim();

  String fields[6];
  int fieldCount = 0, prev = 0;
  for (int i = 0; i <= (int)payload.length() && fieldCount < 6; i++)
  {
    if (i == (int)payload.length() || payload[i] == ',')
    {
      fields[fieldCount++] = payload.substring(prev, i);
      fields[fieldCount - 1].trim();
      prev = i + 1;
    }
  }

  if (fieldCount < 2)
    return -1;
  if (fields[0].length() == 0)
    return 0;
  if (fields[1].length() == 0)
    return 0;
  return fields[1].toInt();
}

// ═══════════════════════════════════════════════════════════════
//  UTILITY — called every time a valid GPS fix is received
// ═══════════════════════════════════════════════════════════════
void onGpsData(const GpsData &gps)
{
  // ── Print to Serial Monitor ────────────────────────────────────
  DEBUG_SERIAL.println("----------------------------------------");
  DEBUG_SERIAL.print("Fix #");
  DEBUG_SERIAL.print(fixCount);
  DEBUG_SERIAL.print(" (attempt ");
  DEBUG_SERIAL.print(attempt);
  DEBUG_SERIAL.println(")");
  DEBUG_SERIAL.print("  Date      : ");
  DEBUG_SERIAL.println(formatDate(gps.date));
  DEBUG_SERIAL.print("  Time      : ");
  DEBUG_SERIAL.println(formatTime(gps.utcTime));
  DEBUG_SERIAL.print("  Latitude  : ");
  DEBUG_SERIAL.println(gps.latitude, 6);
  DEBUG_SERIAL.print("  Longitude : ");
  DEBUG_SERIAL.println(gps.longitude, 6);
  DEBUG_SERIAL.print("  Altitude  : ");
  DEBUG_SERIAL.print(gps.altitude, 1);
  DEBUG_SERIAL.println(" m");
  DEBUG_SERIAL.print("  Speed     : ");
  DEBUG_SERIAL.print(gps.speedKnots, 2);
  DEBUG_SERIAL.println(" knots");
  DEBUG_SERIAL.print("  Course    : ");
  DEBUG_SERIAL.print(gps.course, 1);
  DEBUG_SERIAL.println(" deg");

  // ── SMS every SMS_FIX_INTERVAL fixes ──────────────────────────
  if (fixCount % SMS_FIX_INTERVAL == 0)
  {
    DEBUG_SERIAL.print("Fix #");
    DEBUG_SERIAL.print(fixCount);
    DEBUG_SERIAL.println(" — sending periodic SMS...");

    String msg = "";
    msg += "speed= " + String(gps.speedKnots, 2) + " knots\n";
    msg += "lat= " + String(gps.latitude, 6) + "\n";
    msg += "long= " + String(gps.longitude, 6);
    sendSMS(PHONE_NUMBER, msg);
  }

  DEBUG_SERIAL.println("----------------------------------------");
}

// ── Non-blocking module wake with retry ─────────────────────────
void tryWakeModule()
{
  static int retryCount = 0;
  static long lastRetryTime = -(long)MODULE_RETRY_DELAY;

  if (millis() - lastRetryTime < MODULE_RETRY_DELAY)
    return;
  lastRetryTime = millis();

  retryCount++;
  DEBUG_SERIAL.print("Attempt ");
  DEBUG_SERIAL.print(retryCount);
  DEBUG_SERIAL.print("/");
  DEBUG_SERIAL.print(MODULE_RETRY_MAX);
  DEBUG_SERIAL.print(" - Contacting module... ");

  String resp = sendAT("AT");

  if (resp.indexOf("OK") != -1)
  {
    DEBUG_SERIAL.println("OK! Module is alive.");
    moduleReady = true;

    // SMS 1: device has started
    if (!smsSentBoot)
    {
      sendSMS(PHONE_NUMBER, "The device has started.");
      smsSentBoot = true;
    }

    // Power on GNSS
    String gnssResp = sendAT("AT+CGNSSPWR=1");
    if (gnssResp.indexOf("OK") != -1)
    {
      DEBUG_SERIAL.println("GNSS power ON. Searching for satellites...");
    }
    else
    {
      DEBUG_SERIAL.println("WARN: " + gnssResp);
    }

    // Initialize HTTP service early (if enabled) so we catch errors early
    if (HTTP_ENABLED)
    {
      delay(2000); // give module time to settle after GNSS start
      initHTTP();
    }
  }
  else
  {
    DEBUG_SERIAL.println("No response.");
    if (retryCount % 3 == 0)
    {
      DEBUG_SERIAL.println("Re-pulsing PWRKEY...");
      powerOnModule();
    }
    if (retryCount >= MODULE_RETRY_MAX)
    {
      DEBUG_SERIAL.println("Retrying every 10 seconds...");
      retryCount = 0;
      lastRetryTime = millis() + 8000;
    }
  }
}

// ── Setup ────────────────────────────────────────────────────────
void setup()
{
  DEBUG_SERIAL.begin(115200);

  // Wait up to 3 seconds for Serial Monitor to connect.
  // If no computer is attached (e.g. USB power adapter), skip and run anyway.
  unsigned long serialWait = millis();
  while (!DEBUG_SERIAL && millis() - serialWait < 3000)
    delay(10);

  delay(500);

  MODULE_SERIAL.setFIFOSize(256);
  MODULE_SERIAL.begin(BAUD_RATE);

  DEBUG_SERIAL.println();
  DEBUG_SERIAL.println("=== A7680C GPS Tracker on Pi Pico ===");
  DEBUG_SERIAL.println();

  powerOnModule();

  DEBUG_SERIAL.println("Initialising A7680C ...");
  DEBUG_SERIAL.println();
}

// ── Loop ─────────────────────────────────────────────────────────
void loop()
{

  if (!moduleReady)
  {
    tryWakeModule();
    return;
  }

  if (millis() - lastPollTime >= POLL_INTERVAL)
  {
    lastPollTime = millis();
    attempt++;

    String raw = sendAT("AT+CGPSINFO");
    GpsData data = parseCgpsInfo(raw);

    if (data.valid)
    {
      fixCount++; // increment BEFORE onGpsData

      // SMS 3: first GPS fix acquired (one time only)
      if (!smsSentFix)
      {
        sendSMS(PHONE_NUMBER, "GPS Acquired, the device will now send GPS data to the website");
        smsSentFix = true;
      }

      onGpsData(data);

      // ── HTTP POST every HTTP_POST_INTERVAL ─────────────────────
      if (HTTP_ENABLED && millis() - lastHttpPostTime >= HTTP_POST_INTERVAL)
      {
        lastHttpPostTime = millis();

        if (!httpInitialized)
        {
          DEBUG_SERIAL.println("HTTP not initialized. Retrying initialization...");
          initHTTP();
        }

        if (httpInitialized)
        {
          String jsonBody = "{";
          jsonBody += "\"uid\":\"" + DEVICE_ID + "\",";
          jsonBody += "\"lat\":" + String(data.latitude, 6) + ",";
          jsonBody += "\"long\":" + String(data.longitude, 6) + ",";
          jsonBody += "\"speed\":" + String(data.speedKnots, 2);
          jsonBody += "}";

          httpPost(API_URL, jsonBody);
        }
        else
        {
          DEBUG_SERIAL.println("HTTP initialization failed. Skipping POST.");
          DEBUG_SERIAL.println("Check: 1) SIM has active data plan, 2) Network signal, 3) APN settings");
        }
      }
    }
    else
    {
      // Show satellite scan progress while waiting
      int sats = getSatelliteCount();

      // SMS 2: first satellite detected (one time only)
      if (!smsSentStart && sats > 0)
      {
        sendSMS(PHONE_NUMBER, "Found satellite, making signal accurate.");
        smsSentStart = true;
      }

      DEBUG_SERIAL.print("[");
      DEBUG_SERIAL.print(attempt);
      DEBUG_SERIAL.print("] Waiting for fix... ");
      if (sats >= 0)
      {
        DEBUG_SERIAL.print("Satellites: ");
        DEBUG_SERIAL.print(sats);
        DEBUG_SERIAL.print("  [");
        for (int i = 0; i < 12; i++)
          DEBUG_SERIAL.print(i < sats ? "#" : "-");
        DEBUG_SERIAL.print("]");
      }
      else
      {
        DEBUG_SERIAL.print("(querying satellites...)");
      }
      DEBUG_SERIAL.println();
    }
  }
}
