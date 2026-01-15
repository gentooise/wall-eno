#include <EthernetReset.h>

#include "wall-page.h"

// This server is used for remote control of the board through specific GET requests:
// - http://{ip}/{password}/reset (just restart the existing sketch)
// - http://{ip}/{password}/reprogram (invalidate existing sketch and reset board to wait for new upload)
// - http://{ip}/{password}/status (get the current wall-eno status in a friendly HTML page, including control values and error messages)
// - http://{ip}/{password}/raw-status (get the current wall-eno status as raw text, useful for command line)
// - http://{ip}/{password}/json-status (get the current wall-eno status as JSON object, useful for API request)
EthernetReset server(80, html_page);
EthernetClient client;

// Network settings
const IPAddress WALLBOX_IP(192, 168, 1, 200); // wallbox IP address, adjust as needed
const unsigned short WALLBOX_PORT = 502;

// Controller and sensor settings
const int SENSOR_PIN = A0; // analog pin which the hall sensor is attached into
const int SENSOR_MAX_CURRENT = 50; // the maximum current supported by the hall sensor
const short LOOP_TIME = 1000; // milliseconds
const short POLL_RATE = 6; // number of loops (wallbox minimum pollrate is 5s: setting 6s to play safe)
const short REFRESH_TIME = LOOP_TIME * POLL_RATE / 1000; // seconds (refresh status page at least every time a new current limit is set)

// Energy supply settings
const short MAX_POWER = 6000; // watt
const short VOLTAGE = 230; // actual voltage can range from 220V to 230V, assume 230V for better margin

// Buffer used for raw logging
#define LOG_BUF_SIZE 256
char logbuf[LOG_BUF_SIZE];

// Buffers used for status values and json
#define VALUE_COUNT 4
#define VALUE_SIZE 8
#define JSON_BUF_SIZE 256
char values[VALUE_COUNT][VALUE_SIZE];
char json[JSON_BUF_SIZE];


unsigned short sizeLeft(char* p, char* start, short size) {
    if (p < start || p > start + size) {
        reportFatal("Error: internal buffer pointer out of bounds");
    }
    return start + size - p;
}

char* vlog(char* p, char* start, short size, const char *fmt, va_list args) {
    int written = vsnprintf(p, sizeLeft(p, start, size), fmt, args);
    if (written < 0) {
        reportFatal("Error: log formatting failed");
    }
    return p + written;
}

char* log(char* p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    p = vlog(p, logbuf, LOG_BUF_SIZE, fmt, args);
    va_end(args);
    return p;
}

char* logj(char* p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    p = vlog(p, json, JSON_BUF_SIZE, fmt, args);
    va_end(args);
    return p;
}

char* logBuffer(char* p, byte* buf, unsigned short len) {
    for (unsigned short i = 0; i < len; i++) {
        p = log(p, "%02x ", buf[i]);
    }
    return p;
}

byte* putByte(byte* buf, byte val) {
    buf[0] = val;
    return buf + 1;
}

byte* putShort(byte* buf, unsigned short val) {
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
    return buf + 2;
}

byte* putBytes(byte* buf, byte* data, unsigned short len) {
    memcpy(buf, data, len);
    return buf + len;
}

void idle() {
    server.check(); // check for any remote requests
    delay(LOOP_TIME); // wait before next loop
}

void reportStatus(const char * msg, bool fatal) {
    server.resetRawAPIs();

    // Report wall-eno status as json (for HTML page)
    char* jsonp = json;
    jsonp = logj(jsonp,
        "{"
            "\"homePower\":%s,"
            "\"homeRaw\":%s,"
            "\"wallboxPower\":%s,"
            "\"wallboxCurrent\":%s,"
            "\"error\":\"%s\""
        "}",
        values[0],
        values[1],
        values[2],
        values[3],
        fatal ? msg : ""
    );
    server.addRawAPI("json-status", json);

    // Report wall-eno status as raw text
    server.addRawAPI("raw-status", msg);
}

