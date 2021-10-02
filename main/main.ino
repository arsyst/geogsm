/*   
 *   Code version 1.0
 *   Author: Artem Voronenko
 *   Project repository on GitHub:
 *   https://github.com/arsyst/geogsm
 */


// ----------- Настройки -----------

//#define    DEBUG_MODE                   // Для включения режима отладки - раскомментировать эту строку

const long   NOTIFY_PERIOD   = 1200000;   // 30 мин - период отправки SMS уведомлений (пока не установлен режим noSpeedNotify)

String       SECRET_CODE     = "91q1";    // Секретный код

#define      BATTERY_PIN     A5           // Пин для проверки оставшегося заряда аккумулятора

#define      RED_LED_PIN     12           // Пин для RGB светодиода - красный
#define      GREEN_LED_PIN   11           // Пин для RGB светодиода - зеленый
#define      BLUE_LED_PIN    10           // Пин для RGB светодиода - синий

#define      PHONE_ADDR      0            // Адрес EEPROM памяти для хранения доверенного номера телефона
#define      USSD_ADDR       20           // Адрес EEPROM памяти для хранения USSD-запроса баланса
#define      PHONE_INIT_ADDR 1023         // Адрес EEPROM памяти для хранения ключа инициализации доверенного номера телефона
#define      USSD_INIT_ADDR  1022         // Адрес EEPROM памяти для хранения ключа инициализации USSD-запроса баланса
#define      INIT_KEY        50           // Ключ инициализации (первого запуска)

#define      USSD_MAX_LENGTH 12           // Максимальная длина USSD-запроса


// ЗДЕСЬ НИЧЕГО НЕ МЕНЯТЬ!
#define      GSM_POWER_PIN  9             // Сигнальный пин для программного включения GSM шильда (не изменяемый!)

// Подключение библиотек
#include <SoftwareSerial.h>
#include <EEPROM.h>
#include <TinyGPS++.h>

SoftwareSerial SIM900(7, 8);
TinyGPSPlus    gps;

#ifdef  DEBUG_MODE                             // В случае существования DEBUG_MODE...
#include <MemoryFree.h>
#define DEBUG_PRINT(x)     Serial.print(x)     // Создаем "переадресацию" на стандартную функцию
#define DEBUG_PRINTLN(x)   Serial.println(x)
#else                                          // Если DEBUG_MODE не существует - игнорируем упоминания функций...
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#define gpsSerial          Serial              // И создаем "переадресацию" на аппаратный Serial порт
#endif


// ----------- Глобальные переменные -----------

unsigned long lastUpdate   = 0;         // Переменная хранящая время последней проверки
unsigned long updatePeriod = 90000;     // 90 сек - период автоматической проверки наличия сообщений (в миллисекундах)
unsigned long lastNotify   = 0;         // Время последнего уведомления

String  tasks[10];                      // Переменная для хранения списка задач к исполнению
bool    executingTask   = false;        // Флаг исполнения отложенной задачи

float   balance         = 0.0;          // Переменная для хранения баланса

String  gpsLat          = "";           // Переменная хранящая координату широты
String  gpsLng          = "";           // Переменная хранящая координату долготы
 
bool    noBatteryNotify = false;        // Если true - не уведомлять о заряде аккумулятора
bool    noSpeedNotify   = false;        // Если true - не уведомлять о передвижении 

// ========================================================================


void setup() {

  pinMode(RED_LED_PIN,   OUTPUT);     // Настройка пинов RGB светодиода
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN,  OUTPUT);

  digitalWrite(BLUE_LED_PIN, HIGH);   // Включение синего цвета светодиода
  delay(500);                         // Оно означает начало подготовки к работе

  SIM900power();                      // Включение GSM шильда

  Serial.begin(9600);                 // Начало UART соединений
  SIM900.begin(9600);
  
  DEBUG_PRINTLN(F("Starting..."));

  delay(15000);                       // Ожидание настройки и перевода модулей в рабочий режим

  if (sendATCommand(F("AT"), true).indexOf("OK") > -1)         blinkOK(); else blinkFail();  // Команда готовности GSM-модуля
  if (sendATCommand(F("AT+CLIP=1"), true).indexOf("OK") > -1)  blinkOK(); else blinkFail();  // Установка АОН
  if (sendATCommand(F("AT+CMGF=1"), true).indexOf("OK") > -1)  blinkOK(); else blinkFail();  // Установка текстового режима SMS (Text mode)

  sendATCommand(F("AT+CMGDA=\"DEL ALL\""), true);   // Удаление всех сообщений, чтобы не занимать память МК
  
  lastUpdate = millis() + 10000;      // Ближайшая проверка через 10 сек

  digitalWrite(BLUE_LED_PIN, LOW);    // Выключение синего светодиода - окончание подготовки
}


