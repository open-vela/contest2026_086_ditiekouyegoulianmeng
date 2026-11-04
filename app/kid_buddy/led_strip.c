/****************************************************************************
 * vendor/allwinnertech/apps/kid_buddy/led_strip.c
 *
 * 外接单色呼吸灯带。
 *
 * 硬件（详见 docs/呼吸灯带接线.md）：
 *   H4 第 1 脚 (PG13, BT_PCM_DOUT) --100R--> MOS 管栅极，栅极再用 10k 下拉；
 *   MOS 管源极接 GND，漏极接灯带负极，灯带正极接 5V（H6 第 7 脚）或外部电源。
 *
 * 软件：PG13 复用成 PWM2（功能 6），2 kHz。平时常亮；有「活动」时
 * （等模型回复、给家长回传故事记录）按 4 秒周期做 gamma 校正的呼吸。
 *
 * 为什么不用 /dev/pwmN：NuttX 的 PWM lower-half 在每次 set-duty 时都会走
 * hal_pwm_control()，而后者每次都会调 hal_pwm_pinctrl_init()，sys_config
 * 里没有 pwm2 时会退回硬编码的 PE8（摄像头 OV_D5）——每 20 ms 把摄像头
 * 数据脚改成 PWM 输出显然不行。所以这里只借 HAL 打开 PWM 总线时钟，
 * 通道 2 的寄存器自己配，PG13 也只 mux 一次。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>

#include <hal_gpio.h>
#include <sunxi_hal_pwm.h>

#include "led_strip.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PG13 的功能表（从原理图 SOC 页读出）：
 *   PG13/I2S1_BCLK/TWI0_SDA/RGMII_CLKIN/RMII_RXER/PWM2/LEDC_DO/UART1_RX
 * HAL 的 mux 编号是「GPIO 输入=0、输出=1、第一个复用=2」，所以 PWM2 是 6。
 */

#define STRIP_PIN            GPIOG(13)
#define STRIP_PWM_MUX        GPIO_MUXSEL_FUNCTION6
#define STRIP_PWM_CH         2

/* sun8iw20 PWM 控制器：通道 2 的寄存器。 */

#define STRIP_PWM_BASE       0x02000c00UL
#define PWM_REG(off)         (*(volatile uint32_t *)(STRIP_PWM_BASE + (off)))
#define LED_PWM_PCCR23           0x24
#define LED_PWM_PCGR             0x40
#define LED_PWM_PER              0x80
#define LED_PWM_PCR(n)           (0x0100 + 0x20 * (n))
#define LED_PWM_PPR(n)           (0x0104 + 0x20 * (n))

#define PWM_CLK_SRC_OSC24M   0              /* PCCR 的 clk_src = 0 */
#define PWM_CLK_DIV_M_1      0              /* div_m = 0 → 分频 1 */
#define PWM_PCR_ACTIVE_HIGH  (1u << 8)      /* PWM_POLARITY_NORMAL */

/* 2 kHz、24 MHz 源时钟 → 12000 个周期，不需要预分频。 */

#define STRIP_PWM_HZ         2000
#define STRIP_PERIOD_CYCLES  (24000000 / STRIP_PWM_HZ)

/* 呼吸：4 秒一轮，每 31 ms 走一档（128 档）。最高只开到 60%，灯带
 * 贴在设备上不需要全亮，也从源头上压住 5V 取电的电流。想更亮改这里。 */

#define STRIP_BREATH_MS      4000
#define STRIP_STEPS          128
#define STRIP_STEP_US        (STRIP_BREATH_MS * 1000 / STRIP_STEPS)
#define STRIP_MAX_PCT        60

/* 没有活动时的常亮亮度。比呼吸的峰值低，夜里不至于刺眼。 */

#define STRIP_STEADY_PCT     40
#define STRIP_IDLE_POLL_US   (100 * 1000)  /* 常亮时 10 Hz 看一眼活动位 */

/* 每秒把 PG13 的复用和通道使能重新写一遍。PG13 原是 BT_PCM_DOUT，WiFi/BT
 * 那套在开机、联网失败回退、射频开关的时候都会碰这一组脚；只要有一次把
 * 复用改回去、或者把 PWM 通道的时钟门控关掉，灯带就再也不亮了，而我们的
 * worker 还在照常算占空比——屏幕上看不出任何异常。与其去猜是谁改的，不如
 * 自己定期抢回来。 */

