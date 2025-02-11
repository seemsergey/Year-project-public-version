#include <SoftwareSerial.h>
#include <FPM.h>
#include "SPIFFS.h"
#include <WiFi.h>
#include <dummy.h>
#include <SPI.h>
#include <MFRC522.h>
#include <AmperkaKB.h>
#include <NTPClient.h>
#include <ESP32Servo.h> 
#include <FastBot.h> 
#include <PubSubClient.h>


#define WIFI_PASSWORD "***"
#define WIFI_NETWORK "***"
#define BOT_API_TOKEN "***"
#define CHAT_IDENTIFIER "***"
FastBot bot(BOT_API_TOKEN);

const char* mqttServer = "***";
const int mqttPort = ***;
const char* mqttUser = "***";
const char* mqttPassword = "***";

const int servoPin = 4;
const int hallSensorPin = 2;
const int keypadDelay = 5000;
unsigned long lastKeyPressTime = 0;
String currentPassword = "";
int click_times = 0;

String passwords[100];
int passwordCount = 0;
String nfcIDs[100];


struct PasswordEntry {
  int year;
  int month;
  int day;
  int hour;
  String password;
  //unsigned long timestamp;
};


PasswordEntry Entrypasswords[100];


Servo MG995_Servo;
MFRC522 rfid(5, 32);
MFRC522::MIFARE_Key key; 


AmperkaKB KB(13, 12, 14, 27, 25, 26, 15, 33);
SoftwareSerial fserial(17, 16);
FPM finger(&fserial);
FPM_System_Params params;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
WiFiClient espClient;
PubSubClient client(espClient);

String expectedInput = "";


void setup() {
  SPIFFS.begin(true);
  SPI.begin();
  rfid.PCD_Init();
  KB.begin(KB4x4);

  Serial.begin(115200);
  fserial.begin(57600);

  if (finger.begin()) {
        finger.readParams(&params);
        Serial.println("Found fingerprint sensor!");
        Serial.print("Capacity: "); Serial.println(params.capacity);
        Serial.print("Packet length: "); Serial.println(FPM::packet_lengths[params.packet_len]);
    } else {
        Serial.println("Did not find fingerprint sensor :(");
        while (1) yield();
    }

  pinMode(hallSensorPin, INPUT);

  connectWiFi();

  readPasswords();
  readTimePasswords();
  readNfcIDs();

  timeClient.begin();
  timeClient.setTimeOffset(25200);

  bot.setChatID(CHAT_IDENTIFIER);
  bot.attach(newMsg);
  bot.showMenu("Open \t Add new password \t Logs");


  MG995_Servo.setPeriodHertz(50);
  MG995_Servo.attach(servoPin, 500, 2400);
  
  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
  
  while (!client.connected()) {
    Serial.println("Connecting to MQTT...");
 
    if (client.connect("ESP32Client", mqttUser, mqttPassword )) {
 
      Serial.println("connected");  
 
    } else {
 
      Serial.print("failed with state ");
      Serial.print(client.state());
      delay(2000);
    }
  }
 
  client.subscribe("UID_create");
  client.subscribe("enroll_function");
  client.subscribe("pass_create");
  client.subscribe("door_opener");
}

void loop() {
  bot.tick();
  client.loop();
  KB.read();

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String nfcUID = "";
    for (byte i = 0; i < 4; i++) {
      nfcUID += String(rfid.uid.uidByte[i], DEC);
    }
    Serial.println(nfcUID);
    if (checkNFCUID(nfcUID)) {
      unlockDoor();
    }
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }

  if (KB.justReleased()) {
    char key = KB.getChar;
    clearPasswordIfTimeout();

    if (key == '#') {
      checkFingerprint();}
    else if (key == '*') {
      currentPassword = "";
      click_times = 0;
    } else {
    Serial.println(key);
      click_times++;
      currentPassword += key;
      Serial.println(currentPassword);
      if (click_times == 4) {
        if (checkPassword(currentPassword)) {
          unlockDoor();
          logEvent("Keypad unlocked door", "INF");
        }
        currentPassword = "";
        click_times = 0;
      }
    }

    lastKeyPressTime = millis();
  }
  MG995_Servo.write(1);
}

