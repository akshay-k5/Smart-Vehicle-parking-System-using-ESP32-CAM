//```cpp
// Arduino Nano code to read 3 IR sensors and send statuses to ESP32-CAM via UART
// Prints detailed status and UART communication to Serial Monitor for debugging

#define SENSOR1_PIN 2 // IR sensor for Lot 1
#define SENSOR2_PIN 3 // IR sensor for Lot 2
#define SENSOR3_PIN 4 // IR sensor for Lot 3

unsigned long messageCount = 0; // Counter for UART messages

void setup() {
  Serial.begin(115200); // Initialize Serial Monitor at 115200 baud
  pinMode(SENSOR1_PIN, INPUT_PULLUP); // Lot 1 sensor with internal pull-up
  pinMode(SENSOR2_PIN, INPUT_PULLUP); // Lot 2 sensor with internal pull-up
  pinMode(SENSOR3_PIN, INPUT_PULLUP); // Lot 3 sensor with internal pull-up
  Serial.println("Nano started. Reading IR sensors...");
  Serial.flush(); // Ensure initial message is sent
}

void loop() {
  // Read sensor states (LOW = occupied, HIGH = available)
  int lot1 = digitalRead(SENSOR1_PIN) == LOW ? 1 : 0;
  int lot2 = digitalRead(SENSOR2_PIN) == LOW ? 1 : 0;
  int lot3 = digitalRead(SENSOR3_PIN) == LOW ? 1 : 0;

  // Print detailed status to Serial Monitor
  Serial.println("--- Message " + String(++messageCount) + " ---");
  Serial.print("Lot 1: ");
  Serial.println(lot1 == 1 ? "Occupied" : "Available");
  Serial.print("Lot 2: ");
  Serial.println(lot2 == 1 ? "Occupied" : "Available");
  Serial.print("Lot 3: ");
  Serial.println(lot3 == 1 ? "Occupied" : "Available");

  // Prepare and send UART data to ESP32-CAM
  String uartData = String(lot1) + "," + String(lot2) + "," + String(lot3);
  Serial.print("Sending to ESP32-CAM: ");
  Serial.println(uartData);
  Serial.println(uartData); // Send to ESP32-CAM via UART
  Serial.flush(); // Ensure all messages are sent

  delay(1000); // Update every second
}