bool hasMsgToDel = false;         // Флаг наличия сообщений к удалению

// ======================== Основной цикл ========================

void loop() {

  // Обработка GPS

  while (gpsSerial.available() > 0) {             // Чтение данных с Serial порта GPS модуля
    char temp = gpsSerial.read();
    gps.encode(temp);
  }

  if (gps.location.isValid()) {                   // Запись данных о местоположении
    gpsLat = String(gps.location.lat(), 6);
    gpsLng = String(gps.location.lng(), 6);
  }
  
  // Если скорость превышает допустимую, пользователь не отключал отправку уведомлений, 
  // пользователь установил номер телефона для уведомлений 
  // и если прошел период отправки уведомлений или их еще не было...
  if (gps.speed.isValid() && (gps.speed.kmph() > (millis() > 100000 && gps.satellites.value() > 5) ? 5 : 11) && 
  !noSpeedNotify && (getUserPhone() != "") && (millis() - lastNotify > NOTIFY_PERIOD || lastNotify == 0)) {
    addTask(getSendSMSTaskString(getUserPhone(), "MOVE: " + gpsLat + ", " + gpsLng)); // Уведомить о движении
    DEBUG_PRINTLN(F("\nMOVE DETECTED, sms sent\n"));
    lastNotify = millis();                        // Обновить переменную последнего уведомления
  }
   
  // Обработка GSM
  
  String _buffer = "";                            // Переменная хранения ответов от GSM-модуля
  if (millis() > lastUpdate && !executingTask) {  // Цикл автоматической проверки SMS, повторяется каждый updatePeriod
    DEBUG_PRINTLN(F("per.loop: free memory="));
    DEBUG_PRINTLN((String)freeMemory());

    if (getBalanceUSSD != "") {                   // Если известен USSD-запрос для баланса...
      addTask(F("getBalance"));                   // Добавляем задачу о запросе баланса
    }
    
    batteryNotify();                              // Проверяем заряд аккумулятора и оповещаем, если необходимо
 
    //DEBUG_PRINTLN(gpsLat + "; " + gpsLng);
    
    do {
      _buffer = sendATCommand(F("AT+CMGL=\"REC UNREAD\",1"), true);  // Отправляем запрос чтения непрочитанных сообщений
      
      DEBUG_PRINTLN(F("\nper-loop: _buffer="));
      DEBUG_PRINT(_buffer);
      DEBUG_PRINTLN(F("--END--\n"));
      
      if (_buffer.indexOf("+CMGL: ") > -1) {      // Если есть хоть одно, получаем его индекс

        DEBUG_PRINT(F("\nper-loop: haveMsg, index = "));
        
        int msgIndex = _buffer.substring(_buffer.indexOf("+CMGL: ") + 7, _buffer.indexOf("\"REC UNREAD\"",
                                         _buffer.indexOf("+CMGL: "))).toInt();
        DEBUG_PRINTLN(msgIndex);
        DEBUG_PRINTLN();
        
        byte i = 0;                                               // Счетчик попыток
        do {
          i++;                                                    // Увеличиваем счетчик
          _buffer = sendATCommand("AT+CMGR=" + (String)msgIndex + ",1", true);  // Пробуем получить текст SMS по индексу
          _buffer.trim();                                         // Убираем пробелы в начале и конце
          if (_buffer.endsWith("OK")) {                           // Если ответ заканчивается на "ОК"

            DEBUG_PRINTLN(F("\nMESSAGE IN PARSE ->\n"));
            
            parseSMS(_buffer);                                    // Отправляем текст сообщения на обработку
            if (!hasMsgToDel) hasMsgToDel = true;                 // Ставим флаг наличия сообщений для удаления
            sendATCommand("AT+CMGR=" + (String)msgIndex, true);   // Делаем сообщение прочитанным
            break;                                                // Выход из цикла do
          }
          else {                                                  // Если сообщение не заканчивается на OK
            blinkLed(RED_LED_PIN, 500);                           // Мигаем красным светодиодом
            DEBUG_PRINT(F("\nMESSAGE ERROR, iterNum="));
            DEBUG_PRINTLN(i);
            DEBUG_PRINTLN(F("msg.err: _buffer="));
            DEBUG_PRINT(_buffer);
            DEBUG_PRINTLN(F("--END--\n"));
          }
          sendATCommand("\n", true);
        } while (i < 10);                                         // Пока попыток меньше 10
        break;
      }
      else {
        lastUpdate = millis() + updatePeriod;                     // Если все обработано, обновляем время последнего обновления
        if (hasMsgToDel) {                                        // Если были сообщения для удаления
          addTask("clearSMS");                                    // Добавляем задание для удаления сообщений
          hasMsgToDel = false;                                    // Сбрасываем флаг наличия сообщений
        }
        break;                                                    // Выходим из цикла
      }
    } while (1);
  }

  if (millis() > lastUpdate + 180000 && executingTask) {          // Таймаут на выполнение задачи - 3 минуты
    //DEBUG_PRINTLN(F("Task timeout - True"));
    sendATCommand("\n", true);
    executingTask = false;                                        // Если флаг не был сброшен по исполению задачи, сбрасываем его принудительно через 3 минуты
  }

  if (SIM900.available())   {                                     // Ожидаем прихода данных от модема...
    blinkLed(BLUE_LED_PIN, 50);                                   // Данные пришли - мигаем синим светодиодом

    String msg = waitResponse();                                  // Получаем ответ от GSM модуля для анализа
    msg.trim();                                                   // Убираем ненужные пробелы в начале/конце
    DEBUG_PRINTLN(F("\nMSG IN -> "));                                // ...и выводим их в Serial
    DEBUG_PRINTLN(msg);
    blinkLed(BLUE_LED_PIN, 50);                                    // Мигаем зеленым светодиодом о приходе данных

    DEBUG_PRINT(F("\nsim-resp: free memory = "));
    DEBUG_PRINTLN((String)freeMemory());
    DEBUG_PRINTLN();

    if (msg.startsWith("+CUSD:")) {                               // Если USSD-ответ о балансе
      String msgBalance = msg.substring(msg.indexOf("\"") + 2);   // Парсим ответ
      msgBalance = msgBalance.substring(0, msgBalance.indexOf("\n"));

      DEBUG_PRINTLN(F("bUSSD: msgBalance="));
      DEBUG_PRINT(msgBalance);
      DEBUG_PRINTLN(F("--END--"));

      getDigitsFromString(msgBalance);                            // Сохраняем баланс
      deleteFirstTask();                                          // Удаляем задачу
      executingTask = false;                                      // Сбрасываем флаг исполнения
      DEBUG_PRINTLN(F("balance="));                               // Отчитываемся в Serial
      DEBUG_PRINTLN(balance);
    }
    else if (msg.startsWith("+CMGS:")) {                          // Результат отправки сообщения
      deleteFirstTask();                                          // Удаляем задачу
      executingTask = false;                                      // Сбрасываем флаг исполнения
      DEBUG_PRINTLN(F("SMS sending - task removed."));            // Отчитываемся в Serial
      addTask("getBalance");                                      // Добавляем задачу запроса баланса
    }
    else if (msg.startsWith("RING")) {                            // При входящем вызове
      sendATCommand(F("ATH"), true);                              // Всегда сбрасываем
    }
    else if (msg.startsWith("+CMTI:")) {                          // Незапрашиваемый ответ - приход сообщения
      lastUpdate = millis();                                      // Сбрасываем таймер автопроверки наличия сообщений
    }
    else if (msg.startsWith("ERROR")) {                           // Ошибка исполнения команды
      DEBUG_PRINTLN(F("Error executing last command."));
      executingTask = false;                                      // Сбрасываем флаг исполнения, но задачу не удаляем - на повторное исполнение
    }
  }

  // Обработка задач
  
  if (!executingTask && tasks[0] != "") {                         // Если никакая задача не исполняется, и список задач не пуст, то запускаем выполнение.
    printAllTasks();                                              // Показать список задач

    String task = tasks[0];
    if (tasks[0].startsWith("sendSMS")) {                         // Если задача - отправка SMS - отправляем
      task = task.substring(task.indexOf(";") + 1);
      executingTask = true;                                       // Флаг исполнения в true
      sendSMS(task.substring(0, task.indexOf(";")),
              task.substring(task.indexOf(";") + 1));

    }
    else if ((tasks[0].startsWith("getBalance"))) {               // Задача - запрос баланса
      executingTask = true;
      sendATCommand("AT+CUSD=1,\"" + getBalanceUSSD() + "\"", true);  // Отправка запроса баланса
    }
    else if ((tasks[0].startsWith("clearSMS"))) {                 // Задача - удалить все прочитанные SMS
      sendATCommand(F("AT+CMGDA=\"DEL READ\""), true);
      deleteFirstTask();                                          // Удаляем задачу, сразу после исполнения
    }
    else {
      //DEBUG_PRINTLN(F("ERR: unknown task="));
      //DEBUG_PRINTLN(task)
    }
  }
}