#define STRIP_REARM_US       (1000 * 1000)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 128 点、gamma 2.2 校正后的呼吸曲线，0~1000。
 * 直接把正弦当占空比的话，低亮度段变化太快、看着像在闪；按
 * duty = perceived^2.2 校正后才是均匀的「呼吸」。 */

static const uint16_t g_breath[STRIP_STEPS] =
{
     0,    0,    0,    0,    0,    0,    0,    0,    1,    1,    2,    3,
     4,    6,    8,   11,   15,   19,   24,   30,   37,   44,   54,   64,
    75,   88,  102,  118,  135,  154,  173,  195,  218,  242,  267,  294,
   322,  351,  381,  412,  444,  476,  509,  542,  575,  608,  641,  674,
   706,  737,  767,  796,  824,  850,  875,  897,  918,  937,  953,  967,
   979,  988,  995,  999, 1000,  999,  995,  988,  979,  967,  953,  937,
   918,  897,  875,  850,  824,  796,  767,  737,  706,  674,  641,  608,
   575,  542,  509,  476,  444,  412,  381,  351,  322,  294,  267,  242,
   218,  195,  173,  154,  135,  118,  102,   88,   75,   64,   54,   44,
    37,   30,   24,   19,   15,   11,    8,    6,    4,    3,    2,    1,
     1,    0,    0,    0,    0,    0,    0,    0,
};

static volatile int g_strip_run;
static pthread_t    g_strip_thread;
static int          g_strip_started;

/* 上一次写进去的占空比，重新接管通道时用它恢复亮度。 */

static int          g_strip_pct = STRIP_STEADY_PCT;

/* 复用/使能被别人改掉的次数，只在第一次和每 60 次打一行日志。 */

static unsigned int g_strip_rearm_hits;

/* 活动位由不同线程置/清（GUI 线程等模型，MQTT sync 线程发故事记录），
 * 所以用一个锁保护；worker 每 31 ms 读一次，开销可以忽略。 */

static pthread_mutex_t g_strip_activity_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int    g_strip_activity;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 只写通道 2 的 active-cycles 字段，period 字段保持不动。 */

void led_strip_set_pct(int pct)
{
  uint32_t active;
  uint32_t reg;

  if (pct < 0)
    {
      pct = 0;
    }
  else if (pct > 100)
    {
      pct = 100;
    }

  active = (uint32_t)STRIP_PERIOD_CYCLES * (uint32_t)pct / 100u;

  reg  = PWM_REG(LED_PWM_PPR(STRIP_PWM_CH));
  reg &= 0xffff0000u;                       /* 清掉 [15:0] active cycles */
  reg |= active & 0xffffu;
  PWM_REG(LED_PWM_PPR(STRIP_PWM_CH)) = reg;

  g_strip_pct = pct;
}

void led_strip_set_activity(unsigned int activity, int on)
{
  pthread_mutex_lock(&g_strip_activity_lock);

  if (on)
    {
      g_strip_activity |= activity;
    }
  else
    {
      g_strip_activity &= ~activity;
    }

  pthread_mutex_unlock(&g_strip_activity_lock);
}

static bool strip_activity_any(void)
{
  bool any;

  pthread_mutex_lock(&g_strip_activity_lock);
  any = (g_strip_activity != 0);
  pthread_mutex_unlock(&g_strip_activity_lock);

  return any;
}