void reportValues(short hm_raw, short hm_power, short wb_power, short wb_current) {
    // Store wall-eno values
    sprintf(values[0], "%hd.%hd", hm_power / 1000, ((hm_power % 1000) + 50) / 100); // float not supported, compute kW digits manually
    sprintf(values[1], "%hd", hm_raw);
    sprintf(values[2], "%4hd.%hd", wb_power / 1000, ((wb_power % 1000) + 50) / 100); // float not supported, compute kW digits manually
    sprintf(values[3], "%hd", wb_current);

    // Store wall-eno raw status
    char* logp = logbuf;
    logp = log(logp, "Home consumption: %skW (raw: %s)", values[0], values[1]);
    logp = log(logp, "\nWallbox limit: %skW (%sA)", values[2], values[3]);

    // Report full status (with values filled)
    reportStatus(logbuf, false);
}

void _report(char* msg, bool fatal) {
    Serial.println(msg); // uncomment if using serial output

    // Report empty values, just a message or error
    values[0][0] = '-'; values[0][1] = '\0';
    values[1][0] = '-'; values[1][1] = '\0';
    values[2][0] = '-'; values[2][1] = '\0';
    values[3][0] = '-'; values[3][1] = '\0';
    reportStatus(msg, fatal);
}

void report(char* msg) {
    _report(msg, false);
}

void reportFatal(char* msg) {
    client.stop(); // close connection to wallbox

    _report(msg, true);

    while (true) {
        idle();
    }
}

// Modbus TCP ADU packet structure
const unsigned short HEADER_SIZE = 8;
const unsigned short UNIT_ID = 2;
unsigned short tid = 0;
const short pid = 0;

byte buffer[32]; // buffer for request/response

const byte MODBUS_OK = 0;
const byte MODBUS_ERROR = 1;
const byte MODBUS_FATAL = 2;

static byte errors = 0;

byte countError() {
    // tolerate a certain number of non-fatal errors before giving up
    return (++errors > 10) ? MODBUS_FATAL : MODBUS_ERROR;
}
byte resetError() {
    errors = 0;
}

size_t modbusRequest(byte function, byte* data, unsigned short len) {
    // check buffer size
    unsigned short total_len = HEADER_SIZE + len;
    if (total_len > sizeof(buffer)) {
        reportFatal("Error: buffer size not enough for Modbus request");
    }

    // store request in buffer
    char* buf = buffer;
    buf = putShort(buf, tid);
    buf = putShort(buf, pid);
    buf = putShort(buf, len + 2); // length of unit id + function code + data
    buf = putByte(buf, UNIT_ID);
    buf = putByte(buf, function);
    buf = putBytes(buf, data, len);

    // dump request in ASCII hex values
    char* logp = logbuf;
    logp = log(logp, "Tx: ");
    logp = logBuffer(logp, buffer, total_len);

    // send request
    client.write(buffer, total_len);
    delay(100);

    // store response in buffer
    total_len = 0;
    while (client.available()) {
        if (total_len == sizeof(buffer)) {
            reportFatal("Error: buffer size not enough for Modbus response");
        }
        buffer[total_len++] = client.read();
    }

    // dump response in ASCII hex values
    logp = log(logp, "\nRx: ");
    logp = logBuffer(logp, buffer, total_len);

    byte error = MODBUS_OK;
    if (total_len <= HEADER_SIZE || total_len != 6 + buffer[5]) {
        logp = log(logp, "\nModbus protocol error");
        error = countError();
    } else {
        if (buffer[7] != function) { // error condition
            /* Error codes:
             * - 01 The received function code can not be processed.
             * - 02 The data address specified in the request is not available.
             * - 03 The value contained in the query data field is an invalid value.
             * - 04 An unrecoverable error occurred while the slave attempted to perform the requested action.
             * - 05 The slave has accepted the request and processes it, but it takes a long time. This response prevents the host from generating a timeout error.
             * - 06 The slave is busy processing the command. The master must repeat the message later when the slave is freed.
             * - 07 The slave can not execute the program function specified in the request. This code is returned for an unsuccessful program request using functions with numbers 13 or 14. The master must request diagnostic information or error information from the slave.
             * - 08 The slave detected a parity error when reading the extended memory. The master can repeat the request, but usually in such cases, repairs are required.
             */

            // error code is in the last byte received, i.e. buffer[8]
            if (buffer[8] == 2 || buffer[8] == 5 || buffer[8] == 6 || buffer[8] == 8) {
                error = countError();
            } else error = MODBUS_FATAL;

            logp = log(logp, "<- error code");
        }
    }

    switch (error) {
        default:
        case MODBUS_OK:
            resetError();
            break;
        case MODBUS_ERROR:
            report(logbuf);
            break;
        case MODBUS_FATAL:
            reportFatal(logbuf);
            break;
    }

    tid++; // increase transaction id

    return total_len; // response length
}

