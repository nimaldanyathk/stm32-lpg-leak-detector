/*
 * LPG leak detector - main firmware
 * STM32F401CCU6 Black Pill, Keil + CMSIS (no HAL), 16 MHz HSI
 * 23CSE304 Embedded Systems case study
 *
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 *
 * Pins:
 *   PA1  - MQ-6 AO   (via 10k/10k divider!)
 *   PB0  - MQ-2 AO   (via divider)
 *   PB1  - MQ-135 AO (via divider)
 *   PB6/PB7 - I2C1 SCL/SDA, OLED (0x3C) + AHT20 (0x38)
 *   PA9/PA10 - USART1 TX/RX to HC-05, 9600 baud
 *   PB12 - buzzer, PC13 - LED (active low), PA0 - KEY button
 *   PA13/PA14 - SWD, don't touch
 *
 * Flow: warm-up 2 min -> calibrate in clean air -> every 100 ms read
 * sensors, make 10 features, run the tree, vote over 1 s, decide alarm.
 * Fail-safe: strong MQ-6 drop always alarms, whatever the model says.
 *
 * KEY: short = next OLED page / mute, hold 1 s = self test,
 *      hold 4 s = calibrate.
 * Bluetooth keys: L/R log on/off, 0-4 label, X clear, C cal, T test,
 *      N next page, M mute, E events, ? help
 */

#include "stm32f4xx.h"
#include <stdint.h>
#include "features.h"
#include "model.h"

/* ---- settings ---- */

#define WARMUP_SECONDS      120U    /* heater warm-up after power on      */
#define CAL_SAMPLES         100U    /* 100 x 100 ms = 10 s baseline       */

#define FAILSAFE_R6         0.40f   /* MQ-6 Rs/R0 below this = leak       */
#define FAILSAFE_HOLD       10U     /* for 1 s before it alarms           */
#define VOTE_WINDOW         10U     /* last 1 s of predictions            */
#define LPG_VOTES_TO_ALARM  6U      /* 6 of the last 10 say LPG = alarm   */
#define CLEAR_SAMPLES       50U     /* 5 s of safe before alarm clears    */

#define RAW_LOW_FAULT       15U     /* below: sensor wire disconnected    */
#define RAW_HIGH_FAULT      3400U   /* above: impossible with the divider */
#define FAULT_HOLD          20U     /* 2 s of bad readings = fault        */

#define BUZZER_IS_PASSIVE   0       /* 0 active buzzer, 1 passive buzzer  */
#define BUZZER_ON_HIGH      1       /* 1 pin high = on, 0 pin low = on    */

#define TEST_PRESS_MS       1000U   /* hold 1 s = self test               */
#define CAL_PRESS_MS        4000U   /* hold 4 s = calibrate               */
#define MUTE_MS             60000U  /* alarm mute time                    */
#define UI_PAGES            5U      /* OLED pages cycled by short press   */
#define TREND_LEN           128U    /* 128 points x 1 s = ~2 min graph     */
#define EVT_MAX             5U      /* alarm events remembered            */

#define RH_BASE_ALPHA       (1.0f / 3000.0f) /* humidity average, ~5 min */

/* ---- hardware map ---- */

#define ADC_CH_MQ6          1U      /* PA1 */
#define ADC_CH_MQ2          8U      /* PB0 */
#define ADC_CH_MQ135        9U      /* PB1 */
#define OLED_ADDR           0x3CU
#define AHT_ADDR            0x38U

#define CPU_HZ              16000000U
#define TICK_HZ             4000U   /* SysTick rate, 4 per millisecond    */

/* raw Cortex-M debug registers, used only to time the inference */
#define DEMCR_REG           (*(volatile uint32_t *)0xE000EDFCU)
#define DWT_CTRL_REG        (*(volatile uint32_t *)0xE0001000U)
#define DWT_CYCCNT_REG      (*(volatile uint32_t *)0xE0001004U)

/* ---- classes ---- */

#define CLS_CLEAN   0U
#define CLS_LPG     1U
#define CLS_SMOKE   2U
#define CLS_STEAM   3U
#define CLS_VOC     4U
#define NUM_CLASSES 5U

static const char *const class_name[NUM_CLASSES] = {
    "CLEAN AIR", "LPG", "SMOKE", "STEAM", "ALCOHOL/VOC"
};
static const char *const class_short[NUM_CLASSES] = {
    "CLEAN", "LPG", "SMOKE", "STEAM", "VOC"
};

/* ---- state shared with interrupts ---- */

static volatile uint32_t g_ms;              /* milliseconds since boot    */
static volatile uint8_t  g_sample_due;      /* set every 100 ms           */
static volatile uint32_t g_sample_ms;       /* time stamp of that sample  */

#define BUZ_OFF     0U
#define BUZ_ALARM   1U                      /* 250 ms on, 250 ms off      */
#define BUZ_FAULT   2U                      /* short chirp every 2 s      */
static volatile uint8_t  g_buz_mode;
static volatile uint32_t g_chirp_until;     /* one-off beep end time      */

#define BTN_NONE    0U
#define BTN_SHORT   1U
#define BTN_TEST    2U
#define BTN_CAL     3U
static volatile uint8_t  g_btn_event;
static volatile uint8_t  g_btn_down;
static volatile uint32_t g_btn_t_down;
static volatile uint32_t g_btn_t_edge;

#define TX_SIZE     1024U
#define RX_SIZE     64U
static volatile uint8_t  tx_buf[TX_SIZE];
static volatile uint16_t tx_head, tx_tail;
static volatile uint8_t  rx_buf[RX_SIZE];
static volatile uint16_t rx_head, rx_tail;

/* ---- small helpers ---- */

static void delay_us(uint32_t us)
{
    volatile uint32_t n = us * 4U;          /* rough, about 1 us per 4    */
    while (n--) { __NOP(); }
}

static void delay_ms(uint32_t ms)
{
    uint32_t start = g_ms;
    while ((g_ms - start) < ms) { __NOP(); }
}

/* Tiny string builder, so there is no printf and no library to set up. */
typedef struct {
    char    *buf;
    uint16_t len;
    uint16_t cap;
} str_t;

static void str_init(str_t *s, char *buf, uint16_t cap)
{
    s->buf = buf; s->len = 0; s->cap = cap; buf[0] = '\0';
}

static void str_c(str_t *s, char c)
{
    if ((uint16_t)(s->len + 1U) < s->cap) {
        s->buf[s->len++] = c;
        s->buf[s->len] = '\0';
    }
}

static void str_s(str_t *s, const char *t)
{
    while (*t) str_c(s, *t++);
}

static void str_u(str_t *s, uint32_t v)
{
    char t[10];
    uint8_t i = 0;
    do { t[i++] = (char)('0' + (v % 10U)); v /= 10U; } while (v && i < 10U);
    while (i) str_c(s, t[--i]);
}

static void str_i(str_t *s, int32_t v)
{
    if (v < 0) { str_c(s, '-'); str_u(s, (uint32_t)(-v)); }
    else       { str_u(s, (uint32_t)v); }
}

static void str_f(str_t *s, float v, uint8_t dec)
{
    uint32_t scale = 1U, x, frac, d;
    uint8_t i;
    for (i = 0; i < dec; i++) scale *= 10U;
    if (v < 0.0f) { str_c(s, '-'); v = -v; }
    if (v > 99999.0f) v = 99999.0f;
    x = (uint32_t)(v * (float)scale + 0.5f);
    str_u(s, x / scale);
    if (dec) {
        str_c(s, '.');
        frac = x % scale;
        d = scale / 10U;
        while (d > 1U && frac < d) { str_c(s, '0'); d /= 10U; }
        str_u(s, frac);
    }
}

/* ---- clock ---- */

/* Force a known clock: HSI 16 MHz, all buses undivided. This makes every
   baud rate and timing below correct no matter what SystemInit did.   */
static void clock_init(void)
{
    uint32_t t;

    RCC->CR |= RCC_CR_HSION;
    t = 100000U;
    while (!(RCC->CR & RCC_CR_HSIRDY) && --t) { }

    RCC->CFGR &= ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    t = 100000U;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) && --t) { }

    RCC->CR &= ~RCC_CR_PLLON;               /* PLL not needed             */

    SCB->CPACR |= (0xFU << 20);             /* enable the FPU (CP10/CP11) */
    __DSB();
    __ISB();
}

/* ---- systick: time + buzzer ---- */

static void buzzer_pin(uint8_t on)
{
#if BUZZER_ON_HIGH
    GPIOB->BSRR = on ? (1U << 12) : (1U << (12 + 16));
#else
    GPIOB->BSRR = on ? (1U << (12 + 16)) : (1U << 12);
#endif
}

void SysTick_Handler(void)
{
    static uint8_t sub = 0;
    static uint8_t tone = 0;
    uint32_t ms = g_ms;
    uint8_t on = 0;

    if ((int32_t)(g_chirp_until - ms) > 0)           on = 1;
    else if (g_buz_mode == BUZ_ALARM)                on = (uint8_t)((ms % 500U) < 250U);
    else if (g_buz_mode == BUZ_FAULT)                on = (uint8_t)((ms % 2000U) < 80U);

#if BUZZER_IS_PASSIVE
    if (on) { tone ^= 1U; buzzer_pin(tone); }        /* 2 kHz square wave  */
    else    { tone = 0;   buzzer_pin(0); }
#else
    (void)tone;
    buzzer_pin(on);
#endif

    if (++sub >= (TICK_HZ / 1000U)) {
        sub = 0;
        g_ms = ms + 1U;
        if ((g_ms % 100U) == 0U) {
            g_sample_ms  = g_ms;
            g_sample_due = 1U;
        }
    }
}