static int strip_pwm_init(void)
{
  uint32_t reg;
  int      ret;

  /* 只借它打开 PWM 总线时钟。显示背光在 LCD 初始化时已经调用过一次，
   * 这里是引用计数 +1，不会重置控制器。反过来，这一步失败（时钟还没
   * 就绪）时后面所有权都没意义，必须让调用方知道。 */

  ret = (int)hal_pwm_init();
  if (ret != 0)
    {
      return ret;
    }

  /* 通道 2：24 MHz 源、不分频。 */

  reg  = PWM_REG(LED_PWM_PCCR23);
  reg &= ~((0x3u << 7) | 0xfu);             /* clk_src = OSC24M, div_m = 0 */
  PWM_REG(LED_PWM_PCCR23) = reg;

  /* 打开通道 2 的时钟门控。 */

  PWM_REG(LED_PWM_PCGR) |= (1u << STRIP_PWM_CH);

  /* 预分频 0，正常极性（占空比期间输出高）。 */

  PWM_REG(LED_PWM_PCR(STRIP_PWM_CH)) = PWM_PCR_ACTIVE_HIGH;

  /* 周期和初始占空比（0，灯带先灭）。 */

  PWM_REG(LED_PWM_PPR(STRIP_PWM_CH)) =
      ((STRIP_PERIOD_CYCLES - 1) << 16);

  /* 使能通道 2。 */

  PWM_REG(LED_PWM_PER) |= (1u << STRIP_PWM_CH);

  return 0;
}

/* 把 PG13 的复用和通道使能重新写一遍，顺便恢复当前亮度。 */

static void strip_rearm_tick(void)
{
  uint32_t ppr;
  bool     was_off;
  int      ret;

  was_off = (PWM_REG(LED_PWM_PER) & (1u << STRIP_PWM_CH)) == 0;

  ret = hal_gpio_pinmux_set_function(STRIP_PIN, STRIP_PWM_MUX);

  /* 控制器要是被复位过，周期寄存器会是 0，只补占空比没有意义。 */

  ppr = PWM_REG(LED_PWM_PPR(STRIP_PWM_CH));
  if ((ppr >> 16) == 0)
    {
      PWM_REG(LED_PWM_PPR(STRIP_PWM_CH)) =
          ((STRIP_PERIOD_CYCLES - 1) << 16) | (ppr & 0xffffu);
    }

  PWM_REG(LED_PWM_PCGR) |= (1u << STRIP_PWM_CH);
  PWM_REG(LED_PWM_PCR(STRIP_PWM_CH)) = PWM_PCR_ACTIVE_HIGH;
  PWM_REG(LED_PWM_PER) |= (1u << STRIP_PWM_CH);

  /* 写进去读不回来，说明总线时钟根本没开（hal_pwm_init 那步失败过），
   * 补一次再写。pwm_init 只在成功时才加计数，失败重试是安全的。 */

  if ((PWM_REG(LED_PWM_PER) & (1u << STRIP_PWM_CH)) == 0)
    {
      syslog(LOG_WARNING, "[led_strip] PWM 总线时钟没开，重新 hal_pwm_init\n");

      if (hal_pwm_init() == 0)
        {
          PWM_REG(LED_PWM_PCGR) |= (1u << STRIP_PWM_CH);
          PWM_REG(LED_PWM_PCR(STRIP_PWM_CH)) = PWM_PCR_ACTIVE_HIGH;
          PWM_REG(LED_PWM_PPR(STRIP_PWM_CH)) =
              ((STRIP_PERIOD_CYCLES - 1) << 16);
          PWM_REG(LED_PWM_PER) |= (1u << STRIP_PWM_CH);

          g_strip_rearm_hits++;
        }
    }

  led_strip_set_pct(g_strip_pct);

  if (was_off || ret != 0)
    {
      g_strip_rearm_hits++;

      /* 每秒一次，日志别刷屏：第一次说一声，之后每分钟一次。 */

      if (g_strip_rearm_hits == 1 || (g_strip_rearm_hits % 60) == 0)
        {
          syslog(LOG_WARNING,
                 "[led_strip] PG13/PWM2 被别人动过（%s，mux ret=%d），"
                 "已抢回（第 %u 次）\n",
                 was_off ? "通道被关" : "复用被改", ret, g_strip_rearm_hits);
        }
    }
}