void callback(String topic, byte* payload, unsigned int length) { 
  Serial.print("Message arrived in topic: ");
  Serial.println(topic);
  Serial.print("Message:");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
 
  Serial.println();
 
  if (topic == "door_opener") {
    unlockDoor();
  }
  else if (topic == "enroll_function") {
    int16_t fid;
    if (get_free_id(&fid))
        enroll_finger(fid);
    else
        Serial.println("No free slot in flash library!");
  }
  else if (topic == "pass_create") 
    addNewPassword();
  else if (topic == "UID_create")
    addNewNfcTag();
}

bool checkNFCUID(String uid) {
  for (int i = 0; i < 100; i++) {
    if (uid == nfcIDs[i]) {
      logEvent("NFC_ID unlocked door", "INF");
      return true;
    }
  }
  return false;
}

int checkFingerprint(void)  {
    int16_t p = -1;

    Serial.println("Waiting for valid finger");
    while (p != FPM_OK) {
        p = finger.getImage();
        switch (p) {
            case FPM_OK:
                Serial.println("Image taken");
                break;
            case FPM_NOFINGER:
                Serial.println(".");
                break;
            case FPM_PACKETRECIEVEERR:
                Serial.println("Communication error");
                break;
            case FPM_IMAGEFAIL:
                Serial.println("Imaging error");
                break;
            case FPM_TIMEOUT:
                Serial.println("Timeout!");
                break;
            case FPM_READ_ERROR:
                Serial.println("Got wrong PID or length!");
                break;
            default:
                Serial.println("Unknown error");
                break;
        }
        yield();
    }

    p = finger.image2Tz();
    switch (p) {
        case FPM_OK:
            Serial.println("Image converted");
            break;
        case FPM_IMAGEMESS:
            Serial.println("Image too messy");
            return p;
        case FPM_PACKETRECIEVEERR:
            Serial.println("Communication error");
            return p;
        case FPM_FEATUREFAIL:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_INVALIDIMAGE:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_TIMEOUT:
            Serial.println("Timeout!");
            return p;
        case FPM_READ_ERROR:
            Serial.println("Got wrong PID or length!");
            return p;
        default:
            Serial.println("Unknown error");
            return p;
    }

    uint16_t fid, score;
    p = finger.searchDatabase(&fid, &score);
    Serial.println("Remove finger");
    while (finger.getImage() != FPM_NOFINGER) {
        delay(500);
    }
    Serial.println();
    
    if (p == FPM_OK) {
        Serial.println("Found a print match!");
        unlockDoor();
    } else if (p == FPM_PACKETRECIEVEERR) {
        Serial.println("Communication error");
        return p;
    } else if (p == FPM_NOTFOUND) {
        Serial.println("Did not find a match");
        return p;
    } else if (p == FPM_TIMEOUT) {
        Serial.println("Timeout!");
        return p;
    } else if (p == FPM_READ_ERROR) {
        Serial.println("Got wrong PID or length!");
        return p;
    } else {
        Serial.println("Unknown error");
        return p;
    }

    Serial.print("Found ID #"); Serial.print(fid);
    Serial.print(" with confidence of "); Serial.println(score);
}