static void chirp(uint32_t ms)
{
    g_chirp_until = g_ms + ms;
}

/* ---- gpio ---- */

static void gpio_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
    __DSB();

    /* PC13 LED: output, start off (high) */
    GPIOC->BSRR   = (1U << 13);
    GPIOC->MODER  = (GPIOC->MODER & ~(3U << (13 * 2))) | (1U << (13 * 2));

    /* PB12 buzzer: set the off level first, then make it an output */
    buzzer_pin(0);
    GPIOB->OTYPER &= ~(1U << 12);
    GPIOB->MODER  = (GPIOB->MODER & ~(3U << (12 * 2))) | (1U << (12 * 2));

    /* PA0 KEY: input with pull-up (button pulls it to ground) */
    GPIOA->MODER &= ~(3U << (0 * 2));
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~(3U << (0 * 2))) | (1U << (0 * 2));
}

#define LED_ON()   (GPIOC->BSRR = (1U << (13 + 16)))
#define LED_OFF()  (GPIOC->BSRR = (1U << 13))

/* ---- key button on pa0, exti0 ---- */

static void button_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    __DSB();
    SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;     /* EXTI0 <- port A     */
    EXTI->IMR  |= EXTI_IMR_MR0;
    EXTI->RTSR |= EXTI_RTSR_TR0;                    /* release             */
    EXTI->FTSR |= EXTI_FTSR_TR0;                    /* press               */
    EXTI->PR    = EXTI_PR_PR0;
    NVIC_SetPriority(EXTI0_IRQn, 2);             /* all IRQs equal: no nesting */
    NVIC_EnableIRQ(EXTI0_IRQn);
}

void EXTI0_IRQHandler(void)
{
    uint32_t now = g_ms;
    uint8_t pressed;

    EXTI->PR = EXTI_PR_PR0;                         /* clear the flag      */
    if ((now - g_btn_t_edge) < 30U) return;         /* ignore bounce       */
    g_btn_t_edge = now;

    pressed = (uint8_t)((GPIOA->IDR & 1U) == 0U);
    if (pressed && !g_btn_down) {
        g_btn_down   = 1U;
        g_btn_t_down = now;
    } else if (!pressed && g_btn_down) {
        uint32_t held = now - g_btn_t_down;
        g_btn_down = 0U;
        if (held >= CAL_PRESS_MS)       g_btn_event = BTN_CAL;
        else if (held >= TEST_PRESS_MS) g_btn_event = BTN_TEST;
        else if (held >= 40U)           g_btn_event = BTN_SHORT;
    }
}

/* ---- usart1 <-> hc-05 ---- */

static void uart_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    __DSB();

    GPIOA->MODER  = (GPIOA->MODER & ~((3U << (9 * 2)) | (3U << (10 * 2))))
                  | (2U << (9 * 2)) | (2U << (10 * 2));
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~(3U << (10 * 2))) | (1U << (10 * 2));
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4))))
                  | (7U << ((9 - 8) * 4)) | (7U << ((10 - 8) * 4));   /* AF7 */

    USART1->CR1 = 0;
    USART1->BRR = 1667U;                    /* 16 MHz / 9600 = 104.19      */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_SetPriority(USART1_IRQn, 2);
    NVIC_EnableIRQ(USART1_IRQn);
}

void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;

    if (sr & (USART_SR_RXNE | USART_SR_ORE)) {
        uint8_t c = (uint8_t)USART1->DR;            /* also clears ORE     */
        uint16_t next = (uint16_t)((rx_head + 1U) % RX_SIZE);
        if (next != rx_tail) { rx_buf[rx_head] = c; rx_head = next; }
    }

    if ((USART1->CR1 & USART_CR1_TXEIE) && (sr & USART_SR_TXE)) {
        if (tx_tail != tx_head) {
            USART1->DR = tx_buf[tx_tail];
            tx_tail = (uint16_t)((tx_tail + 1U) % TX_SIZE);
        } else {
            USART1->CR1 &= ~USART_CR1_TXEIE;        /* nothing left        */
        }
    }
}

static void uart_putc(char c)
{
    uint16_t next = (uint16_t)((tx_head + 1U) % TX_SIZE);
    while (next == tx_tail) { }                     /* full: wait          */
    tx_buf[tx_head] = (uint8_t)c;
    tx_head = next;
    USART1->CR1 |= USART_CR1_TXEIE;
}

static void uart_puts(const char *s)
{
    while (*s) uart_putc(*s++);
}

static int16_t uart_getc(void)
{
    uint8_t c;
    if (rx_tail == rx_head) return -1;
    c = rx_buf[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) % RX_SIZE);
    return (int16_t)c;
}

/* ---- adc ---- */

static void adc_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;
    __DSB();

    GPIOA->MODER |= (3U << (1 * 2));                    /* PA1 analog     */
    GPIOB->MODER |= (3U << (0 * 2)) | (3U << (1 * 2));  /* PB0, PB1       */
    GPIOA->PUPDR &= ~(3U << (1 * 2));
    GPIOB->PUPDR &= ~((3U << (0 * 2)) | (3U << (1 * 2)));

    ADC->CCR    = 0;                                    /* PCLK2 / 2      */
    ADC1->CR1   = 0;                                    /* 12 bit         */
    ADC1->SQR1  = 0;                                    /* 1 conversion   */
    /* 480-cycle sampling on channels 1, 8, 9 (5 kOhm divider source) */
    ADC1->SMPR2 = (7U << (ADC_CH_MQ6 * 3U)) | (7U << (ADC_CH_MQ2 * 3U))
                | (7U << (ADC_CH_MQ135 * 3U));
    ADC1->CR2   = ADC_CR2_ADON;
    delay_us(20);
}

static uint16_t adc_read(uint32_t ch)
{
    uint32_t t = 20000U;
    ADC1->SQR3 = ch;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC) && --t) { }
    return (uint16_t)(ADC1->DR & 0x0FFFU);
}

static uint16_t adc_avg16(uint32_t ch)
{
    uint32_t sum = 0;
    uint8_t i;
    (void)adc_read(ch);                     /* settle after channel switch */
    for (i = 0; i < 16U; i++) sum += adc_read(ch);
    return (uint16_t)(sum >> 4);
}

/* ---- i2c1 ---- */

#define I2C_TIMEOUT 20000U

/* If a slave was left holding SDA low (reset mid-transfer), clock it out. */
static void i2c_bus_recover(void)
{
    uint8_t i;

    GPIOB->BSRR   = (1U << 6) | (1U << 7);                     /* release */
    GPIOB->OTYPER |= (1U << 6) | (1U << 7);                    /* OD      */
    GPIOB->PUPDR  = (GPIOB->PUPDR & ~((3U << 12) | (3U << 14))) | (1U << 12) | (1U << 14);
    GPIOB->MODER  = (GPIOB->MODER & ~((3U << 12) | (3U << 14))) | (1U << 12) | (1U << 14);
    delay_us(10);

    for (i = 0; i < 9U && !(GPIOB->IDR & (1U << 7)); i++) {
        GPIOB->BSRR = (1U << (6 + 16)); delay_us(6);           /* SCL low */
        GPIOB->BSRR = (1U << 6);        delay_us(6);           /* SCL hi  */
    }
    /* STOP condition: SDA rises while SCL is high */
    GPIOB->BSRR = (1U << (6 + 16)); delay_us(6);
    GPIOB->BSRR = (1U << (7 + 16)); delay_us(6);
    GPIOB->BSRR = (1U << 6);        delay_us(6);
    GPIOB->BSRR = (1U << 7);        delay_us(6);
}

static void i2c_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    __DSB();

    i2c_bus_recover();

    /* PB6, PB7: AF4, open drain, pull-up */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFU << 24) | (0xFU << 28))) | (4U << 24) | (4U << 28);
    GPIOB->OSPEEDR |= (3U << 12) | (3U << 14);
    GPIOB->MODER  = (GPIOB->MODER & ~((3U << 12) | (3U << 14))) | (2U << 12) | (2U << 14);

    RCC->APB1RSTR |=  RCC_APB1RSTR_I2C1RST;     /* clean peripheral reset  */
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;

    I2C1->CR1   = 0;
    I2C1->CR2   = 16U;                          /* APB1 = 16 MHz           */
    I2C1->CCR   = 80U;                          /* 100 kHz standard mode   */
    I2C1->TRISE = 17U;
    I2C1->CR1   = I2C_CR1_PE;
}

static void i2c_fail(void)
{
    I2C1->CR1 |= I2C_CR1_STOP;
    i2c_init();
}

static uint8_t i2c_tx(uint8_t b)
{
    uint32_t t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & I2C_SR1_TXE)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return 1;
        }
        if (!--t) { i2c_fail(); return 1; }
    }
    I2C1->DR = b;
    return 0;
}