// ========================================================================


// Функция получения действия из SMS

void parseSMS(String msg) {
  String msgheader  = "";
  String msgbody    = "";
  String msgphone   = "";
  
  // Парсинг SMS, получение телефона и текста сообщения
  msg = msg.substring(msg.indexOf("+CMGR: "));
  msgheader = msg.substring(0, msg.indexOf("\r"));

  msgbody = msg.substring(msgheader.length() + 2);
  msgbody = msgbody.substring(0, msgbody.lastIndexOf("OK"));
  msgbody.trim();

  int firstIndex = msgheader.indexOf("\",\"") + 3;
  int secondIndex = msgheader.indexOf("\",\"", firstIndex);
  msgphone = msgheader.substring(firstIndex, secondIndex);
  msgbody.toLowerCase();

  DEBUG_PRINT(F("\nsms-parse: free memory = "));
  DEBUG_PRINTLN((String)freeMemory());
  DEBUG_PRINTLN(F("sms-parse: msgbody ="));
  DEBUG_PRINTLN(msgbody);
  DEBUG_PRINTLN();

  // Команда - установить доверенный номер телефона 
  if (msgbody.startsWith(F("setphone"))) {
    if (msgbody.substring(9) == SECRET_CODE) setUserPhone(msgphone); // Если код верный - установить номер телефона
    else DEBUG_PRINTLN(F("\nsms-parse-setphone: Incorrect code\n"));
  }

  // Если сообщение от доверенного номера телефона
  else if (msgphone == getUserPhone()) {

    DEBUG_PRINTLN(F("\nsms-parse: IN MAIN IF"));
    
    // Если пользователь не задал USSD для баланса...
    if (getBalanceUSSD() == "" && !(msgbody.startsWith("ussd"))) {
      DEBUG_PRINTLN(F("\nsms-parse: IN 1st IF"));      
      addTask(getSendSMSTaskString(msgphone, F("No USSD"))); // ...информируем об этом
    } 
   
    else {
      String result = "";

      DEBUG_PRINTLN(F("\nsms-parse: IN BIG IF"));

      if (msgbody.startsWith(F("balance"))) {                // Если команда - запрос баланса...
        if (getBalanceUSSD != "") {                          // Если определен USSD запрос для баланса
          addTask(getSendSMSTaskString(msgphone, "Balance: " + String(balance))); // Добавляем задачу об отправке SMS с балансом
        }
        else {                                               // Иначе информируем об отсутствии USSD запроса
          addTask(getSendSMSTaskString(msgphone, F("No USSD")));
        }
      }
      else if (msgbody.startsWith(F("coords"))) {            // Команда - получить текущие координаты модуля
        addTask(getSendSMSTaskString(msgphone, "Coords: " + gpsLat + ", " + gpsLng));
      }
      else if (msgbody.startsWith(F("notifyon"))) {          // Команда - включить уведомления
        noSpeedNotify = false;
        addTask(getSendSMSTaskString(msgphone, F("Notify is On")));
      }
      else if (msgbody.startsWith(F("notifyoff"))) {         // Команда - выключить уведомления
        noSpeedNotify = false;
        addTask(getSendSMSTaskString(msgphone, F("Notify is Off")));
      }
      else if (msgbody.startsWith(F("setussd"))) {           // Команда - установить USSD запрос для получения данных о балансе
        String balanceUSSD = msgbody.substring(5);
        balanceUSSD.trim();
        DEBUG_PRINT(F("\nsms-parse-ussd: balanceUSSD = "));
        DEBUG_PRINTLN(balanceUSSD);
        DEBUG_PRINTLN();
        setBalanceUSSD(balanceUSSD);
        addTask("getBalance");
        addTask(getSendSMSTaskString(msgphone, F("OK, USSD")));
      }
      else if (msgbody.startsWith(F("callme"))) {            // Команда - осуществить исходящий вызов
        sendATCommand("ATD" + msgphone + ";", true);
      }
      else if (msgbody.startsWith(F("checknow"))) {          // Обнулить таймер периодической проверки сообщений - проверить сразу
        lastUpdate = millis();
      }
      else if (msgbody == F("help")) {                       // Команда получения помощи по командам
        addTask(getSendSMSTaskString(msgphone, getHelpSMS()));
      }
      else {                                                 // Если сообщение не начинается с команды
        DEBUG_PRINTLN(F("sms-parse: Incorrect command\n"));  // Отправить сообщение об этом
        addTask(getSendSMSTaskString(msgphone, F("Incorrect command")));
      }
    }

  }
  else {
    blinkLed(BLUE_LED_PIN, 200);
    blinkLed(RED_LED_PIN,  200);
    DEBUG_PRINTLN(F("\nUnknown number\n"));
  }
}


