#include "Arduino.h"
#include "lib/TM74HC595-4dig-display/TM74HC595Display.h"
#include "lib/TimerOne/TimerOne.h"
#include "lib/ClickEncoder/ClickEncoder.h"
#include "lib/EEPROM/EEPROM.h"


/**
 * Время в милисекундах в течение которых осуществляется подсчёт числа вращений вала насоса
 */
#define MAGN_SENS_VERIFICATION_TIME 3000
/**
 * Пин датчика Холла.
 */
#define PIN_MAGN_SENS 2
/**
 * Пин кнопки энкодера (кнопка запуска/останова насоса).
 */
#define PIN_ENK_SW 3
/**
 * Пин CLK энкодера.
 */
#define PIN_ENK_CLK 4
/**
 * Пин DT энкодера.
 */
#define PIN_ENK_DT 5
/**
 * Пин DIO дисплея.
 */
#define PIN_DISP_DIO 6
/**
 * Пин RCLK дисплея.
 */
#define PIN_DISP_RCLK 7
/**
 * Пин SCLK дисплея.
 */
#define PIN_DISP_SCLK 8
/**
 * Пин 1 управления драйвером двигателя насоса.
 */
#define PIN_DRV_IN1 9
/**
 * Пин 2 управления драйвером двигателя насоса.
 */
#define PIN_DRV_IN2 10
/**
 * Пин PWM управления драйвером двигателя насоса.
 */
#define PIN_DRV_PWM 11


/**
 * Экземпляр класса для работы с энкодером.
 */
ClickEncoder encoder(PIN_ENK_CLK, PIN_ENK_DT, PIN_ENK_SW);
/**
 * Экземпляр класса для работы с дисплеем.
 */
TM74HC595Display disp(PIN_DISP_SCLK, PIN_DISP_RCLK, PIN_DISP_DIO);


/**
 * Значение, установленное энкодером, а также его предыдущее значение.
 */
int enk_val = 0, enk_val_last = 0;
/**
 * Признак направления вращения вала насоса по часовой стрелке.
 */
byte rotation_cw = 1;
/**
 * Число оборотов вала насоса.
 */
volatile int rotation_count = 0;
/**
 * Следующее время расчёта частоты вращения насоса.
 */
unsigned long rotation_count_next_time = 0;
/**
 * Частота вращения вала насоса.
 */
int rotation_speed = 0;


/**
 * Функция инициализации устройства.
 */
void setup() {
	print_HI_I_AM_POMP(); // Выводим приветственное сообщение
	Timer1.initialize(1000); // Инициализируем прерывание по таймеру для опроса энкодера
	Timer1.attachInterrupt(encoder_checker); // Назначаем функцию-обработчик прерывания по таймеру для опроса энкодера
	encoder.setAccelerationEnabled(true); // Разрешаем ускоренную промотку значений при изменении значений с помощью энкодера
	encoder.setDoubleClickEnabled(true); // Разрешаем двойной щелчок по кнопке энкодера
	enk_val = enk_val_last = EEPROM.read(0); // Считываем из EEPROM значение скорости
	analogWrite(PIN_DRV_PWM, enk_val); // Задаем скорость вращения с помощью ШИМ
	rotation_cw = EEPROM.read(1); // Считываем из EEPROM направление вращения насоса
	attachInterrupt(0, rotation_speed_counter, FALLING); // Назначаем прерывание для подсчёта частоты вращения вала насоса
	pinMode(PIN_DRV_IN1, OUTPUT);
	pinMode(PIN_DRV_IN2, OUTPUT);
	pinMode(PIN_DRV_PWM, OUTPUT);
	pinMode(PIN_MAGN_SENS, INPUT);
}
/**
 * Функция бесконечного цикла выполнения программы.
 */
