#include <Ethernet.h>
#include <EthernetReset.h>

// This server is used for remote control of the board through specific GET requests:
// - http://{ip}/{password}/reset (just restart the existing sketch)
// - http://{ip}/{password}/reprogram (invalidate existing sketch and reset board to wait for new upload)
// - http://{ip}/{password}/status (get the latest status/error message reported by wall-eno)
EthernetReset server(80);
EthernetClient client;

// Network settings
const IPAddress WALLBOX_IP(192, 168, 1, 200); // wallbox IP address, adjust as needed
const unsigned short WALLBOX_PORT = 502;

// Controller and sensor settings
const int SENSOR_PIN = A0; // analog pin which the hall sensor is attached into
const int SENSOR_MAX_CURRENT = 50; // the maximum current supported by the hall sensor
const int LOOP_TIME = 1000; // milliseconds
const int POLL_RATE = 6; // number of loops (wallbox minimum pollrate is 5s: setting 6s to play safe)
const int REFRESH_TIME = LOOP_TIME * POLL_RATE / 1000; // seconds (refresh status page at least every time a new current limit is set)

// Energy supply settings
const short MAX_POWER = 6000; // watt
const short VOLTAGE = 230; // actual voltage can range from 220V to 230V, assume 230V for better margin

void idle() {
    server.check(); // check for any remote requests
    delay(LOOP_TIME); // wait before next loop
}

void report(char* msg) {
    server.status(msg);
    // Serial.println(msg); uncomment if using serial output
}

void reportFatal(char* msg) {
    server.refresh(0); // reset server status page refresh interval (no refresh)
    client.stop(); // close connection to wallbox

    report(msg);

    while (true) {
        idle();
    }
}

const unsigned short BUF_SIZE = 256;
char logbuf[BUF_SIZE]; // buffer used for logging

unsigned short sizeLeft(char* p) {
    if (p < logbuf || p > logbuf + BUF_SIZE) {
        reportFatal("buffer pointer error");
    }
    return logbuf + BUF_SIZE - p;
}

char* log(char* p, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(p, sizeLeft(p), fmt, args);
    va_end(args);

    if (written < 0) {
        reportFatal("snprintf error");
    }
    return p + written;
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
        reportFatal("error: buffer size not enough for request");
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
            reportFatal("error: buffer size not enough for response");
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
    server.refresh(REFRESH_TIME); // setup server status page refresh interval (in seconds)

    if (!client.connect(WALLBOX_IP, WALLBOX_PORT)) {
        reportFatal("Failed to connect to wallbox!");
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

    char* logp = logbuf;

    // Read raw value from hall sensor and convert to power
    short raw = analogRead(SENSOR_PIN);
    short hm_power = rawToWatt(raw);
    logp = log(logp, "Home consumption: %hd.%hdkW (raw: %hd)", hm_power / 1000, ((hm_power % 1000) + 50) / 100, raw); // float not supported, compute kW digits manually

    // Compute charging power and current limit
    short wb_power = MAX_POWER - hm_power - 50; // leave some margin (50W)
    short wb_current = wb_power / VOLTAGE; // assume 230V ()
    logp = log(logp, "\nWallbox limit: %4hd.%hdkW (%hdA)", wb_power / 1000, ((wb_power % 1000) + 50) / 100, wb_current);

    // Report wall-eno status
    report(logbuf);

    // Set wallbox charging current limit at the given poll rate
    static byte loop_counter = 0;
    if (++loop_counter >= POLL_RATE) {
        setWallboxLimit(wb_current);
        loop_counter = 0;
    }
}