int16_t enroll_finger(int16_t fid) {
    int16_t p = -1;
    Serial.println("Waiting for valid finger to enroll");
    while (p != FPM_OK) {
        p = finger.getImage();
        switch (p) {
            case FPM_OK:
                Serial.println("Image taken");
                break;
            case FPM_NOFINGER:
                Serial.println(".");
                break;
            case FPM_PACKETRECIEVEERR:
                Serial.println("Communication error");
                break;
            case FPM_IMAGEFAIL:
                Serial.println("Imaging error");
                break;
            case FPM_TIMEOUT:
                Serial.println("Timeout!");
                break;
            case FPM_READ_ERROR:
                Serial.println("Got wrong PID or length!");
                break;
            default:
                Serial.println("Unknown error");
                break;
        }
        yield();
    }

    p = finger.image2Tz(1);
    switch (p) {
        case FPM_OK:
            Serial.println("Image converted");
            break;
        case FPM_IMAGEMESS:
            Serial.println("Image too messy");
            return p;
        case FPM_PACKETRECIEVEERR:
            Serial.println("Communication error");
            return p;
        case FPM_FEATUREFAIL:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_INVALIDIMAGE:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_TIMEOUT:
            Serial.println("Timeout!");
            return p;
        case FPM_READ_ERROR:
            Serial.println("Got wrong PID or length!");
            return p;
        default:
            Serial.println("Unknown error");
            return p;
    }

    Serial.println("Remove finger");
    delay(2000);
    p = 0;
    while (p != FPM_NOFINGER) {
        p = finger.getImage();
        yield();
    }

    p = -1;
    Serial.println("Place same finger again");
    while (p != FPM_OK) {
        p = finger.getImage();
        switch (p) {
            case FPM_OK:
                Serial.println("Image taken");
                break;
            case FPM_NOFINGER:
                Serial.print(".");
                break;
            case FPM_PACKETRECIEVEERR:
                Serial.println("Communication error");
                break;
            case FPM_IMAGEFAIL:
                Serial.println("Imaging error");
                break;
            case FPM_TIMEOUT:
                Serial.println("Timeout!");
                break;
            case FPM_READ_ERROR:
                Serial.println("Got wrong PID or length!");
                break;
            default:
                Serial.println("Unknown error");
                break;
        }
        yield();
    }

    p = finger.image2Tz(2);
    switch (p) {
        case FPM_OK:
            Serial.println("Image converted");
            break;
        case FPM_IMAGEMESS:
            Serial.println("Image too messy");
            return p;
        case FPM_PACKETRECIEVEERR:
            Serial.println("Communication error");
            return p;
        case FPM_FEATUREFAIL:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_INVALIDIMAGE:
            Serial.println("Could not find fingerprint features");
            return p;
        case FPM_TIMEOUT:
            Serial.println("Timeout!");
            return false;
        case FPM_READ_ERROR:
            Serial.println("Got wrong PID or length!");
            return false;
        default:
            Serial.println("Unknown error");
            return p;
    }

    p = finger.createModel();
    if (p == FPM_OK) {
        Serial.println("Prints matched!");
    } else if (p == FPM_PACKETRECIEVEERR) {
        Serial.println("Communication error");
        return p;
    } else if (p == FPM_ENROLLMISMATCH) {
        Serial.println("Fingerprints did not match");
        return p;
    } else if (p == FPM_TIMEOUT) {
        Serial.println("Timeout!");
        return p;
    } else if (p == FPM_READ_ERROR) {
        Serial.println("Got wrong PID or length!");
        return p;
    } else {
        Serial.println("Unknown error");
        return p;
    }

    Serial.print("ID "); Serial.println(fid);
    p = finger.storeModel(fid);
    if (p == FPM_OK) {
        Serial.println("Stored!");
        return 0;
    } else if (p == FPM_PACKETRECIEVEERR) {
        Serial.println("Communication error");
        return p;
    } else if (p == FPM_BADLOCATION) {
        Serial.println("Could not store in that location");
        return p;
    } else if (p == FPM_FLASHERR) {
        Serial.println("Error writing to flash");
        return p;
    } else if (p == FPM_TIMEOUT) {
        Serial.println("Timeout!");
        return p;
    } else if (p == FPM_READ_ERROR) {
        Serial.println("Got wrong PID or length!");
        return p;
    } else {
        Serial.println("Unknown error");
        return p;
    }
}

bool get_free_id(int16_t * fid) {
    int16_t p = -1;
    for (int page = 0; page < (params.capacity / FPM_TEMPLATES_PER_PAGE) + 1; page++) {
        p = finger.getFreeIndex(page, fid);
        switch (p) {
            case FPM_OK:
                if (*fid != FPM_NOFREEINDEX) {
                    Serial.print("Free slot at ID ");
                    Serial.println(*fid);
                    return true;
                }
                break;
            case FPM_PACKETRECIEVEERR:
                Serial.println("Communication error!");
                return false;
            case FPM_TIMEOUT:
                Serial.println("Timeout!");
                return false;
            case FPM_READ_ERROR:
                Serial.println("Got wrong PID or length!");
                return false;
            default:
                Serial.println("Unknown error!");
                return false;
        }
        yield();
    }
    
    Serial.println("No free slots!");
    return false;
}