void loop() {
	enk_val += encoder.getValue(); // Считываем новое значение энкодера
	if (enk_val != enk_val_last) { // Если новое значение и старое отличаются, значит необходимо менять скорость
		if (enk_val < 0) // При выходе за пределы значения не меняем
			enk_val = 0;
		else if (enk_val > 255)
			enk_val = 255;
		enk_val_last = enk_val; // Запоминаем предыдущее значение
		EEPROM.write(0, enk_val); // Сохраняем значение скорости в EEPROM
		analogWrite(PIN_DRV_PWM, enk_val); // Задаем скорость вращения с помощью ШИМ
	}
	switch(encoder.getButton()) { // Считываем и анализируем состояние кнопки
		case ClickEncoder::Clicked: // Если кнопка была нажата один раз, то останавливаем или запускаем насос
			if (digitalRead(PIN_DRV_IN1) ^ digitalRead(PIN_DRV_IN2)) { // Если насос работает, то останавливаем его
				digitalWrite(PIN_DRV_IN1, LOW);
				digitalWrite(PIN_DRV_IN2, LOW);
			} else { // Если насос не запущен, то запускаем его
				digitalWrite(PIN_DRV_IN1, rotation_cw);
				digitalWrite(PIN_DRV_IN2, !rotation_cw);
			}
			break;
		case ClickEncoder::DoubleClicked: // Если кнопка была нажата дважды, то изменяем направление прокачки насоса
			rotation_cw = !rotation_cw; // Запоминаем новое направление вращения насоса
			EEPROM.write(1, rotation_cw); // Запоминаем направление вращения в EEPROM
			if (digitalRead(PIN_DRV_IN1) ^ digitalRead(PIN_DRV_IN2)) { // Если насос запущен, то меняем направление вращения
				digitalWrite(PIN_DRV_IN1, rotation_cw);
				digitalWrite(PIN_DRV_IN2, !rotation_cw);

			}
			break;
	}
	if (digitalRead(PIN_DRV_IN1) ^ digitalRead(PIN_DRV_IN2)) { // Если насос работает, то расчитываем скорость вращения
		if (millis() >= rotation_count_next_time) { // Вычисляем частоту вращения вала насоса каждые MAGN_SENS_VERIFICATION_TIME секунд
			detachInterrupt(0); // Отключаем прерывание на время расчёта
			rotation_speed = rotation_count * 60000 / MAGN_SENS_VERIFICATION_TIME; // Вычисляем число оборотов за 60 секунд
			rotation_count = 0; // Сбрасываем счётчик оборотов
			rotation_count_next_time = millis() + MAGN_SENS_VERIFICATION_TIME; // Запоминаем время съема следующих показаний
			attachInterrupt(0, rotation_speed_counter, FALLING); // Восстанавливаем прерывание
		}
	} else // Если насос не работает, то скорость равна 0
		rotation_speed = 0;
	disp.digit4(rotation_speed, 25); // Отображаем частоту вращения вала насоса
}
/**
 * Функция, реализующая последовательный вывод чисел и фразы "HI, I AM, POMP" (привет, я насос).
 */
void print_HI_I_AM_POMP() {
	for(byte i = 0; i <= 9; i++) {
		disp.send(disp._LED_0F[i], 0b1111);
		delay(100);
	}
	for(int i = 0; i<=1500; i++){
	  disp.send(disp._LED_0F[17], 0b0100);
	  disp.send(disp._LED_0F[18], 0b0010);
	}
	for(int i = 0; i<=1500; i++){
	  disp.send(disp._LED_0F[18], 0b1000);
	  disp.send(disp._LED_0F[10], 0b0010);
	  disp.send(disp._LED_0F[17], 0b0001);
	}
	for(int i = 0; i<=1500; i++){
	  disp.send(disp._LED_0F[23], 0b1000);
	  disp.send(disp._LED_0F[22], 0b0100);
	  disp.send(disp._LED_0F[17], 0b0010);
	  disp.send(disp._LED_0F[23], 0b0001);
	}
}
/**
 * Функция для обработки прерывания по таймеру.
 */
void encoder_checker() {
	encoder.service(); // Проверяем произошли ли какие-либо изменения
}
/**
 * Функция осуществляет подсчёт числа срабатываний датчика Холла.
 */
void rotation_speed_counter() {
	rotation_count++;
}