static void *strip_worker(void *arg)
{
  unsigned int i = 0;
  unsigned int rearm_acc = 0;
  bool breathing = false;

  (void)arg;

  while (g_strip_run)
    {
      if (rearm_acc >= STRIP_REARM_US)
        {
          rearm_acc = 0;
          strip_rearm_tick();
        }

      if (strip_activity_any())
        {
          int pct;

          /* 从最暗一档重新起呼，不要在上一轮的位置上接着走；活动断断续续
           * 时也总是先暗后亮，看起来才像一次完整的呼吸。 */
          if (!breathing)
            {
              i = 0;
            }

          pct = ((int)g_breath[i] * STRIP_MAX_PCT + 500) / 1000;
          led_strip_set_pct(pct);

          i = (i + 1) & (STRIP_STEPS - 1);
          breathing = true;

          usleep(STRIP_STEP_US);
          rearm_acc += STRIP_STEP_US;
        }
      else
        {
          /* 活动全清：回到常亮。只在「刚停下」那一次写寄存器，之后每
           * 100 ms 轮询一次即可，不占 PWM。 */
          if (breathing)
            {
              led_strip_set_pct(STRIP_STEADY_PCT);
              breathing = false;
            }

          usleep(STRIP_IDLE_POLL_US);
          rearm_acc += STRIP_IDLE_POLL_US;
        }
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int led_strip_start(void)
{
  pthread_attr_t attr;
  int ret;

  if (g_strip_started)
    {
      return 0;
    }

  /* kid_buddy 由 rcS 很早就拉起来，GPIO 驱动不一定已经就绪；复用失败
   * 不等于这根脚不能当 PWM 用，给几次机会再放弃。 */

  {
    int tries;

    for (tries = 0; tries < 5; tries++)
      {
        ret = hal_gpio_pinmux_set_function(STRIP_PIN, STRIP_PWM_MUX);
        if (ret == 0)
          {
            break;
          }

        usleep(200 * 1000);
      }
  }

  if (ret != 0)
    {
      /* 不复用失败就退出：worker 每秒会再试一次，开机早期 GPIO 还没
       * 就绪、或者 WiFi 那头先把脚占了，都能在运行中补回来。 */

      syslog(LOG_WARNING,
             "[led_strip] PG13 mux to PWM2 failed: %d, worker 会继续重试\n",
             ret);
    }

  /* 下拉：PWM 还没启动的这段时间（以及将来复位时）栅极必须是低电平，
   * 否则 MOS 管会被 BT PCM 那一路的残留电平打开。外部 10k 下拉是主
   * 保障，这里是顺手加一层。 */

  hal_gpio_set_pull(STRIP_PIN, GPIO_PULL_DOWN);

  /* PWM 总线时钟要等 CCU 起来，开机早期可能还没好；和上面的复用一样
   * 给几次机会。全部失败也不退出——worker 起来以后还会自己补。 */

  {
    int tries;

    for (tries = 0; tries < 5; tries++)
      {
        ret = strip_pwm_init();
        if (ret == 0)
          {
            break;
          }

        usleep(200 * 1000);
      }
  }

  if (ret != 0)
    {
      syslog(LOG_WARNING, "[led_strip] PWM clock not ready: %d, "
             "worker 会继续重试\n", ret);
    }

  /* 平时就是常亮；有活动位时 worker 会接管做呼吸。 */
  led_strip_set_pct(STRIP_STEADY_PCT);

  g_strip_run = 1;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);

  ret = pthread_create(&g_strip_thread, &attr, strip_worker, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_strip_run = 0;
      led_strip_set_pct(0);
      PWM_REG(LED_PWM_PER) &= ~(1u << STRIP_PWM_CH);
      syslog(LOG_ERR, "[led_strip] worker create failed: %d\n", ret);
      return -1;
    }

  g_strip_started = 1;
  syslog(LOG_INFO,
         "[led_strip] PG13/PWM2 %d Hz, steady %d%%, breathe on activity "
         "(max %d%%)\n",
         STRIP_PWM_HZ, STRIP_STEADY_PCT, STRIP_MAX_PCT);
  return 0;
}

void led_strip_stop(void)
{
  if (!g_strip_started)
    {
      return;
    }

  g_strip_run = 0;

  /* 等 worker 真的退出再关灯，否则它可能在我们写 0% 之后又写一档亮度。
   * idle 时最长睡 100 ms，join 最多也就等这么久。 */
  pthread_join(g_strip_thread, NULL);

  led_strip_set_pct(0);
  PWM_REG(LED_PWM_PER) &= ~(1u << STRIP_PWM_CH);

  led_strip_set_activity(0xffffffffu, 0);
  g_strip_started = 0;
}