bool checkPassword(String password) {
  click_times = 0;
  currentPassword = "";
  bool passwordFound = false;

  for (int i = 0; i < 100; i++) {
    if (password.equals(passwords[i])) {
      passwordFound = true;
      break;
    }
  }

  for (int i = 0; i < 100; i++) {
    Serial.print("Checking password: ");
    Serial.println(Entrypasswords[i].password);
    
    if (password.equals(Entrypasswords[i].password)) {
      Serial.println("Password found: " + Entrypasswords[i].password);
      passwordFound = true;
      break;
    }
  }

  Serial.print("Entered password: ");
  Serial.println(password);
  Serial.println(passwordFound);
  return passwordFound;
}

void unlockDoor() {
  Serial.println("Door opened");
  MG995_Servo.write(89);
  logEvent("Door unlocked", "INF");
  delay(250);
  lockDoor();

}

void lockDoor() {
  Serial.println("Door locked");

  MG995_Servo.write(1);
  logEvent("Door locked", "INF");
}

void readPasswords() {
  File file = SPIFFS.open("/passwords.txt", "r");
  if (!file) {
    logEvent("Failed to open passwords.txt", "ERROR");
    return;
  }

  int index = 0;
  while (file.available()) {
    String password = file.readStringUntil('\n');
    password.trim();
    passwords[index] = password;
    index++;
  }

  file.close();

  for (int i = 0; i < index; i++) {
    Serial.println(passwords[i]);
  }
}

void readTimePasswords() {
    File passwordFile = SPIFFS.open("/Time_password.txt", "r");
    if (!passwordFile) {
        Serial.println("Ошибка открытия файла /Time_password.txt");
        return;
    }

    passwordCount = 0;
    String updatedPasswords = ""; 
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = gmtime(&epochTime);
    time_t currentTime = mktime(ptm);

    Serial.println("Проверка паролей...");

    while (passwordFile.available() && passwordCount < 100) {
        //разбиение строки файла на составные части(дату и пароль)
        String passwordLine = passwordFile.readStringUntil('\n');
        PasswordEntry entry;
        entry.year = passwordLine.substring(0, 4).toInt();
        entry.month = passwordLine.substring(5, 7).toInt();
        entry.day = passwordLine.substring(8, 10).toInt();
        entry.hour = passwordLine.substring(11, 13).toInt();
        entry.password = passwordLine.substring(14);

        //перевод UNIX-время для сравнения
        struct tm passwordTm = {0};
        passwordTm.tm_year = entry.year - 1900;
        passwordTm.tm_mon = entry.month - 1;
        passwordTm.tm_mday = entry.day;
        passwordTm.tm_hour = entry.hour;
        time_t passwordTime = mktime(&passwordTm);

        //проверка актуальности
        if (currentTime > passwordTime) {
            Serial.print("Удален просроченный пароль: ");
            Serial.println(entry.password);
        }
        else {
            updatedPasswords += passwordLine + "\n";
            Entrypasswords[passwordCount++] = entry;
        }
    }
    passwordFile.close();

    if (SPIFFS.exists("/Time_password.txt")) {
        SPIFFS.remove("/Time_password.txt");
    }

    File updatedFile = SPIFFS.open("/Time_password.txt", "w");
    if (updatedFile) {
        updatedFile.print(updatedPasswords);
        updatedFile.close();
        Serial.println("Файл обновлен: устаревшие пароли удалены.");
    }
    else {
        Serial.println("Ошибка обновления файла паролей.");
    }

    Serial.println("Загружены актуальные пароли:");
    for (int i = 0; i < passwordCount; i++) {
        Serial.print("Пароль ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.println(Entrypasswords[i].password);
    }
}

void clearPasswordIfTimeout() {
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = gmtime(&epochTime);
    time_t currentTime = mktime(ptm);

    int i = 0;
    while (i < passwordCount) {
        struct tm passwordTm = {0};
        passwordTm.tm_year = Entrypasswords[i].year - 1900;
        passwordTm.tm_mon = Entrypasswords[i].month - 1;
        passwordTm.tm_mday = Entrypasswords[i].day;
        passwordTm.tm_hour = Entrypasswords[i].hour;
        time_t passwordTime = mktime(&passwordTm);

        if (currentTime > passwordTime) {
            Serial.print("Удален просроченный пароль (из памяти): ");
            Serial.println(Entrypasswords[i].password);
            Entrypasswords[i] = Entrypasswords[--passwordCount];
        }
        else {
            i++;
        }
    }
}

void readNfcIDs() {
  File file = SPIFFS.open("/nfc_ids.txt", "r");
  if (!file) {
    logEvent("Failed to open nfc_ids.txt", "ERROR");
    return;
  }

  int index = 0;
  while (file.available() && index < 100) {
    String nfcID = file.readStringUntil('\n');
    nfcID.trim();
    nfcIDs[index] = nfcID;
    index++;
  }

  file.close();

  for (int i = 0; i < index; i++) {
    Serial.println(nfcIDs[i]);
  }
}

void addNewNfcTag() {
  //Serial.println("addNewNfcTag");
  while (true){
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String newTagUID = "";
      for (byte i = 0; i < 4; i++) {
        newTagUID += String(rfid.uid.uidByte[i], DEC);
      }
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      
      bool tagExists = false;
      for (int i = 0; i < 100; i++) {
        if (newTagUID == nfcIDs[i]) {
          tagExists = true;
          break;
        }
      }

      if (!tagExists) {
        File nfcFile = SPIFFS.open("/nfc_ids.txt", "a");
        if (nfcFile) {
          nfcFile.println(newTagUID);
          nfcFile.close();
          for (int i = 0; i < 100; i++) {
            if (nfcIDs[i] == "") {
              nfcIDs[i] = newTagUID;
              break;
            }
          }
          Serial.println("New NFC tag added and updated successfully.");
          break;
        }
        else {
          Serial.println("Error opening NFC tag file for writing.");
          break;

        }
      }
      else {
        Serial.println("NFC tag already exists in the list.");
        break;

      }
    }
  }
}

