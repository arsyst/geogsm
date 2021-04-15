/*
 *   Author: Artem Voronenko
 *   Project repository on GitHub:
 *   https://github.com/arsyst/geogsm
 */


// ======================== Настройки ========================
float   lowLevelBalance = 50.0;    // Нижний порог для баланса
bool    DEBUG_MODE      = true     // true - включить режим отладки, false - выключить
bool    pinGNDMode      = false    // true - если необходимо использовать дополнительный пин в качестве земли

#define pinGND            -        // Дополнительный пин используемый в качестве земли (в режиме pinGNDMode)
#define gsmRXPin          8        // RX пин для SIM900 модуля
#define gsmTXPin          9        // TX пин для SIM900 модуля
#define redLedPin         10       // Пин для RGB светодиода - красный
#define greenLedPin       11       // Пин для RGB светодиода - зеленый
#define blueLedPin        13       // Пин для RGB светодиода - синий


// Подключение библиотек
#include <TinyGPS++.h>
#include <SoftwareSerial.h>
SoftwareSerial SIM900(gsmRXPin, gsmTXPin);
SoftwareSerial gpsSerial(6, 5)


// ======================== Программные переменные ========================
String  secretCode[]    = "12345";      // Код для совершения некоторых действий при отправке SMS
String  phoneForNotify  = ""            // Телефон для срочных оповещений (пока доступен только один)

String  _response       = "";
String  tasks[13];                      // Переменная для хранения списка задач к исполнению
bool    executingTask   = false;        // Флаг исполнения отложенной задачи

long    lastUpdate      = 0;            // Переменная хранящая время последней проверки
long    updatePeriod    = 90000;        // 90 сек - период автоматической проверки наличия сообщений (в миллисекундах, 1000 - 1 сек)

float   balance         = 0.0           // Переменная для хранения баланса
String  balanceUSSD     = "NONE"        // USSD запрос для определения баланса (задается пользователем)

// ========================================================================

void setup() {
  if (pinGNDMode) {                 // Настройка дополнительного пина для земли (в режиме pinGNDMode)
    digitalWrite(pinGND, LOW);
    pinMode(pinGND, OUTPUT);
  }
  pinMode(redLedPin,   OUTPUT);     // Настройка пинов RGB светодиода
  pinMode(greenLedPin, OUTPUT);
  pinMode(blueLedPin,  OUTPUT);

  digitalWrite(blueLedPin, HIGH);   // Включение синего цвета светодиода
  delay(2000);                      // Оно означает начало подготовки к работе
  
  Serial.begin(9600);
  SIM900.begin(9600);
  DEBUG_PRINTLN("Starting...");

  delay(5000)

  if (sendATCommand("AT", true).indexOf("OK") > -1)         blinkOK(); else blinkFail();  // Команда готовности GSM-модуля
  if (sendATCommand("AT+CLIP=1", true).indexOf("OK") > -1)  blinkOK(); else blinkFail();  // Установка АОН
  if (sendATCommand("AT+CMGF=1", true).indexOf("OK") > -1)  blinkOK(); else blinkFail();  // Установка текстового режима SMS (Text mode)

  sendATCommand("AT+CMGDA=\"DEL ALL\"", true);    // Удаляем все сообщения, чтобы не занимали память МК
  
  lastUpdate = millis() + 10000;     // Ближайшая проверка через 10 сек
}


bool hasMsgToDel = false;                              // Флаг наличия сообщений к удалению

// ======================== Основной цикл ========================

