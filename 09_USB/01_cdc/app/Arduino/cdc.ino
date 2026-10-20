/*
 * Arduino USB CDC Echo Responder
 *
 * Commands:
 *
 * [writeread]Hello
 *   -> Echo "Hello" once.
 *
 * [write]Hello
 *   -> Receive "Hello", but do not reply.
 *
 * [read]Hello
 *   -> Wait 1 second, then echo "[00th]Hello" every 1 second.
 *      Index increments from 00 to 99, then wraps around to 00.
 *      Repeating stops when another command is received.
 */

String inputString = "";
String repeatMessage = "";

bool repeatEnabled = false;

unsigned long nextRepeatTime = 0;
unsigned int readIndex = 0;

const unsigned long REPEAT_INTERVAL_MS = 1000;
const size_t MAX_LINE_LENGTH = 512;

int led = 1;

// --------------------------------------------------
// Setup
// --------------------------------------------------
void setup()
{
    Serial.begin(115200);

    pinMode(7, OUTPUT);
    digitalWrite(7, led);

    // Useful for native USB boards such as Leonardo.
    // Remove this loop if the board must operate headlessly.
    while (!Serial) {
        ;
    }

    inputString.reserve(200);
    repeatMessage.reserve(200);
}

// --------------------------------------------------
// LED
// --------------------------------------------------
void led_changed()
{
    led = 1 - led;
    digitalWrite(7, led);
}

// --------------------------------------------------
// Process one complete command
// --------------------------------------------------
void processCommand(String command)
{
    // A new command cancels the previous repeating response.
    repeatEnabled = false;
    repeatMessage = "";

    // [writeread] -> Echo once.
    if (command.startsWith("[writeread]")) {

        String message = command.substring(11);

        delay(50);
        Serial.println(message);

        led_changed();
    }

    // [write] -> Receive only, no response.
    else if (command.startsWith("[write]")) {

        // Message is received, but no response is sent.
        led_changed();
    }

    // [read] -> Start repeating the indexed response.
    else if (command.startsWith("[read]")) {

        repeatMessage = command.substring(6);

        // Start counting from 00 for every new read command.
        readIndex = 0;

        repeatEnabled = true;

        // First response is sent after one second.
        nextRepeatTime = millis() + REPEAT_INTERVAL_MS;

        led_changed();
    }

    // Unknown prefix: ignore the command.
}

// --------------------------------------------------
// Read incoming serial data
// --------------------------------------------------
void receiveSerialData()
{
    while (Serial.available() > 0) {

        char inChar = (char)Serial.read();

        // Ignore carriage return for CRLF compatibility.
        if (inChar == '\r') {
            continue;
        }

        // A newline terminates the command.
        if (inChar == '\n') {

            processCommand(inputString);

            inputString = "";

            continue;
        }

        // Protect against excessively long input lines.
        if (inputString.length() < MAX_LINE_LENGTH) {
            inputString += inChar;
        }
    }
}

// --------------------------------------------------
// Send a repeating indexed response
// --------------------------------------------------
void handleRepeatingResponse()
{
    if (!repeatEnabled) {
        return;
    }

    unsigned long currentTime = millis();

    // Handle millis() rollover safely.
    if ((long)(currentTime - nextRepeatTime) >= 0) {

        // Format: [00th]Hello ... [99th]Hello
        char prefix[8];

        snprintf(prefix, sizeof(prefix), "[%02uth]", readIndex);

        Serial.print(prefix);
        Serial.println(repeatMessage);

        // Increment index and wrap around after 99.
        readIndex = (readIndex + 1) % 100;

        nextRepeatTime = currentTime + REPEAT_INTERVAL_MS;
    }
}

// --------------------------------------------------
// Main loop
// --------------------------------------------------
void loop()
{
    receiveSerialData();

    handleRepeatingResponse();
}