String getHelpSMS() {                                       // Текст сообщения по команде 'help'
  return F("Balance, CallMe, Coords, SetNotify, SetCode, SetUSSD, CheckNow");
}

String getSendSMSTaskString(String phone, String msg) {     // Формирование строки задачи отправки SMS
//  DEBUG_PRINTLN(F("\nget.smstask: return="));
//  DEBUG_PRINT(F("sendSMS;"));
//  DEBUG_PRINT(phone);
//  DEBUG_PRINT(F(";"));
//  DEBUG_PRINTLN(msg);
  addTask("getBalance");                                    // Добавляем задачу о запросе баланса
  printAllTasks();                                          // Выводим все задачи
  return "sendSMS;" + phone + ";" + msg;
}


// ======================== Светодиодная индикация ========================

void blinkOK() {                        // Функция индикации OK (3 мигания зеленым индикатором)
  for (int i = 0; i < 3; i++) {
    blinkLed(GREEN_LED_PIN, 100);
    delay(100);
  }
}

void blinkFail() {                      // Функция индикации ошибки (длительное мигание красным)
  blinkLed(RED_LED_PIN, 1500);
}

void blinkLed(int pin, int _delay) {    // Общая функция для мигания светодиодами
  digitalWrite(pin, HIGH);
  if (_delay > 0) delay(_delay);
  digitalWrite(pin, LOW);
}