void addNewPassword() {
  //Serial.println("addNewPassword");
  int indx = 0;
  String newPassword = "";
  while(indx != 4) {
    KB.read();

    if (KB.justReleased()) {
      char key = KB.getChar;
      newPassword += key;
      indx++;
    }
  }

  bool passExists = false;
  for (int i = 0; i < 100; i++) {
    if (newPassword == passwords[i]) {
      passExists = true;
      break;
    }
  }

  if (!passExists) {
    File passFile = SPIFFS.open("/passwords.txt", "a");
    if (passFile) {
      passFile.println(newPassword);
      passFile.close();
      for (int i = 0; i < 100; i++) {
        if (passwords[i] == "") {
          passwords[i] = newPassword;
          break;
        }
      }
      Serial.println("New password added and updated successfully.");
    }
    else {
      Serial.println("Error opening password file for writing.");
    }
  }
  else {
    Serial.println("password already exists in the list.");
  }
}

void logEvent(String message, String type) {
  File eventLogFile = SPIFFS.open("/event_log.txt", "a");
  if (!eventLogFile) {
    Serial.println("Failed to open event_log.txt for writing");
  }
  else{
    String logEntry = "[" + type + "] " + getTime() + " " + message + "\n";
    Serial.println(logEntry);
    eventLogFile.println(logEntry);
  }
  eventLogFile.close();
}

String getTime() {  
  timeClient.update();
  return timeClient.getFormattedTime();
}

void connectWiFi() {
  delay(2000);
  Serial.begin(115200);
  Serial.println();

  WiFi.begin(WIFI_NETWORK, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() > 15000) ESP.restart();
  }
  Serial.println("Connected");
}