void loop() {
  String _buffer = "";                            // Переменная хранения ответов от GSM-модуля
  if (millis() > lastUpdate && !executingTask) {  // Цикл автоматической проверки SMS, повторяется каждые updatePeriod
    do {
      _buffer = sendATCommand("AT+CMGL=\"REC UNREAD\",1", true);  // Отправляем запрос чтения непрочитанных сообщений
      if (_buffer.indexOf("+CMGL: ") > -1) {                      // Если есть хоть одно, получаем его индекс
        int msgIndex = _buffer.substring(_buffer.indexOf("+CMGL: ") + 7, _buffer.indexOf("\"REC UNREAD\"", _buffer.indexOf("+CMGL: "))).toInt();
        char i = 0;                                               // Счетчик попыток
        do {
          i++;                                                    // Увеличиваем счетчик
          _buffer = sendATCommand("AT+CMGR=" + (String)msgIndex + ",1", true);  // Пробуем получить текст SMS по индексу
          _buffer.trim();                                         // Убираем пробелы в начале и конце
          if (_buffer.endsWith("OK")) {                           // Если ответ заканчивается на "ОК"
            parseSMS(_buffer);                                    // Отправляем текст сообщения на обработку
            if (!hasMsgToDel) hasMsgToDel = true;                 // Ставим флаг наличия сообщений для удаления
            sendATCommand("AT+CMGR=" + (String)msgIndex, true);   // Делаем сообщение прочитанным
            break;                                                // Выход из цикла do
          }
          else {                                                  // Если сообщение не заканчивается на OK
            blinkLed(redLedPin, 500);                                // Мигаем красным светодиодом
            // DEBUG_PRINTLN("MESSAGE ERROR")
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
    //DEBUG_PRINTLN("Task timeout - True");
    sendATCommand("\n", true);
    executingTask = false;                                        // Если флаг не был сброшен по исполению задачи, сбрасываем его принудительно через 3 минуты
  }

  if (SIM800.available())   {                                     // Ожидаем прихода данных (ответа) от модема...
    blinkLed(blueLedPin, 50);                                     // Данные пришли - мигаем зеленым светодиодом

    String msg = waitResponse();                                  // Получаем ответ от модема для анализа
    msg.trim();                                                   // Убираем ненужные пробелы в начале/конце
    DEBUG_PRINTLN(".. " + msg);                                   // ...и выводим их в Serial
    blinkLed(blueLedPin, 50);                                    // Мигаем зеленым светодиодом о приходе данных

    if (msg.startsWith("+CUSD:")) {                               // Если USSD-ответ о балансе
      String msgBalance = msg.substring(msg.indexOf("\"") + 2);   // Парсим ответ
      msgBalance = msgBalance.substring(0, msgBalance.indexOf("\n"));

      balance = getDigitsFromString(msgBalance);                  // Сохраняем баланс
      deleteFirstTask();                                          // Удаляем задачу
      executingTask = false;                                      // Сбрасываем флаг исполнения
      DEBUG_PRINTLN("Balance: " + (String)balance);               // Отчитываемся в Serial
    }
    else if (msg.startsWith("+CMGS:")) {                          // Результат отправки сообщения
      deleteFirstTask();                                          // Удаляем задачу
      executingTask = false;                                      // Сбрасываем флаг исполнения
      DEBUG_PRINTLN("SMS sending - task removed.");               // Отчитываемся в Serial
      addTask("getBalance");                                      // Добавляем задачу запроса баланса
    }
    else if (msg.startsWith("RING")) {                            // При входящем вызове
      sendATCommand("ATH", true);                                 // Всегда сбрасываем
    }
    else if (msg.startsWith("+CMTI:")) {                          // Незапрашиваемый ответ - приход сообщения
      lastUpdate = millis();                                      // Сбрасываем таймер автопроверки наличия сообщений
    }
    else if (msg.startsWith("ERROR")) {                           // Ошибка исполнения команды
      //DEBUG_PRINTLN("Error executing last command.");
      executingTask = false;                                      // Сбрасываем флаг исполнения, но задачу не удаляем - на повторное исполнение
    }
  }

  if (!executingTask && tasks[0] != "") {                         // Если никакая задача не исполняется, и список задач не пуст, то запускаем выполнение.
    showAllTasks();                                               // Показать список задач

    String task = tasks[0];
    if (tasks[0].startsWith("sendSMS")) {                         // Если задача - отправка SMS - отправляем
      task = task.substring(task.indexOf(";") + 1);
      executingTask = true;                                       // Флаг исполнения в true
      sendSMS(task.substring(0, task.indexOf(";")),
              task.substring(task.indexOf(";") + 1));

    }
    else if ((tasks[0].startsWith("getBalance"))) {               // Задача - запрос баланса
      executingTask = true;                                       // Флаг исполнения в true
      sendATCommand("AT+CUSD=1,\"#102#\"", true);                 // Отправка запроса баланса
    }
    else if ((tasks[0].startsWith("clearSMS"))) {                 // Задача - удалить все прочитанные SMS
      sendATCommand("AT+CMGDA=\"DEL READ\"", true);               // Флаг исполнения не устанавливаем - здесь не нужно.
      deleteFirstTask();                                          // Удаляем задачу, сразу после исполнения
    }
    else {
      //DEBUG_PRINTLN("Error: unknown task - " + task);
    }
  }
}

// ========================================================================


void parseSMS(String msg) {         // Функция получения действия из SMS
  String msgheader  = "";
  String msgbody    = "";
  String msgphone   = "";
  // Парсинг SMS, получаем телефон и текст
  msg = msg.substring(msg.indexOf("+CMGR: "));
  msgheader = msg.substring(0, msg.indexOf("\r"));

  msgbody = msg.substring(msgheader.length() + 2);
  msgbody = msgbody.substring(0, msgbody.lastIndexOf("OK"));
  msgbody.trim();

  int firstIndex = msgheader.indexOf("\",\"") + 3;
  int secondIndex = msgheader.indexOf("\",\"", firstIndex);
  msgphone = msgheader.substring(firstIndex, secondIndex);
  msgbody.toLowerCase();

    
  if msgbody.substring(msgbody.indexOf("code") + 5)) == secretCode && msgbody.indexOf("code") > -1 {
    String result = "";                                           // Если в сообщении нету команды 'code' или если код неверный
                                                                  // Обрабатываем это сообщение
    if (msgbody.startsWith("balance")) {                          // Если команда запроса баланса
      if balanceUSSD != "NONE":
        addTask(getSendSMSTaskString(msgphone, "Balance: " + String(balance))); // Добавляем задачу об отправке SMS с балансом
        addTask("getBalance");                                      // Добавляем задачу о запросе баланса
        showAllTasks();                                             // Выводим все задачи
      else {
        addTask(getSendSMSTaskString(msgphone, "Please, send with command 'setussd' USSD request for balance for your operator."));
      }
    }
    else if (msgbody.startsWith("callme")) {                      // Команда осуществить исходящий вызов
      sendATCommand("ATD" + msgphone + ";", true);
    }
    else if (msgbody == "?" || msgbody == "help") {               // Команда получения помощи по командам
      //   String task=getSendSMSTaskString(msgphone, getHelpSMS());
      //   DEBUG_PRINTLN("task: " + task);
      //   addTask(task);

      addTask(getSendSMSTaskString(msgphone, getHelpSMS()));
      addTask("getBalance");
      showAllTasks();
    }
    else if (msgbody.startsWith("checknow")) {                    // Обнулить таймер периодической проверки - проверить сразу
      lastUpdate = millis();
    }
    else {                                                        // Если сообщение не начинается командой
      //DEBUG_PRINTLN("Incorrect format")                         // Отправить сообщение об этом
      addTask(getSendSMSTaskString(msgphone, "Incorrect SMS format. Send 'help code <your_code>' for list all commands. "))
    }

  }
  else {
    blinkLed(blueLedPin, 200)
    blinkLed(redLedPin,  200)
    //DEBUG_PRINTLN("Incorrect code");
  }
}


String getHelpSMS() {                                             // Текст сообщения с помощью по камандам
  return "Balance, CallMe, GetCoords, SetPhone, CheckNow, Help";
}

String getSendSMSTaskString( String phone, String msg) {          // Формируем строку задачи отправки SMS
  return "sendSMS;" + phone + ";" + msg;
}


// ======================== Светодиодная индикация ========================

void blinkOK() {                        // Функция индикации OK (3 мигания зеленым индикатором)
  for (int i = 0; i < 3; i++) {
    blinkLed(greenLedPin, 100);
    delay(100);
  }
}

void blinkFail() {                      // Функция индикации ошибки (длительное мигание красным)
  blinkLed(redLedPin, 1500);
}

void blinkLed(int pin, int _delay) {    // Общая функция для мигания светодиодами
  digitalWrite(pin, HIGH);
  if (_delay > 0) delay(_delay);
  digitalWrite(pin, LOW);
}


// ======================== Задачи ========================

void showAllTasks() {                                             // Показать все задачи
  DEBUG_PRINTLN("All Tasks:");
  for (int i = 0; i < 10; i++) {
    if (tasks[i] == "") break;
    DEBUG_PRINTLN("Task " + (String)(i + 1) + ": " + tasks[i]);
  }
}
void deleteFirstTask() {                                          // Удалить первую задачу, остальные передвинуть вверх на 1
  for (int i = 0; i < 10 - 1; i++) {
    tasks[i] = tasks[i + 1];
    if (tasks[i + 1] == "") break;
  }
}
void addTask(String task) {                                       // Добавить задачу в конец очереди
  for (int i = 0; i < 10; i++) {
    if (tasks[i] == task && (task == "clearSMS" || task == "getBalance")) {
      DEBUG_PRINTLN("Task already exists " + (String)(i + 1) + ": " + task);
      return;
    }
    if (tasks[i] == "") {
      tasks[i] = task;
      DEBUG_PRINTLN("Task " + (String)(i + 1) + " added: " + task);
      return;
    }
  }
  DEBUG_PRINTLN("Error!!! Task NOT added: " + task);
}


void sendSMS(String phone, String message)                      // Функция отправки SMS
{
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


String sendATCommand(String cmd, bool waiting) {  // Функция отправки AT-команд GSM модулю
  String _resp = "";
  
  DEBUG_PRINTLN(cmd);                           // Дублируем команду в монитор порта если нужно
  SIM900.println(cmd);                          // Отправляем команду модулю
  
  if (waiting) {                                // Если необходимо дождаться ответа...
    _resp = waitResponse();                     // ... ждем, когда будет передан ответ
    // Если Echo Mode выключен (ATE0), то эти 3 строки можно закомментировать
    if (_resp.startsWith(cmd)) {                // Убираем из ответа дублирующуюся команду
      _resp = _resp.substring(_resp.indexOf("\r", cmd.length()) + 2);
    }
    DEBUG_PRINTLN(_resp);                    // Дублируем ответ в монитор порта
  }
  return _resp;                                 // Возвращаем результат. Пусто, если проблема
}


String waitResponse() {                         // Функция ожидания ответа и возврата полученного результата
  String _resp = "";                            // Переменная для хранения результата
  long _timeout = millis() + 10000;             // Переменная для отслеживания таймаута (10 секунд)
  
  while (!SIM900.available() && millis() < _timeout)  {}; // Ждем ответа 10 секунд, если пришел ответ или наступил таймаут, то...
  
  if (SIM900.available()) {                     // Если есть, что считывать...
    _resp = SIM900.readString();                // ... считываем и запоминаем
  }
  else {                                        // Если пришел таймаут, то...
    DEBUG_PRINTLN("Timeout...");             // ... если нужно оповещаем об этом и...
  }
  return _resp;                                 // ... возвращаем результат. Пусто, если проблема
}


// ========================================================

String UCS2ToString(String s) {                       // Функция декодирования UCS2 строки
  String result = "";
  unsigned char c[5] = "";                            // Массив для хранения результата
  for (int i = 0; i < s.length() - 3; i += 4) {       // Перебираем по 4 символа кодировки
    unsigned long code = (((unsigned int)HexSymbolToChar(s[i])) << 12) +    // Получаем UNICODE-код символа из HEX представления
                         (((unsigned int)HexSymbolToChar(s[i + 1])) << 8) +
                         (((unsigned int)HexSymbolToChar(s[i + 2])) << 4) +
                         ((unsigned int)HexSymbolToChar(s[i + 3]));
    if (code <= 0x7F) {                               // Теперь в соответствии с количеством байт формируем символ
      c[0] = (char)code;                              
      c[1] = 0;                                       // Не забываем про завершающий ноль
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
    result += String((char*)c);                       // Добавляем полученный символ к результату
  }
  return (result);
}

unsigned char HexSymbolToChar(char c) {
  if      ((c >= 0x30) && (c <= 0x39)) return (c - 0x30);
  else if ((c >= 'A') && (c <= 'F'))   return (c - 'A' + 10);
  else                                 return (0);
}


void DEBUG_PRINTLN(String text) {  // Функция печати данных для отладки 
  if (DEBUG_MODE) {                // Работает при включенном DEBUG_MODE
    Serial.println(text);
  }
}