/* Write prefix a[] then payload b[] in one transaction. 0 = OK. */
static uint8_t i2c_write(uint8_t addr7, const uint8_t *a, uint16_t na,
                         const uint8_t *b, uint16_t nb)
{
    uint32_t t;

    t = I2C_TIMEOUT;
    while (I2C1->SR2 & I2C_SR2_BUSY) { if (!--t) { i2c_fail(); return 1; } }

    I2C1->CR1 |= I2C_CR1_START;
    t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & I2C_SR1_SB)) { if (!--t) { i2c_fail(); return 1; } }

    I2C1->DR = (uint8_t)(addr7 << 1);
    t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & I2C_SR1_ADDR)) {
        if (I2C1->SR1 & I2C_SR1_AF) {               /* nobody answered     */
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return 2;
        }
        if (!--t) { i2c_fail(); return 1; }
    }
    (void)I2C1->SR2;                                /* clears ADDR         */

    while (na--) { if (i2c_tx(*a++)) return 1; }
    while (nb--) { if (i2c_tx(*b++)) return 1; }

    t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & I2C_SR1_BTF)) { if (!--t) { i2c_fail(); return 1; } }
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}

/* wait for an SR1 flag during a read. 0 = ok */
static uint8_t i2c_wait_sr1(uint32_t flag)
{
    uint32_t t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & flag)) {
        if (!--t) { i2c_fail(); return 1; }
    }
    return 0;
}

/* Read n bytes (n = 1, 2 or more) using the STM32F4 I2C sequences
   from the reference manual. 0 = OK, 1 = bus error, 2 = no answer. */
static uint8_t i2c_read(uint8_t addr7, uint8_t *buf, uint16_t n)
{
    uint32_t t;

    if (n == 0U) return 0;
    t = I2C_TIMEOUT;
    while (I2C1->SR2 & I2C_SR2_BUSY) { if (!--t) { i2c_fail(); return 1; } }

    I2C1->CR1 &= ~I2C_CR1_POS;
    I2C1->CR1 |= I2C_CR1_ACK;
    I2C1->CR1 |= I2C_CR1_START;
    if (i2c_wait_sr1(I2C_SR1_SB)) return 1;

    I2C1->DR = (uint8_t)((addr7 << 1) | 1U);
    t = I2C_TIMEOUT;
    while (!(I2C1->SR1 & I2C_SR1_ADDR)) {
        if (I2C1->SR1 & I2C_SR1_AF) {
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 &= ~I2C_CR1_ACK;
            I2C1->CR1 |= I2C_CR1_STOP;
            return 2;
        }
        if (!--t) { i2c_fail(); return 1; }
    }

    if (n == 1U) {
        I2C1->CR1 &= ~I2C_CR1_ACK;              /* NACK the only byte  */
        __disable_irq();
        (void)I2C1->SR2;                        /* clear ADDR          */
        I2C1->CR1 |= I2C_CR1_STOP;
        __enable_irq();
        if (i2c_wait_sr1(I2C_SR1_RXNE)) return 1;
        buf[0] = (uint8_t)I2C1->DR;
        return 0;
    }

    if (n == 2U) {
        I2C1->CR1 |= I2C_CR1_POS;
        __disable_irq();
        (void)I2C1->SR2;
        I2C1->CR1 &= ~I2C_CR1_ACK;
        __enable_irq();
        if (i2c_wait_sr1(I2C_SR1_BTF)) return 1;
        __disable_irq();
        I2C1->CR1 |= I2C_CR1_STOP;
        buf[0] = (uint8_t)I2C1->DR;
        __enable_irq();
        buf[1] = (uint8_t)I2C1->DR;
        I2C1->CR1 &= ~I2C_CR1_POS;
        return 0;
    }

    (void)I2C1->SR2;                            /* clear ADDR, ACK on  */
    while (n > 3U) {
        if (i2c_wait_sr1(I2C_SR1_RXNE)) return 1;
        *buf++ = (uint8_t)I2C1->DR;
        n--;
    }
    if (i2c_wait_sr1(I2C_SR1_BTF)) return 1;   /* N-2 in DR, N-1 shift */
    I2C1->CR1 &= ~I2C_CR1_ACK;
    *buf++ = (uint8_t)I2C1->DR;                 /* byte N-2            */
    if (i2c_wait_sr1(I2C_SR1_BTF)) return 1;   /* N-1 in DR, N shift   */
    I2C1->CR1 |= I2C_CR1_STOP;
    *buf++ = (uint8_t)I2C1->DR;                 /* byte N-1            */
    if (i2c_wait_sr1(I2C_SR1_RXNE)) return 1;
    *buf = (uint8_t)I2C1->DR;                   /* byte N              */
    return 0;
}

/* ---- aht20 temperature + humidity ---- */

static uint8_t aht_crc_on = 1;                  /* some clones skip CRC  */

static uint8_t aht_crc8(const uint8_t *d, uint8_t n)
{
    uint8_t crc = 0xFF, i, b;
    for (i = 0; i < n; i++) {
        crc ^= d[i];
        for (b = 0; b < 8U; b++)
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x31U) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* reset + calibrate the AHT20. 0 = found and ready */
static uint8_t aht_init(void)
{
    static const uint8_t reset = 0xBA;
    static const uint8_t calib[3] = { 0xBE, 0x08, 0x00 };
    uint8_t st;

    if (i2c_write(AHT_ADDR, &reset, 1, 0, 0)) return 1;
    delay_ms(25);
    if (i2c_read(AHT_ADDR, &st, 1)) return 1;
    if (!(st & 0x08U)) {                        /* not calibrated yet    */
        if (i2c_write(AHT_ADDR, calib, 3, 0, 0)) return 1;
        delay_ms(15);
    }
    return 0;
}

/* start one measurement; the result is ready about 80 ms later */
static uint8_t aht_trigger(void)
{
    static const uint8_t cmd[3] = { 0xAC, 0x33, 0x00 };
    return i2c_write(AHT_ADDR, cmd, 3, 0, 0);
}

/* 0 = ok, 1 = bus error, 2 = still busy, 3 = CRC mismatch */
static uint8_t aht_fetch(float *rh, float *temp)
{
    uint8_t d[7];
    uint32_t hr, tr;

    if (i2c_read(AHT_ADDR, d, 7)) return 1;
    if (d[0] & 0x80U) return 2;
    if (aht_crc_on && aht_crc8(d, 6) != d[6]) return 3;

    hr = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | ((uint32_t)d[3] >> 4);
    tr = (((uint32_t)d[3] & 0x0FU) << 16) | ((uint32_t)d[4] << 8) | (uint32_t)d[5];
    *rh   = (float)hr * (100.0f / 1048576.0f);
    *temp = (float)tr * (200.0f / 1048576.0f) - 50.0f;
    return 0;
}

/* ---- ssd1306 oled 128 x 64 ---- */

static const uint8_t font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}
};

#define OLED_W              128
#define OLED_H              64
#define OLED_PAGES          8U
#define OLED_PAGES_PER_TICK 3U          /* about 12 ms each at 100 kHz      */

static uint8_t oled_ok;
static uint8_t oled_inverted;           /* hardware invert, used to flash   */
static uint8_t fb[OLED_PAGES][OLED_W];      /* what we want on screen       */
static uint8_t fb_sent[OLED_PAGES][OLED_W]; /* what the OLED already shows  */

static uint8_t oled_cmds(const uint8_t *c, uint16_t n)
{
    static const uint8_t ctrl = 0x00;
    return i2c_write(OLED_ADDR, &ctrl, 1, c, n);
}

/* send one 128-byte page (8 pixel rows) to the OLED */
static uint8_t oled_send_page(uint8_t page, const uint8_t *data)
{
    static const uint8_t ctrl = 0x40;
    uint8_t cmd[3];

    cmd[0] = (uint8_t)(0xB0U | (page & 7U));    /* page                    */
    cmd[1] = 0x00;                              /* column low  = 0         */
    cmd[2] = 0x10;                              /* column high = 0         */
    if (oled_cmds(cmd, 3)) return 1;
    return i2c_write(OLED_ADDR, &ctrl, 1, data, (uint16_t)OLED_W);
}

static uint8_t oled_init(void)
{
    static const uint8_t init_seq[] = {
        0xAE,             /* display off                    */
        0xD5, 0x80,       /* clock divide                   */
        0xA8, 0x3F,       /* multiplex 64                   */
        0xD3, 0x00,       /* no offset                      */
        0x40,             /* start line 0                   */
        0x8D, 0x14,       /* charge pump on                 */
        0x20, 0x02,       /* page addressing                */
        0xA1, 0xC8,       /* normal orientation             */
        0xDA, 0x12,       /* COM pins                       */
        0x81, 0xCF,       /* contrast (bright)              */
        0xD9, 0xF1,       /* pre-charge                     */
        0xDB, 0x40,       /* VCOM detect                    */
        0xA4, 0xA6,       /* show RAM, not inverted         */
        0xAF              /* display on                     */
    };
    static const uint8_t blank[OLED_W] = { 0 };
    uint8_t p, x;

    if (oled_cmds(init_seq, (uint16_t)sizeof(init_seq))) { oled_ok = 0; return 1; }
    for (p = 0; p < OLED_PAGES; p++) {          /* clear the panel RAM     */
        if (oled_send_page(p, blank)) { oled_ok = 0; return 1; }
        for (x = 0; x < (uint8_t)OLED_W; x++) fb_sent[p][x] = 0x00;
    }
    oled_inverted = 0;
    oled_ok = 1;
    return 0;
}