void newMsg(FB_msg& msg) {

  if (expectedInput != ""){
    if (expectedInput == "temporary_password") {
      Serial.println("Processing temporary password...");
      if (msg.text.length() > 10) {
        time_t epochTime = timeClient.getEpochTime();
        struct tm *ptm = gmtime(&epochTime);
        time_t currentTime = mktime(ptm);

        String passwordLine = msg.text;
        PasswordEntry entry;
        entry.year = passwordLine.substring(0, 4).toInt();
        entry.month = passwordLine.substring(5, 7).toInt();
        entry.day = passwordLine.substring(8, 10).toInt();
        entry.hour = passwordLine.substring(11, 13).toInt();
        entry.password = passwordLine.substring(14);

        struct tm passwordTm = {0};
        passwordTm.tm_year = entry.year - 1900;
        passwordTm.tm_mon = entry.month - 1;
        passwordTm.tm_mday = entry.day;
        passwordTm.tm_hour = entry.hour;
        time_t passwordTime = mktime(&passwordTm);
        if (currentTime > passwordTime) {
          bot.sendMessage("Временный пароль устарел.");
        }
        else {
          File passwordFile = SPIFFS.open("/Time_password.txt", "a");
          if (passwordFile) {
            passwordFile.println(msg.text);
            passwordFile.close();
            bot.sendMessage("Временный пароль сохранен.");
          }
          else {
            bot.sendMessage("Ошибка сохранения пароля.");
          }
          }
        }
      else {
          bot.sendMessage("Некорректный формат. Попробуйте снова.");
        }
      expectedInput = "";
      return;
    }
    else if (expectedInput == "permanent_password") {
      Serial.println("Processing permanent password...");
      if (msg.text.length() == 4 && msg.text.toInt() != 0) {
        File passFile = SPIFFS.open("/passwords.txt", "a");
        if (passFile) {
            passFile.println(msg.text);
            passFile.close();
            bot.sendMessage("Постоянный пароль сохранен.");
        }
        else {
          bot.sendMessage("Ошибка сохранения пароля.");
        }
      }
      else {
        bot.sendMessage("Некорректный формат. Введите 4-значный пароль.");
      }
      expectedInput = "";
      return;
    } 
    else if (expectedInput == "nfc_uid") {
      Serial.println("Processing NFC UID...");
      addNewNfcTag();
      bot.sendMessage("Новая NFC-карта добавлена.");
      expectedInput = "";
      return;
    }   
    else {
      Serial.println("Пароль не получен");
    }
  }

  if(msg.text == "Open") {
    unlockDoor();
    bot.answer("Дверь открыта", FB_NOTIF);
    return;
  }
  else if(msg.text == "Logs") {
    File eventLogFile = SPIFFS.open("/event_log.txt", "r");
    if (!eventLogFile) {
      logEvent("Failed to open event_log.txt for writing", "ERROR");
      bot.answer("Ошибка открытия файла", FB_ALERT);
      return;
    }
    bot.sendFile(eventLogFile, FB_DOC, "event_log.txt", CHAT_IDENTIFIER);
    eventLogFile.close();
    bot.answer("Лог-файл отправлен", FB_NOTIF);
    return;
  }
  else if(msg.text == "Add new password") {
    bot.showMenu("Пароль \t Отпечаток пальца \t UID карты \n Отмена");
    return;
    }
  else if(msg.text == "Пароль") {
    bot.showMenu("Временный \t Постоянный");
    return;
  }
  else if (msg.text == "Временный") {
    expectedInput = "temporary_password";
    String examplePassword = getCurrentDateTime() + " 1234";
    bot.sendMessage("Введите временный пароль вместе с датой (Формат: ГГГГ-ММ-ДД ЧЧ пароль)\nПример: " + examplePassword);
    return;
  }
  else if (msg.text == "Постоянный") {
    expectedInput = "permanent_password";
    bot.sendMessage("Введите постоянный пароль (4 цифры)");
    Serial.print("###Expected input: ");
    Serial.println(expectedInput);
    return;
  }
  else if (msg.text == "Отпечаток пальца") {
    int16_t fid;
    if (get_free_id(&fid)) {
      enroll_finger(fid);
      bot.sendMessage("Добавление отпечатка пальца завершено.");
      return;
    }
    else {
      bot.sendMessage("Нет свободных слотов для отпечатков пальцев.");
      return;
    }
  }
  else if (msg.text == "UID карты") {
    expectedInput = "nfc_uid";
    bot.sendMessage("Приложите карту к сканеру NFC.");
    return;
  }
  else {
    bot.sendMessage(msg.text);
    return;
  }
  

  
}

String getCurrentDateTime() {
    time_t now = time(nullptr);
    struct tm* timeinfo = localtime(&now);
    char buffer[20];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H", timeinfo);
    return String(buffer);
}