void setup() {
    Serial.begin(9600); // for local debug only
    server.begin(); // initialize Ethernet shield and start web server

    if (!client.connect(WALLBOX_IP, WALLBOX_PORT)) {
        reportFatal("Error: failed to connect to wallbox!");
    } else {
        report("Hello from wall-eno :)");

        // Example requests:
        // size_t len = modbusRequest(0x03, "\x00\x00\x00\x05", 4); // Manufacturer's name
        // size_t len = modbusRequest(0x03, "\x06\x61\x00\x01", 4); // Charging Current Limit
        // if (len > 0) {
        //   TODO get data from buffer
        // }
    }
}

void setWallboxLimit(byte current) {
    /* From wallbox modbus map:
     *   Charging Current Limit register: 0x0661
     *   Valid Write Value Range: 0 to 8000
     *   Values <= 100 are interpreted as A unit.
     *   Values > 100 are interpreted as 0.01A unit.
     */
    if (current > 32) current = 32; // max 32A
    short regValue = current * 100;
    byte data[] = { 0x06, 0x61, 0x00, 0x00 };
    putShort(data + 2, regValue);

    modbusRequest(0x06, data, sizeof(data));
}

short rawToWatt(short raw) {
    /* This function takes a raw value from hall sensor and returns the measured power, expressed in Watt.
     * The raw value of an analog pin is expected in range [0-1023]. Ideally it should be:
     * -    0 -> 0A measured
     * - 1023 -> 50A measured (max current supported by the sensor)
     * So this function should look like:
     *   if (raw < 0) raw = 0;
     *   if (raw > 1023) raw = 1023;
     *   float current = raw / 1023.0 * SENSOR_MAX_CURRENT;
     *   return current * VOLTAGE;
     * You can try using the above code, but the reality is often different due to
     * noise signal, voltage drop and crappy sensors :D, so you likely need a custom calibration.
     * The sensor can be characterized by collecting some raw values from direct readings on the pin
     * and then comparing them with the actual power absorbed at that moment (e.g. provided by a 3rd party like a home control system).
     * After collecting some valid <rawValue, power> pairs in a table, a linear function can be calculated
     * using curve fitting method (e.g. https://www.dcode.fr/function-equation-finder).
     * The same calculation can also be achieved by using FORECAST() function in a spreadsheet.
     * To avoid results out of the expected domain, the raw value is properly capped before conversion.
     */
    if (raw < 72) raw = 72;   // close to 0 kW
    if (raw > 659) raw = 659; // close to 6 kW (set proper value if you have maximum power different than 6 kW)
    return 10.2122 * raw - 731.228;
}

void loop() {
    idle();

    // Read raw value from hall sensor and convert to power
    short hm_raw = analogRead(SENSOR_PIN);
    short hm_power = rawToWatt(hm_raw);

    // Compute charging power and current limit
    short wb_power = MAX_POWER - hm_power - 50; // leave some margin (50W)
    short wb_current = wb_power / VOLTAGE; // assume 230V ()

    // Report wall-eno status
    reportValues(hm_raw, hm_power, wb_power, wb_current);

    // Set wallbox charging current limit at the given poll rate
    static byte loop_counter = 0;
    if (++loop_counter >= POLL_RATE) {
        setWallboxLimit(wb_current);
        loop_counter = 0;
    }
}