// ======================== Задачи ========================

void printAllTasks() {                                            // Показать все задачи
  DEBUG_PRINTLN(F("All Tasks:"));
  for (int i = 0; i < 10; i++) {
    if (tasks[i] == "") break;
    DEBUG_PRINT(F("task: "));
    DEBUG_PRINTLN((String)(i + 1) + ": " + tasks[i]);
  }
}
void deleteFirstTask() {                                          // Удалить первую задачу, остальные передвинуть вверх на 1
  for (int i = 0; i < 10 - 1; i++) {
    tasks[i] = tasks[i + 1];
    if (tasks[i + 1] == "") break;
  }
}
void addTask(String task) {                                       // Добавить задачу в конец очереди
  DEBUG_PRINTLN(F("add.task: free memory="));
  DEBUG_PRINTLN(freeMemory());
  DEBUG_PRINTLN(F("\nadd.task: TASK ADDING, task="));
  DEBUG_PRINTLN(task);
  for (int i = 0; i < 10; i++) {
    if (task == "getBalance" && getBalanceUSSD() == "N") {
      DEBUG_PRINTLN(F("\nadd-task-getbalance: no USSD\n"));
      return;
    }
    if (tasks[i] == task && (task == "clearSMS" || task == "getBalance")) {
      DEBUG_PRINT(F("\nTask already exists: "));
      DEBUG_PRINTLN(task);
      return;
    }
    if (tasks[i] == "") {
      tasks[i] = task;
      DEBUG_PRINT(F("task "));
      DEBUG_PRINT((String)(i + 1));
      DEBUG_PRINT(F(" added: "));
      DEBUG_PRINTLN(task);
      return;
    }
  }
  DEBUG_PRINT(F("Error! Task not added: "));
  DEBUG_PRINTLN(task);
  DEBUG_PRINTLN();
}