/* whole-screen invert in one command, used to flash during an alarm */
static void oled_set_invert(uint8_t on)
{
    uint8_t cmd;
    if (!oled_ok || on == oled_inverted) return;
    cmd = on ? 0xA7U : 0xA6U;
    if (oled_cmds(&cmd, 1) == 0) oled_inverted = on;
}

/* Send only the pages that changed, at most OLED_PAGES_PER_TICK per call,
   so the 10 Hz sensor loop never waits on the display. */
static void oled_service(void)
{
    static uint8_t next_page = 0;
    uint8_t checked, pushed = 0, p, x, same;

    if (!oled_ok) return;
    for (checked = 0; checked < OLED_PAGES && pushed < OLED_PAGES_PER_TICK; checked++) {
        p = next_page;
        next_page = (uint8_t)((next_page + 1U) % OLED_PAGES);
        same = 1;
        for (x = 0; x < (uint8_t)OLED_W; x++) {
            if (fb[p][x] != fb_sent[p][x]) { same = 0; break; }
        }
        if (same) continue;
        if (oled_send_page(p, fb[p])) { oled_ok = 0; return; }
        for (x = 0; x < (uint8_t)OLED_W; x++) fb_sent[p][x] = fb[p][x];
        pushed++;
    }
}

/* ---- drawing primitives ---- */

static void fb_clear(void)
{
    uint8_t p, x;
    for (p = 0; p < OLED_PAGES; p++)
        for (x = 0; x < (uint8_t)OLED_W; x++) fb[p][x] = 0x00;
}

static void fb_px(int x, int y, uint8_t on)
{
    uint8_t *b;
    if (x < 0 || y < 0 || x >= OLED_W || y >= OLED_H) return;
    b = &fb[y >> 3][x];
    if (on) *b = (uint8_t)(*b | (1U << (y & 7)));
    else    *b = (uint8_t)(*b & ~(1U << (y & 7)));
}

static void fb_hline(int x, int y, int w, uint8_t on)
{
    while (w-- > 0) fb_px(x++, y, on);
}

static void fb_vline(int x, int y, int h, uint8_t on)
{
    while (h-- > 0) fb_px(x, y++, on);
}

static void fb_rect(int x, int y, int w, int h, uint8_t on)
{
    if (w <= 0 || h <= 0) return;
    fb_hline(x, y, w, on);
    fb_hline(x, y + h - 1, w, on);
    fb_vline(x, y, h, on);
    fb_vline(x + w - 1, y, h, on);
}

static void fb_fill(int x, int y, int w, int h, uint8_t on)
{
    int j;
    for (j = 0; j < h; j++) fb_hline(x, y + j, w, on);
}

static void fb_dotted_hline(int x, int y, int w, uint8_t on)
{
    int i;
    for (i = 0; i < w; i += 3) fb_px(x + i, y, on);
}

/* one 5x7 character, scale 1 = 6x8 cell, scale 2 = 12x16 cell */
static void fb_char(int x, int y, char c, int scale, uint8_t on)
{
    int col, row, sx, sy;
    uint8_t bits, idx;

    if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* uppercase font */
    if (c < 32 || c > 95) c = ' ';
    idx = (uint8_t)(c - 32);
    for (col = 0; col < 5; col++) {
        bits = font5x7[idx][col];
        for (row = 0; row < 7; row++) {
            if (bits & (1U << row)) {
                for (sx = 0; sx < scale; sx++)
                    for (sy = 0; sy < scale; sy++)
                        fb_px(x + col * scale + sx, y + row * scale + sy, on);
            }
        }
    }
}

static int text_width(const char *t, int scale)
{
    int n = 0;
    while (t[n]) n++;
    return (n == 0) ? 0 : (n * 6 - 1) * scale;
}

static int fb_text(int x, int y, const char *t, int scale, uint8_t on)
{
    while (*t) { fb_char(x, y, *t++, scale, on); x += 6 * scale; }
    return x;
}

static void fb_text_center(int y, const char *t, int scale, uint8_t on)
{
    fb_text((OLED_W - text_width(t, scale)) / 2, y, t, scale, on);
}

static void fb_text_right(int x_right, int y, const char *t, int scale, uint8_t on)
{
    fb_text(x_right - text_width(t, scale), y, t, scale, on);
}

/* outlined bar filled to pct (0..100) */
static void fb_bar(int x, int y, int w, int h, uint32_t pct)
{
    int fill;
    if (pct > 100U) pct = 100U;
    fb_rect(x, y, w, h, 1);
    fill = (int)(((uint32_t)(w - 4) * pct) / 100U);
    if (fill > 0) fb_fill(x + 2, y + 2, fill, h - 4, 1);
}

/* inverted title bar across the top: title left, extra text right */
static void fb_header(const char *title, const char *right, uint8_t beat)
{
    fb_fill(0, 0, OLED_W, 9, 1);
    fb_text(2, 1, title, 1, 0);
    if (right && right[0]) fb_text_right(OLED_W - 7, 1, right, 1, 0);
    fb_fill(OLED_W - 4, 3, 3, 3, (uint8_t)(beat ? 0U : 1U));   /* heartbeat */
}

/* inverted message bar across the bottom */
static void fb_footer(const char *t)
{
    fb_fill(0, OLED_H - 9, OLED_W, 9, 1);
    fb_text_center(OLED_H - 8, t, 1, 0);
}

/* ---- app state ---- */

#define ST_WARMUP     0U
#define ST_CALIBRATE  1U
#define ST_RUN        2U

static uint8_t      app_state;
static uint32_t     t_boot;
static uint32_t     cal_n;
static uint32_t     cal_sum[NUM_SENSORS];
static uint16_t     base_raw[NUM_SENSORS];
static feat_state_t feats;
static float        f[NUM_FEATURES];
static uint16_t     raw[NUM_SENSORS];

static uint8_t      votes[VOTE_WINDOW];
static uint8_t      vote_idx;
static uint8_t      shown_class;
static uint8_t      alarm_on;
static uint8_t      failsafe_hit;
static uint16_t     failsafe_count;
static uint16_t     clear_count;
static uint16_t     fault_count[NUM_SENSORS];
static uint8_t      sensor_fault;

static uint8_t      logging;
static int8_t       label = -1;
static uint16_t     run_id;

static uint32_t     infer_cycles;
static uint32_t     infer_cycles_max;
static uint32_t     selftest_until;
static uint32_t     last_status_ms;
static uint32_t     last_oled_retry;

/* AHT20 */
static uint8_t      aht_ok;                     /* chip answering         */
static uint8_t      have_rh;                    /* at least one reading   */
static uint8_t      aht_busy;                   /* measurement running    */
static uint8_t      aht_fail;
static uint8_t      aht_crc_fail;
static uint32_t     aht_t;                      /* last trigger time      */
static uint32_t     aht_retry;
static float        rh_now;                     /* %RH                    */
static float        temp_now;                   /* deg C                  */
static float        rh_avg;                     /* slow average, %RH      */
static int32_t      rh_c, rhb_c, temp_c;        /* x100 integers          */

/* OLED user interface */
static uint8_t      ui_page;                    /* 0..UI_PAGES-1          */
static char         toast_txt[22];              /* short bottom message   */
static uint32_t     toast_until;

/* alarm mute */
static uint8_t      muted;
static uint32_t     mute_until;

/* alarm event log */
typedef struct {
    uint32_t start_s;                           /* uptime when it began   */
    uint32_t dur_s;                             /* how long it lasted     */
    uint8_t  peak;                              /* highest MQ-6 gas index */
    uint8_t  failsafe;                          /* 1 = strong leak        */
    uint8_t  active;
} alarm_evt_t;

static alarm_evt_t  evts[EVT_MAX];
static uint8_t      evt_head;                   /* next slot to write     */
static uint16_t     evt_total;

/* 2-minute trend graphs, one point per second */
static uint8_t      trend6[TREND_LEN];          /* MQ-6 gas index 0..199  */
static uint8_t      trend_rh[TREND_LEN];        /* humidity x2 (0..200)   */
static uint8_t      trend_head;
static uint8_t      trend_n;
static uint32_t     last_trend_ms;

static const char *const sensor_name[NUM_SENSORS] = { "MQ6", "MQ2", "MQ135" };

/* Gas index for the display: 0 = clean air, 100 = fail-safe level.
   Uses the same Rs/R0 numbers the model sees. */
static uint8_t gas_index(float r)
{
    float g = (1.0f - r) / (1.0f - FAILSAFE_R6) * 100.0f;
    if (g < 0.0f)   g = 0.0f;
    if (g > 199.0f) g = 199.0f;
    return (uint8_t)(g + 0.5f);
}

/* two-digit number with leading zero */
static void str_u2(str_t *s, uint32_t v)
{
    if (v < 10U) str_c(s, '0');
    str_u(s, v);
}

/* seconds as HH:MM:SS */
static void str_hms(str_t *s, uint32_t sec)
{
    str_u2(s, sec / 3600U);       str_c(s, ':');
    str_u2(s, (sec / 60U) % 60U); str_c(s, ':');
    str_u2(s, sec % 60U);
}

/* short message shown on the bottom bar of the OLED */
static void toast(const char *t, uint32_t ms)
{
    uint8_t i = 0;
    while (t[i] && i < (uint8_t)(sizeof(toast_txt) - 1U)) { toast_txt[i] = t[i]; i++; }
    toast_txt[i] = '\0';
    toast_until = g_ms + ms;
}

