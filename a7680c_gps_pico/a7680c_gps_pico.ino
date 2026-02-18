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
    - SMS on device start + first GPS fix
    - onGpsData() utility hook for future HTTP POST

  Serial Monitor baud: 115200
*/

// ── Configuration ───────────────────────────────────────────────
#define MODULE_SERIAL Serial1
#define DEBUG_SERIAL Serial
#define BAUD_RATE 115200
#define PWRKEY_PIN 2
#define READ_TIMEOUT 10000
#define POLL_INTERVAL 5000
#define MODULE_RETRY_MAX 10
#define MODULE_RETRY_DELAY 2000

// ── SMS Configuration ────────────────────────────────────────────
#define SMS_ENABLED true
const String PHONE_NUMBER = "+639XXXXXXXXX"; // ← SEND TO NUMBER +639XXXXXXXXX
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
bool smsSentBoot = false;  // SMS 1: sent on module alive
bool smsSentStart = false; // SMS 2: sent on first satellite detected
bool smsSentFix = false;   // SMS 3: sent on first GPS fix
int attempt = 0;
long lastPollTime = -(long)POLL_INTERVAL;

// ═══════════════════════════════════════════════════════════════
//  UTILITY — called every time a valid GPS fix is received
//  Add your HTTP POST / MQTT / LoRa logic here in the future
// ═══════════════════════════════════════════════════════════════
void onGpsData(const GpsData &gps)
{
  // ── Print to Serial Monitor ────────────────────────────────────
  DEBUG_SERIAL.println("----------------------------------------");
  DEBUG_SERIAL.print("Fix #");
  DEBUG_SERIAL.println(attempt);
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
  DEBUG_SERIAL.println("----------------------------------------");

  // ── TODO: HTTP POST ───────────────────────────────────────────
  // Uncomment and fill in once your server is ready:
  //
  // String payload = "{";
  // payload += "\"lat\":"  + String(gps.latitude,  6) + ",";
  // payload += "\"lon\":"  + String(gps.longitude, 6) + ",";
  // payload += "\"alt\":"  + String(gps.altitude,  1) + ",";
  // payload += "\"spd\":"  + String(gps.speedKnots,2) + ",";
  // payload += "\"date\":\"" + gps.date    + "\",";
  // payload += "\"time\":\"" + gps.utcTime + "\"";
  // payload += "}";
  // httpPost("https://your-api.com/location", payload);

  // ── TODO: MQTT publish ────────────────────────────────────────
  // mqttPublish("tracker/location", payload);
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

  // Set SMS text mode
  String r = sendAT("AT+CMGF=1", 5000);
  if (r.indexOf("OK") == -1)
  {
    DEBUG_SERIAL.println("SMS ERROR: Could not set text mode.");
    return false;
  }

  // Set recipient — module responds with "> " prompt
  MODULE_SERIAL.println("AT+CMGS=\"" + number + "\"");
  delay(2000);

  // Send message body followed by Ctrl+Z (char 26) to submit
  MODULE_SERIAL.print(message);
  MODULE_SERIAL.write(26);

  // Wait for +CMGS response (up to 10s)
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

// ── Parse AT+CGPSINFO ────────────────────────────────────────────
// Format: +CGPSINFO:<lat>,<N/S>,<lon>,<E/W>,<date>,<UTC>,<alt>,<speed>,<course>
// No fix: +CGPSINFO:,,,,,,,,
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
  int fieldCount = 0;
  int prev = 0;
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

  // ddmm.mmmmmm → decimal degrees
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

// ── Parse satellite count from AT+CGNSSINFO ──────────────────────
// Returns -1 if unavailable
int getSatelliteCount()
{
  // Use AT+CGPSINFO which we know works, and infer satellite status from fix mode.
  // AT+CGNSSINFO returns all empty fields when there's no positional fix yet,
  // even if the module is tracking satellites — so we use a separate NMEA query.
  // AT+CGNSSINFO still gives us fix mode + sat count even before lat/lon is resolved
  // as long as we read the first two fields before the response fills with commas.
  String raw = sendAT("AT+CGNSSINFO");
  int idx = raw.indexOf("+CGNSSINFO:");
  if (idx == -1)
    return -1;

  String payload = raw.substring(idx + 11);
  payload.trim();

  // Split all fields so we can handle both partial and full responses
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

  // Field 0 = fix mode (2=2D, 3=3D, empty=no fix)
  // Field 1 = GPS satellites
  // If fix mode is empty, module is still scanning — return 0
  if (fields[0].length() == 0)
    return 0;
  if (fields[1].length() == 0)
    return 0;
  return fields[1].toInt();
}

// ── Format helpers ───────────────────────────────────────────────
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
  }
  else
  {
    DEBUG_SERIAL.println("No response.");
    if (retryCount % 3 == 0)
    {
      DEBUG_SERIAL.println("Re-pulsing PWRKEY...");
      //powerOnModule();
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
  while (!DEBUG_SERIAL)
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
      // SMS 3: first GPS fix acquired
      if (!smsSentFix)
      {
        sendSMS(PHONE_NUMBER, "GPS Acquired, the device will now send GPS data to the website");
        smsSentFix = true;
      }

      onGpsData(data); // ← all GPS data goes through here
    }
    else
    {
      // Show satellite scan progress while waiting
      int sats = getSatelliteCount();

      // SMS 2: first satellite detected
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
        // Simple visual signal bar
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