void sendSMS(String phone, String message) {                    // Функция отправки SMS
  sendATCommand("AT+CMGS=\"" + phone + "\"", true);             // Переходим в режим ввода текстового сообщения
  sendATCommand(message + "\r\n" + (String)((char)26), true);   // После текста отправляем перенос строки и Ctrl+Z+
}

float getDigitsFromString(String str) {                         // Функция выбора цифр из сообщения - для парсинга баланса из USSD-запроса
  bool   flag     = false;
  String digits   = "0123456789.";
  String result   = "";

  for (int i = 0; i < str.length(); i++) {
    char symbol = char(str.substring(i, i + 1)[0]);
    if (digits.indexOf(symbol) > -1) {
      result += symbol;
      if (!flag) flag = true;
    }
    else {
      if (flag) break;
    }
  }
  return result.toFloat();
}


// ========================================================


String sendATCommand(String cmd, bool waiting) {  // Функция отправки AT-команды
  blinkLed(GREEN_LED_PIN, 70);                      // Мигаем зеленым индикатором об отправке данных GSM-модулю
  String _response = "";
  DEBUG_PRINT(F("\nsend-at-cmd: cmd = "));
  DEBUG_PRINTLN(cmd);
  DEBUG_PRINTLN();
  SIM900.println(cmd);                            // Отправляем команду модулю
  if (waiting) {                                  // Если нужно ждать ответ от модема...
    _response = waitResponse();                   // Результат ответа сохраняем в переменную
//    DEBUG_PRINTLN(F("\nsend-at-cmd: preResp="));
//    DEBUG_PRINTLN(_response);
    if (_response.startsWith(cmd)) {              // Если ответ начинается с отправленной команды, убираем её, чтобы не дублировать
      _response = _response.substring(_response.indexOf("\r\n", cmd.length()) + 2);
    }
    DEBUG_PRINT(F("\nsend-at-cmd: trimmedResp = "));
    DEBUG_PRINTLN(_response);
    DEBUG_PRINTLN();
    return _response;                             // Возвращаем ответ
  }
  return "";                                      // Если ждать ответа не нужно, возвращаем пустую строку
}


String waitResponse() {                           // Функция ожидания ответа от GSM-модуля
  String _buffer;                                 // Переменная для хранения ответа
  unsigned long _timeout = millis() + 10000;      // Таймаут наступит через 10 секунд
  
  while (!SIM900.available() && millis() < _timeout)  {}; // Ждем...
  if (SIM900.available()) {                       // Если есть что принимать...
    _buffer = SIM900.readString();                // ...принимаем
    
    DEBUG_PRINTLN(F("\nwait-resp: response ="));
    DEBUG_PRINTLN(_buffer);
    DEBUG_PRINTLN(F("!END!\n"));
    
    return _buffer;                               // и возвращаем полученные данные
  }
  else {                                          // Если таймаут вышел...
    blinkLed(RED_LED_PIN, 500);                     // ...мигаем красным светодиодом
    DEBUG_PRINTLN(F("wait-resp: Timeout..."));
  }
  return "";                                      // и возвращаем пустую строку
}


// ========================================================