static void read_sensors(void)
{
    raw[S_MQ6]   = adc_avg16(ADC_CH_MQ6);
    raw[S_MQ2]   = adc_avg16(ADC_CH_MQ2);
    raw[S_MQ135] = adc_avg16(ADC_CH_MQ135);
}

static const char *raw_health(uint16_t v)
{
    if (v < RAW_LOW_FAULT)  return "NO SIGNAL";
    if (v > RAW_HIGH_FAULT) return "DIVIDER FAULT";
    return "OK";
}

static void print_baseline(void)
{
    char b[64];
    str_t s;
    str_init(&s, b, sizeof(b));
    str_s(&s, "B,");
    str_u(&s, base_raw[S_MQ6]);   str_c(&s, ',');
    str_u(&s, base_raw[S_MQ2]);   str_c(&s, ',');
    str_u(&s, base_raw[S_MQ135]); str_s(&s, "\r\n");
    uart_puts(b);
}

static void print_help(void)
{
    uart_puts("\r\n# COMMANDS\r\n"
              "# L START LOG   R STOP LOG\r\n"
              "# 0 CLEAN 1 LPG 2 SMOKE 3 STEAM 4 VOC (LABEL)\r\n"
              "# X CLEAR LABEL\r\n"
              "# C CALIBRATE NOW (CLEAN AIR)\r\n"
              "# T SELF TEST   N NEXT OLED PAGE\r\n"
              "# M MUTE ALARM 60 S   E ALARM EVENT LOG\r\n"
              "# ? HELP\r\n"
              "# KEY: SHORT = NEXT PAGE / MUTE, HOLD 1 S = TEST, HOLD 4 S = CALIBRATE\r\n");
}

static void start_calibration(void)
{
    uint8_t i;
    app_state = ST_CALIBRATE;
    cal_n = 0;
    for (i = 0; i < NUM_SENSORS; i++) cal_sum[i] = 0;
    alarm_on = 0;
    muted = 0;
    g_buz_mode = BUZ_OFF;
    uart_puts("# CALIBRATING, KEEP SENSORS IN CLEAN AIR\r\n");
    toast("CALIBRATING...", 2000U);
}

/* Silence the buzzer for MUTE_MS. Refused during a strong leak: the
   fail-safe must always be heard. The OLED and LED keep flashing. */
static void try_mute(uint32_t now)
{
    if (!alarm_on) {
        toast("NO ALARM TO MUTE", 1500U);
        return;
    }
    if (failsafe_hit) {
        toast("STRONG LEAK: NO MUTE", 2500U);
        uart_puts("# MUTE REFUSED, STRONG LEAK\r\n");
        return;
    }
    muted = 1;
    mute_until = now + MUTE_MS;
    toast("BUZZER MUTED 60 S", 2000U);
    uart_puts("# BUZZER MUTED FOR 60 S\r\n");
}

/* ---- alarm event log ---- */
static alarm_evt_t *evt_current(void)
{
    uint8_t i = (uint8_t)((evt_head + EVT_MAX - 1U) % EVT_MAX);
    return &evts[i];
}

static void evt_begin(uint32_t now, uint8_t is_failsafe)
{
    alarm_evt_t *e = &evts[evt_head];
    e->start_s  = now / 1000U;
    e->dur_s    = 0;
    e->peak     = 0;
    e->failsafe = is_failsafe;
    e->active   = 1;
    evt_head = (uint8_t)((evt_head + 1U) % EVT_MAX);
    if (evt_total < 65535U) evt_total++;
}

static void evt_update(uint32_t now, uint8_t idx6, uint8_t is_failsafe)
{
    alarm_evt_t *e;
    if (evt_total == 0U) return;
    e = evt_current();
    if (!e->active) return;
    if (idx6 > e->peak) e->peak = idx6;
    if (is_failsafe) e->failsafe = 1;
    e->dur_s = now / 1000U - e->start_s;
}

static void evt_end(uint32_t now)
{
    alarm_evt_t *e;
    if (evt_total == 0U) return;
    e = evt_current();
    e->dur_s = now / 1000U - e->start_s;
    e->active = 0;
}

static void print_events(void)
{
    char b[80];
    str_t s;
    uint8_t n, k, i;
    alarm_evt_t *e;

    str_init(&s, b, sizeof(b));
    str_s(&s, "# ALARM EVENTS, TOTAL "); str_u(&s, evt_total); str_s(&s, "\r\n");
    uart_puts(b);
    n = (evt_total < EVT_MAX) ? (uint8_t)evt_total : (uint8_t)EVT_MAX;
    for (k = 0; k < n; k++) {                   /* newest first            */
        i = (uint8_t)((evt_head + EVT_MAX - 1U - k) % EVT_MAX);
        e = &evts[i];
        str_init(&s, b, sizeof(b));
        str_s(&s, "# "); str_u(&s, (uint32_t)k + 1U);
        str_s(&s, " AT "); str_hms(&s, e->start_s);
        str_s(&s, " FOR "); str_u(&s, e->dur_s); str_s(&s, " S PEAK ");
        str_u(&s, e->peak); str_s(&s, "% BY ");
        str_s(&s, e->failsafe ? "FAIL-SAFE" : "MODEL");
        if (e->active) str_s(&s, " (ACTIVE)");
        str_s(&s, "\r\n");
        uart_puts(b);
    }
}

static void self_test(void)
{
    char b[96];
    str_t s;
    uint8_t i;

    chirp(300);
    selftest_until = g_ms + 4000U;
    read_sensors();

    uart_puts("# SELF TEST\r\n");
    for (i = 0; i < NUM_SENSORS; i++) {
        str_init(&s, b, sizeof(b));
        str_s(&s, "# "); str_s(&s, sensor_name[i]); str_s(&s, " RAW ");
        str_u(&s, raw[i]); str_c(&s, ' '); str_s(&s, raw_health(raw[i]));
        str_s(&s, "\r\n");
        uart_puts(b);
    }
    str_init(&s, b, sizeof(b));
    str_s(&s, "# AHT20 ");
    if (aht_ok && have_rh) {
        str_s(&s, "OK ");  str_f(&s, temp_now, 1); str_s(&s, " C ");
        str_f(&s, rh_now, 1); str_s(&s, " %RH");
    } else {
        str_s(&s, "NOT FOUND, CHECK PB6 PB7 AND 3V3");
    }
    str_s(&s, "\r\n");
    uart_puts(b);
    str_init(&s, b, sizeof(b));
    str_s(&s, "# OLED "); str_s(&s, oled_ok ? "OK" : "NOT FOUND");
    str_s(&s, "\r\n# MODEL "); str_s(&s, MODEL_IS_TRAINED ? "TRAINED TREE" : "PLACEHOLDER RULES");
    str_s(&s, "\r\n# INFERENCE ");
    str_u(&s, infer_cycles_max); str_s(&s, " CYCLES = ");
    str_u(&s, infer_cycles_max / (CPU_HZ / 1000000U)); str_s(&s, " US\r\n");
    uart_puts(b);
}

static void handle_command(char c)
{
    char b[48];
    str_t s;

    if (c >= 'a' && c <= 'z') c = (char)(c - 32);

    switch (c) {
    case 'L':
        logging = 1;
        run_id++;
        uart_puts("# LOG START\r\n");
        toast("LOGGING STARTED", 1500U);
        if (app_state == ST_RUN) print_baseline();
        else uart_puts("# WAITING FOR CALIBRATION, LOG STARTS AFTER\r\n");
        break;
    case 'R':
        logging = 0;
        uart_puts("# LOG STOP\r\n");
        toast("LOGGING STOPPED", 1500U);
        break;
    case '0': case '1': case '2': case '3': case '4':
        label = (int8_t)(c - '0');
        run_id++;
        str_init(&s, b, sizeof(b));
        str_s(&s, "# LABEL "); str_u(&s, (uint32_t)label);
        str_s(&s, " "); str_s(&s, class_name[(uint8_t)label]);
        str_s(&s, " RUN "); str_u(&s, run_id); str_s(&s, "\r\n");
        uart_puts(b);
        str_init(&s, b, sizeof(b));
        str_s(&s, "LABEL "); str_u(&s, (uint32_t)label); str_c(&s, ' ');
        str_s(&s, class_short[(uint8_t)label]);
        toast(b, 1500U);
        chirp(60);
        break;
    case 'X':
        label = -1;
        uart_puts("# LABEL CLEARED\r\n");
        toast("LABEL CLEARED", 1500U);
        break;
    case 'C':
        start_calibration();
        break;
    case 'T':
        self_test();
        break;
    case 'N':
        ui_page = (uint8_t)((ui_page + 1U) % UI_PAGES);
        break;
    case 'M':
        try_mute(g_ms);
        break;
    case 'E':
        print_events();
        break;
    case '?': case 'H':
        print_help();
        break;
    default:
        break;                                  /* ignore CR, LF, others */
    }
}

/* seconds of warm-up left, never negative */
static uint32_t warmup_left_s(uint32_t now)
{
    uint32_t elapsed = (now - t_boot) / 1000U;
    return (elapsed >= WARMUP_SECONDS) ? 0U : (WARMUP_SECONDS - elapsed);
}