String UCS2ToString(String s) {        // Функция декодирования UCS2 строки
  String result = "";
  unsigned char c[5] = "";
  for (int i = 0; i < s.length() - 3; i += 4) {
    unsigned long code = (((unsigned int)HexSymbolToChar(s[i])) << 12) +
                         (((unsigned int)HexSymbolToChar(s[i + 1])) << 8) +
                         (((unsigned int)HexSymbolToChar(s[i + 2])) << 4) +
                         ((unsigned int)HexSymbolToChar(s[i + 3]));
    if (code <= 0x7F) {
      c[0] = (char)code;                              
      c[1] = 0;
    } else if (code <= 0x7FF) {
      c[0] = (char)(0xC0 | (code >> 6));
      c[1] = (char)(0x80 | (code & 0x3F));
      c[2] = 0;
    } else if (code <= 0xFFFF) {
      c[0] = (char)(0xE0 | (code >> 12));
      c[1] = (char)(0x80 | ((code >> 6) & 0x3F));
      c[2] = (char)(0x80 | (code & 0x3F));
      c[3] = 0;
    } else if (code <= 0x1FFFFF) {
      c[0] = (char)(0xE0 | (code >> 18));
      c[1] = (char)(0xE0 | ((code >> 12) & 0x3F));
      c[2] = (char)(0x80 | ((code >> 6) & 0x3F));
      c[3] = (char)(0x80 | (code & 0x3F));
      c[4] = 0;
    }
    result += String((char*)c);
  }
  DEBUG_PRINTLN(F("\nucs2-to-str: result ="));
  DEBUG_PRINTLN(result);
  DEBUG_PRINTLN(F("!END!\n"));
  return (result);
}

unsigned char HexSymbolToChar(char c) {
  if      ((c >= 0x30) && (c <= 0x39)) return (c - 0x30);
  else if ((c >= 'A') && (c <= 'F'))   return (c - 'A' + 10);
  else                                 return (0);
}

// ========================================================


// Функция своевременной отправки уведомлений о низком заряде аккумулятора
void batteryNotify() {
//  if (getUserPhone() != "") {
//    if (analogRead(BATTERY_PIN) < 188 && !noBatteryNotify) {
//      addTask(getSendSMSTaskString(getUserPhone(), F("LOW BATTERY")));
//      noBatteryNotify = true;
//    } else if (analogRead(BATTERY_PIN) >= 188) {
//      noBatteryNotify = false;
//    }
//  }
}

// Функция программного включения GSM модуля
void SIM900power() {
  pinMode(GSM_POWER_PIN, OUTPUT);
  digitalWrite(GSM_POWER_PIN, LOW);
  delay(1000);
  digitalWrite(GSM_POWER_PIN, HIGH);
  delay(2000);
  digitalWrite(GSM_POWER_PIN, LOW);
  delay(3000);
}

// ========================================================


// Функция установки и сохранения доверенного номера телефона в EEPROM памяти
bool setUserPhone(String phone) {
  if (phone.length() != 12) return false;    // Если длина номера телефона отличается от заданной длины - вернуть false
  EEPROM.update(PHONE_INIT_ADDR, INIT_KEY);  // Установить ключ инициализации номера телефона
  char phoneCharArray[12];
  phone.toCharArray(phoneCharArray, 12);
  EEPROM.put(PHONE_ADDR, phoneCharArray);
  return true;
}

// Фунция возвращающая доверенный номер телефона
String getUserPhone() {
  if (EEPROM.read(PHONE_INIT_ADDR) != INIT_KEY) return (String)"";
  char phoneCharArray[12];
  EEPROM.get(PHONE_ADDR, phoneCharArray);
  return (String)phoneCharArray;
}

// Функция установки и сохранения USSD-запроса баланса в EEPROM памяти
bool setBalanceUSSD(String ussd) {
  if (ussd.length() > USSD_MAX_LENGTH) return false;    // Если длина строки больше разрешеннной - вернуть false
  if (ussd.indexOf(".") > -1) return false;             // Если строка содержит символ "." - вернуть false
  EEPROM.update(USSD_INIT_ADDR, INIT_KEY);              // Установить ключ инициализации USSD-запроса баланса
  while (ussd.length() < USSD_MAX_LENGTH) ussd += ".";  // Дополнить строку символами "." до указанной длины
  char ussdCharArray[USSD_MAX_LENGTH];
  ussd.toCharArray(ussdCharArray, USSD_MAX_LENGTH);
  EEPROM.put(USSD_ADDR, ussdCharArray);                 // Dev: здесь строка имеет вид "ussd_запрос....."
  return true;
}

// Функция возвращающая USSD-запрос баланса
String getBalanceUSSD() {
  if (EEPROM.read(USSD_INIT_ADDR) != INIT_KEY) return (String)"";
  char ussdCharArray[USSD_MAX_LENGTH];
  EEPROM.get(USSD_ADDR, ussdCharArray);
  String ussdString = (String)ussdCharArray;
  ussdString.replace(".", "");
  return ussdString;
}