/* Run the AHT20 once a second without ever blocking the 10 Hz loop:
   tick n triggers a measurement, tick n+1 (100 ms later) reads it. */
static void aht_service(uint32_t now)
{
    uint8_t r;
    float rh, t;

    if (!aht_ok) {
        if ((now - aht_retry) >= 10000U) {
            aht_retry = now;
            if (aht_init() == 0) {
                aht_ok = 1; aht_fail = 0; aht_busy = 0;
                uart_puts("# AHT20 FOUND\r\n");
            }
        }
        return;
    }

    if (!aht_busy) {
        if ((now - aht_t) >= 1000U) {
            if (aht_trigger() == 0) { aht_busy = 1; aht_t = now; }
            else if (++aht_fail >= 5U) { aht_ok = 0; uart_puts("# AHT20 LOST\r\n"); }
        }
        return;
    }

    if ((now - aht_t) < 100U) return;           /* not ready yet          */
    r = aht_fetch(&rh, &t);
    if (r == 2U && (now - aht_t) < 400U) return;  /* busy: try next tick  */
    aht_busy = 0;

    if (r == 0U) {
        rh_now = rh; temp_now = t;
        if (!have_rh) rh_avg = rh;
        have_rh = 1; aht_fail = 0; aht_crc_fail = 0;
    } else if (r == 3U) {
        if (++aht_crc_fail >= 5U && aht_crc_on) {
            aht_crc_on = 0;
            uart_puts("# AHT20 CRC NOT SUPPORTED, IGNORING CRC\r\n");
        }
    } else if (++aht_fail >= 5U) {
        aht_ok = 0; have_rh = 0;
        uart_puts("# AHT20 LOST\r\n");
    }
}

/* Update the slow humidity average and the x100 integers used by the
   features and the log. Called once per 100 ms sample. */
static void humidity_update(void)
{
    if (aht_ok && have_rh) {
        rh_avg += (rh_now - rh_avg) * RH_BASE_ALPHA;
        rh_c   = (int32_t)(rh_now * 100.0f + 0.5f);
        rhb_c  = (int32_t)(rh_avg * 100.0f + 0.5f);
        temp_c = (int32_t)(temp_now * 100.0f + (temp_now >= 0.0f ? 0.5f : -0.5f));
    } else {
        rh_c = 0; rhb_c = 0; temp_c = 0;
    }
}

static void str_env(str_t *s)                   /* "T 28.5C RH 61.2%"  */
{
    if (aht_ok && have_rh) {
        str_s(s, "T "); str_f(s, temp_now, 1); str_s(s, "C  RH ");
        str_f(s, rh_now, 1); str_c(s, '%');
    } else {
        str_s(s, "TEMP/RH: NO AHT20");
    }
}

/* ---- display composition ---- */
/* Everything is drawn into the frame buffer fb[] every 100 ms, then
   oled_service() sends only the 8-pixel pages that actually changed.  */

/* right side of the header: "REC" while logging, else "2/5" */
static void header_right(char *b, uint8_t page)
{
    str_t s;
    str_init(&s, b, 8);
    if (logging) { str_s(&s, "REC"); return; }
    str_u(&s, (uint32_t)page + 1U); str_c(&s, '/'); str_u(&s, UI_PAGES);
}

/* first sensor with a wiring fault, or NUM_SENSORS if none */
static uint8_t first_faulty_sensor(void)
{
    uint8_t i;
    for (i = 0; i < NUM_SENSORS; i++)
        if (fault_count[i] >= FAULT_HOLD) return i;
    return NUM_SENSORS;
}

/* ---- start-up ---- */

static void draw_warmup(uint32_t now)
{
    char b[24];
    str_t s;
    uint32_t left = warmup_left_s(now);
    uint32_t done = WARMUP_SECONDS - left;

    fb_header("STARTING UP", "", (uint8_t)((now / 500U) & 1U));
    fb_text_center(12, "WARM UP", 2, 1);
    fb_bar(4, 30, OLED_W - 8, 8, (done * 100U) / WARMUP_SECONDS);

    str_init(&s, b, sizeof(b));
    str_s(&s, "READY IN "); str_u(&s, left); str_s(&s, " S");
    fb_text_center(41, b, 1, 1);

    str_init(&s, b, sizeof(b));
    if (((now / 3000U) & 1U) && aht_ok && have_rh) str_env(&s);
    else str_s(&s, "HOLD KEY 4S: SKIP");
    fb_text_center(52, b, 1, 1);
}

static void draw_calibrate(uint32_t now)
{
    char b[24];
    str_t s;
    uint32_t pct = (cal_n * 100U) / CAL_SAMPLES;

    fb_header("CALIBRATING", "", (uint8_t)((now / 500U) & 1U));
    fb_text_center(13, "KEEP IN CLEAN AIR", 1, 1);
    fb_bar(4, 25, OLED_W - 8, 10, pct);
    str_init(&s, b, sizeof(b));
    str_u(&s, pct); str_c(&s, '%');
    fb_text_center(39, b, 2, 1);
    fb_text_center(56, "NO GAS NEAR SENSORS", 1, 1);
}

static void draw_selftest(uint32_t now)
{
    char b[24];
    str_t s;
    uint8_t i;

    fb_header("SELF TEST", "", (uint8_t)((now / 500U) & 1U));
    for (i = 0; i < NUM_SENSORS; i++) {
        str_init(&s, b, sizeof(b));
        str_s(&s, sensor_name[i]);
        while (s.len < 6U) str_c(&s, ' ');
        str_u(&s, raw[i]); str_c(&s, ' ');
        str_s(&s, raw_health(raw[i]));
        fb_text(0, 11 + 8 * (int)i, b, 1, 1);
    }
    str_init(&s, b, sizeof(b));
    if (aht_ok && have_rh) {
        str_s(&s, "AHT20 OK ");
        str_f(&s, temp_now, 0); str_s(&s, "C ");
        str_f(&s, rh_now, 0); str_c(&s, '%');
    } else {
        str_s(&s, "AHT20 MISSING");
    }
    fb_text(0, 35, b, 1, 1);

    str_init(&s, b, sizeof(b));
    str_s(&s, MODEL_IS_TRAINED ? "MODEL TREE " : "MODEL RULES ");
    str_u(&s, infer_cycles_max / (CPU_HZ / 1000000U)); str_s(&s, " US");
    fb_text(0, 43, b, 1, 1);
    fb_text(0, 51, "BUZZER: LISTEN", 1, 1);
}

/* ---- pages ---- */

/* Page 1: the main screen. Big status, class, climate, LPG bar. */
static void draw_dashboard(uint32_t now, const char *hdr)
{
    char b[24];
    str_t s;
    uint8_t idx6 = gas_index(f[F_R6]);
    uint8_t bad = first_faulty_sensor();
    uint8_t conf = 0, i;

    for (i = 0; i < VOTE_WINDOW; i++) if (votes[i] == shown_class) conf++;

    fb_header("DASHBOARD", hdr, (uint8_t)((now / 500U) & 1U));

    if (alarm_on)          fb_text_center(12, "GAS LEAK!", 2, 1);
    else if (sensor_fault) fb_text_center(12, "FAULT", 2, 1);
    else                   fb_text_center(12, "SAFE", 2, 1);

    str_init(&s, b, sizeof(b));
    if (alarm_on && failsafe_hit) {
        str_s(&s, "FAIL-SAFE TRIGGERED");
    } else if (sensor_fault && bad < NUM_SENSORS) {
        str_s(&s, sensor_name[bad]); str_s(&s, ": "); str_s(&s, raw_health(raw[bad]));
    } else {
        str_s(&s, "AIR: "); str_s(&s, class_short[shown_class]);
        str_c(&s, ' '); str_u(&s, (uint32_t)conf * 10U); str_c(&s, '%');
    }
    fb_text_center(30, b, 1, 1);

    str_init(&s, b, sizeof(b));
    if (muted) {
        str_s(&s, "MUTED ");
        str_u(&s, (uint32_t)(mute_until - now) / 1000U); str_s(&s, " S");
    } else {
        str_env(&s);
    }
    fb_text_center(40, b, 1, 1);

    fb_text(0, 53, "LPG", 1, 1);
    fb_bar(20, 51, 82, 11, idx6);
    str_init(&s, b, sizeof(b));
    str_u(&s, idx6 > 100U ? 100U : idx6); str_c(&s, '%');
    fb_text_right(OLED_W, 53, b, 1, 1);
}

/* Page 2: all three gas sensors with Rs/R0, raw value and a bar. */
static void draw_sensors(uint32_t now, const char *hdr)
{
    char b[24];
    str_t s;
    uint8_t i, g;
    int y;
    float r;

    fb_header("SENSORS", hdr, (uint8_t)((now / 500U) & 1U));
    for (i = 0; i < NUM_SENSORS; i++) {
        y = 11 + 18 * (int)i;
        r = f[i];                               /* F_R6, F_R2, F_R135     */
        g = gas_index(r);
        str_init(&s, b, sizeof(b));
        str_s(&s, sensor_name[i]);
        while (s.len < 6U) str_c(&s, ' ');
        str_f(&s, r, 2);
        str_s(&s, "  RAW "); str_u(&s, raw[i]);
        fb_text(0, y, b, 1, 1);
        fb_bar(0, y + 8, 104, 7, g);
        str_init(&s, b, sizeof(b));
        str_u(&s, g > 100U ? 100U : g); str_c(&s, '%');
        fb_text_right(OLED_W, y + 8, b, 1, 1);
    }
}

/* Page 3: last ~2 minutes of the MQ-6 gas index as a filled graph. */
static void draw_trend(uint32_t now, const char *hdr)
{
    char b[24];
    str_t s;
    uint8_t k, v, peak = 0;
    int x, top, gy0 = 11, gh = 40;              /* graph area y 11..50    */

    fb_header("MQ6 TREND", hdr, (uint8_t)((now / 500U) & 1U));
    fb_dotted_hline(0, gy0, OLED_W, 1);         /* 100% = fail-safe level */
    fb_hline(0, gy0 + gh, OLED_W, 1);           /* zero line              */

    for (k = 0; k < trend_n; k++) {
        /* oldest on the left, newest at the right edge */
        uint8_t i = (uint8_t)((trend_head + TREND_LEN - trend_n + k) % TREND_LEN);
        v = trend6[i];
        if (v > peak) peak = v;
        if (v > 100U) v = 100U;
        x = OLED_W - (int)trend_n + (int)k;
        top = gy0 + gh - ((int)v * gh) / 100;
        fb_vline(x, top, gy0 + gh - top, 1);
    }

    str_init(&s, b, sizeof(b));
    str_s(&s, "NOW "); str_u(&s, gas_index(f[F_R6])); str_s(&s, "%  PEAK ");
    str_u(&s, peak); str_c(&s, '%');
    fb_text(0, 55, b, 1, 1);
}

/* Page 4: temperature and humidity, big, with a humidity graph. */
static void draw_climate(uint32_t now, const char *hdr)
{
    char b[24];
    str_t s;
    uint8_t k, v, lo = 200, hi = 0;
    int x, y, prev_y = -1, gy0 = 41, gh = 22, span;

    fb_header("CLIMATE", hdr, (uint8_t)((now / 500U) & 1U));
    if (!(aht_ok && have_rh)) {
        fb_text_center(22, "AHT20 NOT FOUND", 1, 1);
        fb_text_center(34, "CHECK B6 B7 3V3", 1, 1);
        fb_text_center(46, "RETRYING EVERY 10 S", 1, 1);
        return;
    }

    str_init(&s, b, sizeof(b));
    str_f(&s, temp_now, 1); str_c(&s, 'C');
    fb_text(0, 12, b, 2, 1);
    str_init(&s, b, sizeof(b));
    str_f(&s, rh_now, 0); str_c(&s, '%');
    fb_text_right(OLED_W, 12, b, 2, 1);

    str_init(&s, b, sizeof(b));
    str_s(&s, "AVG "); str_f(&s, rh_avg, 1);
    str_s(&s, "% RISE ");
    if (rh_now >= rh_avg) str_c(&s, '+');
    str_f(&s, rh_now - rh_avg, 1);
    fb_text(0, 31, b, 1, 1);

    /* humidity graph, auto-scaled, at least a 10 %RH window */
    for (k = 0; k < trend_n; k++) {
        v = trend_rh[(uint8_t)((trend_head + TREND_LEN - trend_n + k) % TREND_LEN)];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (trend_n == 0U) { lo = 0; hi = 20; }
    if ((int)hi - (int)lo < 20) { hi = (uint8_t)(lo + 20U > 200U ? 200U : lo + 20U); }
    span = (int)hi - (int)lo;
    if (span <= 0) span = 1;
    fb_rect(0, gy0 - 1, OLED_W, gh + 2, 1);
    for (k = 0; k < trend_n; k++) {
        v = trend_rh[(uint8_t)((trend_head + TREND_LEN - trend_n + k) % TREND_LEN)];
        x = OLED_W - 1 - (int)trend_n + (int)k;
        if (x < 1) continue;
        y = gy0 + gh - 1 - (((int)v - (int)lo) * (gh - 1)) / span;
        if (prev_y >= 0) {
            int a = prev_y < y ? prev_y : y, c = prev_y < y ? y : prev_y;
            fb_vline(x, a, c - a + 1, 1);
        } else {
            fb_px(x, y, 1);
        }
        prev_y = y;
    }
}

/* Page 5: model, timing, uptime, alarm history, logging. */
static void draw_system(uint32_t now, const char *hdr)
{
    char b[24];
    str_t s;
    alarm_evt_t *e;

    fb_header("SYSTEM", hdr, (uint8_t)((now / 500U) & 1U));

    fb_text(0, 11, MODEL_IS_TRAINED ? "MODEL: TRAINED TREE" : "MODEL: RULES", 1, 1);

    str_init(&s, b, sizeof(b));
    str_s(&s, "INFERENCE: ");
    str_u(&s, infer_cycles / (CPU_HZ / 1000000U)); str_s(&s, " US");
    fb_text(0, 19, b, 1, 1);

    str_init(&s, b, sizeof(b));
    str_s(&s, "UPTIME "); str_hms(&s, now / 1000U);
    fb_text(0, 27, b, 1, 1);

    str_init(&s, b, sizeof(b));
    str_s(&s, "ALARMS: "); str_u(&s, evt_total);
    fb_text(0, 35, b, 1, 1);

    str_init(&s, b, sizeof(b));
    if (evt_total) {
        e = evt_current();
        str_s(&s, "LAST "); str_hms(&s, e->start_s);
        str_c(&s, ' '); str_u(&s, e->peak); str_c(&s, '%');
    } else {
        str_s(&s, "NO ALARMS YET");
    }
    fb_text(0, 43, b, 1, 1);

    str_init(&s, b, sizeof(b));
    if (logging) {
        str_s(&s, "LOG ON L:");
        if (label < 0) str_c(&s, '-'); else str_u(&s, (uint32_t)label);
        str_s(&s, " RUN "); str_u(&s, run_id);
    } else {
        str_s(&s, "BASE "); str_u(&s, base_raw[0]); str_c(&s, ' ');
        str_u(&s, base_raw[1]); str_c(&s, ' '); str_u(&s, base_raw[2]);
    }
    fb_text(0, 51, b, 1, 1);
}

/* bottom bar: button-hold hint has priority, then short messages */
static void draw_overlays(uint32_t now)
{
    uint32_t held;

    if (g_btn_down) {
        held = now - g_btn_t_down;
        if (held >= CAL_PRESS_MS)       { fb_footer("RELEASE: CALIBRATE"); return; }
        else if (held >= TEST_PRESS_MS) { fb_footer("RELEASE: SELF TEST"); return; }
    }
    if ((int32_t)(selftest_until - now) > 0) return;   /* keep test rows visible */
    if ((int32_t)(toast_until - now) > 0) fb_footer(toast_txt);
}

static void compose_display(uint32_t now)
{
    char hdr[8];

    fb_clear();
    header_right(hdr, ui_page);

    if ((int32_t)(selftest_until - now) > 0) draw_selftest(now);
    else if (app_state == ST_WARMUP)         draw_warmup(now);
    else if (app_state == ST_CALIBRATE)      draw_calibrate(now);
    else if (alarm_on)                       draw_dashboard(now, hdr);   /* alarm forces page 1 */
    else {
        switch (ui_page) {
        case 0:  draw_dashboard(now, hdr); break;
        case 1:  draw_sensors(now, hdr);   break;
        case 2:  draw_trend(now, hdr);     break;
        case 3:  draw_climate(now, hdr);   break;
        default: draw_system(now, hdr);    break;
        }
    }
    draw_overlays(now);

    /* flash the whole screen twice a second during an alarm */
    oled_set_invert((uint8_t)(alarm_on && ((now / 500U) & 1U)));
}

/* ---- one 100 ms step ---- */

static void step_run(uint32_t now)
{
    uint8_t pred, i, lpg_votes = 0, best = 0;
    uint8_t count[NUM_CLASSES] = {0, 0, 0, 0, 0};
    uint8_t want_alarm;
    uint32_t c0;

    c0 = DWT_CYCCNT_REG;
    feat_compute(&feats, raw, rh_c, rhb_c, f);
    pred = model_classify(f);
    infer_cycles = DWT_CYCCNT_REG - c0;
    if (infer_cycles > infer_cycles_max) infer_cycles_max = infer_cycles;
    if (pred >= NUM_CLASSES) pred = CLS_CLEAN;

    /* majority vote over the last second */
    votes[vote_idx] = pred;
    vote_idx = (uint8_t)((vote_idx + 1U) % VOTE_WINDOW);
    for (i = 0; i < VOTE_WINDOW; i++) count[votes[i]]++;
    lpg_votes = count[CLS_LPG];
    for (i = 1; i < NUM_CLASSES; i++) if (count[i] > count[best]) best = i;
    shown_class = best;

    /* fail-safe: high MQ-6 response alarms regardless of the model */
    if (f[F_R6] < FAILSAFE_R6) { if (failsafe_count < 1000U) failsafe_count++; }
    else                       failsafe_count = 0;

    want_alarm = (uint8_t)((lpg_votes >= LPG_VOTES_TO_ALARM) || (failsafe_count >= FAILSAFE_HOLD));

    if (want_alarm) {
        if (!alarm_on) {
            alarm_on = 1;
            failsafe_hit = (uint8_t)(failsafe_count >= FAILSAFE_HOLD);
            uart_puts("# ALARM ON\r\n");
            evt_begin(now, failsafe_hit);
        } else if (failsafe_count >= FAILSAFE_HOLD) {
            failsafe_hit = 1;
        }
        clear_count = 0;
        evt_update(now, gas_index(f[F_R6]), failsafe_hit);
    } else if (alarm_on) {
        if (++clear_count >= CLEAR_SAMPLES) {   /* hold 5 s before clearing */
            alarm_on = 0;
            failsafe_hit = 0;
            clear_count = 0;
            muted = 0;
            evt_end(now);
            uart_puts("# ALARM CLEARED\r\n");
            toast("ALARM CLEARED", 2500U);
        }
    }
}

static void check_sensor_faults(void)
{
    uint8_t i, any = 0;
    for (i = 0; i < NUM_SENSORS; i++) {
        if (raw[i] < RAW_LOW_FAULT || raw[i] > RAW_HIGH_FAULT) {
            if (fault_count[i] < 1000U) fault_count[i]++;
        } else {
            fault_count[i] = 0;
        }
        if (fault_count[i] >= FAULT_HOLD) any = 1;
    }
    if (any && !sensor_fault) uart_puts("# SENSOR FAULT, RUN T FOR DETAILS\r\n");
    sensor_fault = any;
}

static void send_log_line(uint32_t ms)
{
    char b[80];
    str_t s;
    str_init(&s, b, sizeof(b));
    str_s(&s, "D,");
    str_u(&s, ms);                  str_c(&s, ',');
    str_u(&s, run_id);              str_c(&s, ',');
    str_i(&s, label);               str_c(&s, ',');
    str_u(&s, raw[S_MQ6]);          str_c(&s, ',');
    str_u(&s, raw[S_MQ2]);          str_c(&s, ',');
    str_u(&s, raw[S_MQ135]);        str_c(&s, ',');
    str_i(&s, rh_c);                str_c(&s, ',');
    str_i(&s, temp_c);              str_c(&s, ',');
    str_i(&s, rhb_c);               str_s(&s, "\r\n");
    uart_puts(b);
}

static void send_status_line(uint32_t ms)
{
    char b[96];
    str_t s;
    str_init(&s, b, sizeof(b));
    str_s(&s, "S,");
    str_u(&s, ms / 1000U);          str_c(&s, ',');
    str_f(&s, f[F_R6], 3);          str_c(&s, ',');
    str_f(&s, f[F_R2], 3);          str_c(&s, ',');
    str_f(&s, f[F_R135], 3);        str_c(&s, ',');
    if (aht_ok && have_rh) { str_f(&s, rh_now, 1); str_c(&s, ','); str_f(&s, temp_now, 1); }
    else                   { str_s(&s, "-,-"); }
    str_c(&s, ',');
    str_s(&s, class_name[shown_class]); str_c(&s, ',');
    str_s(&s, alarm_on ? "ALARM" : (sensor_fault ? "FAULT" : "SAFE"));
    str_s(&s, "\r\n");
    uart_puts(b);
}

/* ---- main ---- */

int main(void)
{
    uint32_t now;
    uint8_t i;
    int16_t c;
    uint8_t btn;

    clock_init();
    gpio_init();
    SysTick_Config(CPU_HZ / TICK_HZ);           /* 4 kHz tick              */
    NVIC_SetPriority(SysTick_IRQn, 2);          /* equal priorities: worst-case */
                                                /* stack stays under 1 KB      */

    DEMCR_REG    |= (1UL << 24);                /* enable DWT cycle counter */
    DWT_CYCCNT_REG = 0;
    DWT_CTRL_REG |= 1UL;

    uart_init();
    button_init();
    adc_init();
    i2c_init();
    delay_ms(100);                              /* OLED power-up            */
    (void)oled_init();
    aht_ok = (uint8_t)(aht_init() == 0);

    for (i = 0; i < VOTE_WINDOW; i++) votes[i] = CLS_CLEAN;
    shown_class = CLS_CLEAN;
    for (i = 0; i < NUM_FEATURES; i++) f[i] = 1.0f;

    uart_puts("\r\n# GAS LEAK DETECTOR READY\r\n");
    uart_puts(oled_ok ? "# OLED OK\r\n" : "# OLED NOT FOUND, CHECK PB6 PB7\r\n");
    uart_puts(aht_ok ? "# AHT20 OK\r\n" : "# AHT20 NOT FOUND, CHECK PB6 PB7 3V3\r\n");
    print_help();
    chirp(120);

    t_boot = g_ms;
    app_state = ST_WARMUP;
    last_status_ms = t_boot;
    last_oled_retry = t_boot;
    aht_retry = t_boot;
    aht_t = t_boot - 1000U;                     /* first reading right away */
    g_sample_due = 0;                           /* every sample from here on */
                                                /* is stamped after t_boot   */

    for (;;) {
        while (!g_sample_due) { __WFI(); }
        g_sample_due = 0;
        now = g_sample_ms;

        /* ---- inputs ---- */
        while ((c = uart_getc()) >= 0) handle_command((char)c);

        btn = g_btn_event;
        g_btn_event = BTN_NONE;
        if (btn == BTN_SHORT) {
            if (alarm_on) try_mute(now);                    /* hush       */
            else if (app_state == ST_RUN) {
                ui_page = (uint8_t)((ui_page + 1U) % UI_PAGES);
                chirp(15);                                  /* click      */
            }
        }
        if (btn == BTN_TEST) self_test();
        if (btn == BTN_CAL)  { chirp(150); start_calibration(); }

        read_sensors();
        check_sensor_faults();
        aht_service(now);
        humidity_update();

        /* ---- state machine ---- */
        if (app_state == ST_WARMUP) {
            if (warmup_left_s(now) == 0U) {
                start_calibration();
            } else if ((now - last_status_ms) >= 5000U) {
                char b[48]; str_t s;
                last_status_ms = now;
                str_init(&s, b, sizeof(b));
                str_s(&s, "# WARMING UP, ");
                str_u(&s, warmup_left_s(now));
                str_s(&s, " S LEFT\r\n");
                uart_puts(b);
            }
        } else if (app_state == ST_CALIBRATE) {
            for (i = 0; i < NUM_SENSORS; i++) cal_sum[i] += raw[i];
            if (++cal_n >= CAL_SAMPLES) {
                uint8_t ok = 1;
                for (i = 0; i < NUM_SENSORS; i++) {
                    base_raw[i] = (uint16_t)(cal_sum[i] / CAL_SAMPLES);
                    if (base_raw[i] < RAW_LOW_FAULT || base_raw[i] > RAW_HIGH_FAULT) ok = 0;
                }
                if (ok) {
                    feat_reset(&feats, base_raw);
                    if (have_rh) rh_avg = rh_now;   /* fresh humidity baseline */
                    for (i = 0; i < VOTE_WINDOW; i++) votes[i] = CLS_CLEAN;
                    failsafe_count = 0;
                    clear_count = 0;
                    alarm_on = 0;
                    app_state = ST_RUN;
                    uart_puts("# CALIBRATION DONE\r\n");
                    toast("CALIBRATED", 2000U);
                    print_baseline();
                    chirp(80);
                } else {
                    uart_puts("# CALIBRATION FAILED, BAD SENSOR READING, RETRYING\r\n");
                    start_calibration();
                }
            }
        } else {
            step_run(now);
            if (logging) {
                send_log_line(now);
            } else if ((now - last_status_ms) >= 1000U) {
                last_status_ms = now;
                send_status_line(now);
            }
        }

        /* ---- outputs ---- */
        /* trend graphs: one point per second */
        if ((now - last_trend_ms) >= 1000U) {
            last_trend_ms = now;
            trend6[trend_head]   = (app_state == ST_RUN) ? gas_index(f[F_R6]) : 0U;
            trend_rh[trend_head] = (aht_ok && have_rh)
                                 ? (uint8_t)(rh_now >= 100.0f ? 200U : (uint32_t)(rh_now * 2.0f))
                                 : 0U;
            trend_head = (uint8_t)((trend_head + 1U) % TREND_LEN);
            if (trend_n < TREND_LEN) trend_n++;
        }

        /* mute ends after MUTE_MS, and never holds during a strong leak */
        if (muted && (!alarm_on || failsafe_hit || (int32_t)(mute_until - now) <= 0)) {
            if (alarm_on && failsafe_hit) uart_puts("# MUTE CANCELLED, STRONG LEAK\r\n");
            muted = 0;
        }

        if (alarm_on && !muted) g_buz_mode = BUZ_ALARM;
        else if (alarm_on)      g_buz_mode = BUZ_OFF;   /* muted: lights only */
        else if (sensor_fault)  g_buz_mode = BUZ_FAULT;
        else                    g_buz_mode = BUZ_OFF;

        if (alarm_on)                    { if ((now / 100U) & 1U) LED_ON(); else LED_OFF(); }
        else if (app_state != ST_RUN)    { if ((now / 500U) & 1U) LED_ON(); else LED_OFF(); }
        else                             { if ((now % 1000U) < 100U) LED_ON(); else LED_OFF(); }

        compose_display(now);
        if (!oled_ok && (now - last_oled_retry) >= 5000U) {
            last_oled_retry = now;
            if (oled_init() == 0) uart_puts("# OLED RECONNECTED\r\n");
        }
        oled_service();
    }
}
