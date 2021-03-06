/*********************************************************************
 * Filename:      acerhk.c
 * Version:       0.5
 *
 * Copyright (C) 2002-2007, Olaf Tauber (olaf-tauber@versanet.de)
 *
 * Description:   kernel driver for Acer Travelmate and similar
 *                laptops special keys
 * Author:        Olaf Tauber <olaf-tauber@versanet.de>
 * Created at:    Mon Apr 29 22:16:42 2002
 * Modified at:   Mon Nov 12 20:53:56 2007
 * Modified by:   Olaf Tauber <ole@smeagol>
 * Modified at:   Thu Nov 24 13:03:01 2005
 * Modified by:   Antonio Cuni <cuni@programmazione.it>
 * Modified at:   Wed Oct 27 19:47:11 CEST 2004
 * Modified by:   Joachim Fenkes <acerhk@dojoe.net>
 *
 * This program is free software; you can redistribute
 * it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place,
 * Suite 330, Boston, MA  02111-1307  USA
 *
 *
 */

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif

/* This driver is heavily dependent on the architecture, don't let
 * anyone without an X86 machine use it. On laptops with AMD64
 * architecture this driver is only useable in 32 bit mode.
 */
#ifdef CONFIG_X86

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0)
#define KERNEL26
#include <linux/moduleparam.h>
#else
#include <linux/modversions.h>
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
#define STATIC_INPUT_DEV
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kmod.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/mc146818rtc.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/delay.h>

#include "acerhk.h"

/* #define ACERDEBUG */
/* #define DUMMYHW */

#define ACERHK_VERSION "0.5.35"
#define MODULE_NAME "acerhk"

/* maximum number of polling loops, adjust it if needed to values between
 * 1 and 32
 */
#define MAX_POLLING_LOOPS 16U

/* maximum length for model string */
#define ACERHK_MODEL_STRLEN 16
/* size of mapped areas */
#define AREA_SIZE 0xffff
/* needed for colussi algorithm */
#define XSIZE     20

/* Module parameters */
static int poll=1;
static int autowlan;
static int usedritek=1;
static int wlan_state=-1;
static int bluetooth_state=-1;
static int verbose;
static unsigned int force_series;
#ifdef KERNEL26
module_param(poll, int, 0444);
module_param(autowlan, int, 0444);
module_param(usedritek, int, 0444);
module_param(verbose, int, 0444);
module_param(wlan_state, int, 0444);
module_param(bluetooth_state, int, 0444);
module_param(force_series, uint, 0444);
#else
MODULE_PARM(poll, "i");
MODULE_PARM(autowlan, "i");
MODULE_PARM(wlan_state, "i");
MODULE_PARM(bluetooth_state, "i");
MODULE_PARM(usedritek, "i");
MODULE_PARM(verbose, "i");
MODULE_PARM(force_series, "i");
#endif
MODULE_PARM_DESC(poll, "start polling timer");
MODULE_PARM_DESC(autowlan, "automatic switching of wlan hardware");
MODULE_PARM_DESC(wlan_state, "(assumed) initial state of WLAN LED/hardware");
MODULE_PARM_DESC(bluetooth_state, "(assumed) initial state of Bluetooth LED/hardware");
MODULE_PARM_DESC(usedritek, "enable dritek keyboard extension");
MODULE_PARM_DESC(verbose, "output additional information");
MODULE_PARM_DESC(force_series, "force laptop series, skip autodetection");

/* input device */
static struct input_dev *acerhk_input_dev_ptr;
#ifdef STATIC_INPUT_DEV
static struct input_dev acerhk_input_dev;
#endif

/* mapped IO area from 0xf0000 */
static void *reg1;
/* mapped IO area from 0xe0000 */
static void *reg2;
/* Pointer to mapped area at 0x400 on 520 series */
static void *preg400;
/* location of IO routine in mapped area */
static unsigned int bios_routine;
/* index of CMOS port to get key event */
static unsigned int cmos_index;
/* function for bios call */
static bios_call call_bios;
/* address of model string */
static char *acerhk_model_addr;
/* copied string, maximum length 16 ('TravelMate xxx') */
static char acerhk_model_string[ACERHK_MODEL_STRLEN];
/* type of hardware access  */
static t_acer_type acerhk_type;
/* travelmate series  */
static unsigned int acerhk_series;
/* supported features for this model */
static unsigned int acerhk_model_features;
/* map of acer key codes to acer key names */
static unsigned char acerhk_key2name[0xff];
/* map of acer key names to key events */
static t_map_name2event acerhk_name2event;
/* timer for polling key presses */
static struct timer_list acerhk_timer_poll;
/* polling active */
static int acerhk_polling_state;
/* polling delay */
static unsigned acerhk_polling_delay = HZ/5;
/* wlan hardware toggle */
static int acerhk_wlan_state;
/* bluetooth hardware toggle */
static int acerhk_bluetooth_state;

/* bluetooth blinking state; added by Antonio Cuni
   possible values:
      -1: blinking disabled (default)
       0: blinking enabled, led currently off
       1: blinking enabled, led currently on
*/
static int acerhk_blueled_blinking = -1;
/* delay between two changes of state, in jiffies */
static unsigned acerhk_blueled_blinking_delay;
/* timer for blinking */
static struct timer_list acerhk_timer_blinking;

/* function prototypes */
static void start_polling(void);
static void stop_polling(void);

/* Added by Antonio Cuni */
static void start_blinking(void);
static void stop_blinking(void);

/* {{{ Experimental use of dritek keyboard extension */

#define EC_STATUS_REG		0x66	/* Status register of EC (R) */
#define EC_CNTL_REG		    0x66	/* Controller command register of EC (W) */
#define EC_DATA_REG		    0x62	/* EC data register (R/W) */

#ifdef KERNEL26

#include <linux/preempt.h>

#define KBD_STATUS_REG		0x64	/* Status register (R) */
#define KBD_CNTL_REG		0x64	/* Controller command register (W) */
#define KBD_DATA_REG		0x60	/* Keyboard data register (R/W) */

#else

#ifndef KEY_MEDIA
#define KEY_MEDIA		226
#endif

#define preempt_disable()		do { } while (0)
#define preempt_enable_no_resched()	do { } while (0)
#define preempt_enable()		do { } while (0)
#define preempt_check_resched()		do { } while (0)
#include <linux/pc_keyb.h>

#endif

static inline int my_i8042_read_status(void)
{
  return inb(KBD_STATUS_REG);
}
static int my_i8042_wait_write(void)
{
	int i = 0;
	while ((my_i8042_read_status() & 0x02) && (i < 10000)) {
		udelay(50);
		i++;
	}
	return -(i == 10000);
}
static void send_kbd_cmd(unsigned char cmd, unsigned char val)
{
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_write())
      outb(cmd, KBD_CNTL_REG);
    if (!my_i8042_wait_write())
      outb(val, KBD_DATA_REG);
    preempt_enable_no_resched();
  } else {
    printk(KERN_INFO"acerhk: request for accessing EC ignored\n"
           KERN_INFO"acerhk: Use of dritek keyboard extension not enabled, use module\n"
           KERN_INFO"acerhk: parameter usedritek=1 to do that (possibly dangerous)\n");
  }
}
#ifdef ACERDEBUG
static inline int my_i8042_read_ecstatus(void)
{
  return inb(EC_STATUS_REG);
}
static int my_i8042_wait_ecwrite(void)
{
	int i = 0;
	while ((my_i8042_read_ecstatus() & 0x02) && (i < 10000)) {
		udelay(50);
		i++;
	}
	return -(i == 10000);
}
static void send_ec_cmd(unsigned char cmd, unsigned char val)
{
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_ecwrite())
      outb(cmd, EC_CNTL_REG);
    if (!my_i8042_wait_ecwrite())
      outb(val, EC_DATA_REG);
    preempt_enable_no_resched();
  } else {
    printk(KERN_INFO"acerhk: request for accessing EC ignored\n"
           KERN_INFO"acerhk: Use of dritek keyboard extension not enabled, use module\n"
           KERN_INFO"acerhk: parameter usedritek=1 to do that (possibly dangerous)\n");
  }
}
#endif
#ifdef ACERDEBUG
static void enable_mute_led_ec(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling mute led via EC\n");
  send_kbd_cmd(0x59, 0x94);
}
static void disable_mute_led_ec(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling mute led via EC\n");
  send_kbd_cmd(0x59, 0x95);
}
static void enable_dmm_function(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling WLAN via EC variant 2\n");
  send_kbd_cmd(0x45, 0xd3);
}
#endif
static void enable_wlan_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling WLAN via EC variant 1\n");
  send_kbd_cmd(0xe7, 0x01);
  acerhk_wlan_state = 1;
}
static void disable_wlan_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling WLAN via EC variant 1\n");
  send_kbd_cmd(0xe7, 0x00);
  acerhk_wlan_state = 0;
}
static void enable_bluetooth_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling Bluetooth via EC variant 1\n");
  send_kbd_cmd(0xe7, 0x03);
  acerhk_bluetooth_state = 1;
}
static void disable_bluetooth_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling Bluetooth via EC variant 1\n");
  send_kbd_cmd(0xe7, 0x02);
  acerhk_bluetooth_state = 0;
}
static void enable_wlan_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling WLAN via EC variant 2\n");
  send_kbd_cmd(0x45, acerhk_bluetooth_state ? 0xa2 : 0xa0);
  acerhk_wlan_state = 1;
}
static void disable_wlan_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling WLAN via EC variant 2\n");
  send_kbd_cmd(0x45, acerhk_bluetooth_state ? 0xa1 : 0xa3);
  acerhk_wlan_state = 0;
}
static void enable_bluetooth_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling Bluetooth via EC variant 2\n");
  send_kbd_cmd(0x45, acerhk_wlan_state ? 0xa2 : 0xa1);
  acerhk_bluetooth_state = 1;
}
static void disable_bluetooth_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling Bluetooth via EC variant 2\n");
  send_kbd_cmd(0x45, acerhk_wlan_state ? 0xa0 : 0xa3);
  acerhk_bluetooth_state = 0;
}
#ifdef ACERDEBUG
static void enable_wireless_ec(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling wireless hardware\n");
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_ecwrite())
      outb(0x4d, EC_CNTL_REG);
    if (!my_i8042_wait_ecwrite())
      outb(0xd2, EC_DATA_REG);
    if (!my_i8042_wait_ecwrite())
      outb(0x01, EC_DATA_REG);
    preempt_enable_no_resched();
  }
  acerhk_wlan_state = 1;
}
static void disable_wireless_ec(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling wireless hardware\n");
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_ecwrite())
      outb(0x4d, EC_CNTL_REG);
    if (!my_i8042_wait_ecwrite())
      outb(0xd2, EC_DATA_REG);
    if (!my_i8042_wait_ecwrite())
      outb(0x00, EC_DATA_REG);
    preempt_enable_no_resched();
  }
  acerhk_wlan_state = 0;
}
#endif
static void enable_dritek_keyboard(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling dritek keyboard extension\n");
  send_kbd_cmd(0x59, 0x90);
}
static void disable_dritek_keyboard(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling dritek keyboard extension\n");
  send_kbd_cmd(0x59, 0x91);
}
static void enable_mail_led_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling mail led via EC variant 1\n");
  send_kbd_cmd(0xe8, 0x01);
}
static void disable_mail_led_ec_1(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling mail led via EC variant 1\n");
  send_kbd_cmd(0xe8, 0x00);
}

static void enable_mail_led_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: enabling mail led via EC variant 2\n");
  send_kbd_cmd(0x59, 0x92);
}
static void disable_mail_led_ec_2(void)
{
  if (verbose)
    printk(KERN_INFO "acerhk: disabling mail led via EC variant 2\n");
  send_kbd_cmd(0x59, 0x93);
}
static void enable_mail_led_ec_3(void)
{
 if (verbose)
    printk(KERN_INFO "acerhk:  enabling mail led via EC variant 3\n");
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_write())
      outl(0x80008894, 0xCF8);
    if (!my_i8042_wait_write())
      outw(0xC061, 0xCFC);
    preempt_enable_no_resched();
  }
}
static void disable_mail_led_ec_3(void)
{
 if (verbose)
    printk(KERN_INFO "acerhk:  disabling mail led via EC variant 3\n");
  if (usedritek) {
    preempt_disable();
    if (!my_i8042_wait_write())
      outl(0x80008894, 0xCF8);
    if (!my_i8042_wait_write())
      outw(0xC060, 0xCFC);
    preempt_enable_no_resched();
  }
}

/* }}} */

/* {{{ string search functions */

/* This is the Colussi algorithm, the code is taken from 
   http://www-igm.univ-mlv.fr/~lecroq/string
*/
int preColussi(char *x, int m, int *h, int *next, int *shift)
{
  int i, k, nd, q, r, s;
  int hmax[XSIZE], kmin[XSIZE], nhd0[XSIZE], rmin[XSIZE];
  /* Computation of hmax */
  i = k = 1;
  do {
    while (x[i] == x[i - k])
      i++;
    hmax[k] = i;
    q = k + 1;
    while (hmax[q - k] + k < i) {
      hmax[q] = hmax[q - k] + k;
      q++;
    }
    k = q;
    if (k == i + 1)
      i = k;
  } while (k <= m);    /* Computation of kmin */
  memset(kmin, 0, m*sizeof(int));
  r = 0;
  for (i = m; i >= 1; --i)
    if (hmax[i] < m)
      kmin[hmax[i]] = i;    /* Computation of rmin */
  for (i = m - 1; i >= 0; --i) {
    if (hmax[i + 1] == m)
      r = i + 1;
    if (kmin[i] == 0)
      rmin[i] = r;
    else
      rmin[i] = 0;
  }    /* Computation of h */
  s = -1;
  r = m;
  for (i = 0; i < m; ++i)
    if (kmin[i] == 0)
      h[--r] = i;
    else
      h[++s] = i;
  nd = s;    /* Computation of shift */
  for (i = 0; i <= nd; ++i)
    shift[i] = kmin[h[i]];
  for (i = nd + 1; i < m; ++i)
    shift[i] = rmin[h[i]];
  shift[m] = rmin[0];    /* Computation of nhd0 */
  s = 0;
  for (i = 0; i < m; ++i) {
    nhd0[i] = s;
    if (kmin[i] > 0)
      ++s;
  }    /* Computation of next */
  for (i = 0; i <= nd; ++i)
    next[i] = nhd0[h[i] - kmin[h[i]]];
  for (i = nd + 1; i < m; ++i)
    next[i] = nhd0[m - rmin[h[i]]];
  next[m] = nhd0[m - rmin[h[m - 1]]];    return(nd);
}

int COLUSSI(char *x, int m, char *y, int n) {
  int i, j, last, nd,
    h[XSIZE], next[XSIZE], shift[XSIZE];    /* Processing */
  int match_pos; /* position of first match */
  nd = preColussi(x, m, h, next, shift);    /* Searching */
  i = j = 0;
  last = -1;
  match_pos = -1;
  while ( (match_pos == -1)
          && (j <= n - m) ) {
    while (i < m && last < j + h[i] &&
           x[h[i]] == y[j + h[i]])
      i++;
    if (i >= m || last >= j + h[i]) {
      /* Match found, bail out */
      match_pos = j;
      i = m;
    }
    if (i > nd)
      last = j + m - 1;
    j += shift[i];
    i = next[i];
  }
  return match_pos;
}

/* }}} */

/* {{{ hardware access functions */

/* call_bios_<model family>
 *
 * call request handler in mapped system rom
 *
 * the request is handed over via all 6 general purpose registers, results are
 * taken from them and copied back to buf
 */
static asmlinkage void call_bios_6xx(struct register_buffer *buf)
{
#ifndef __x86_64__
  if (bios_routine) {
      local_irq_disable();
	__asm__ __volatile__(
						 "movl %1,%%edx\n\t"
						 "pusha\n\t"
						 "movl %%edx,%%ebp\n\t"
						 "movl (%%ebp),%%eax\n\t"
						 "movl 4(%%ebp),%%ebx\n\t"
						 "movl 8(%%ebp),%%ecx\n\t"
						 "movl 12(%%ebp),%%edx\n\t"
						 "movl 16(%%ebp),%%edi\n\t"
						 "movl 20(%%ebp),%%esi\n\t"
						 "pushl %%ebp\n\t"
						 "call *%0\n\t"
						 "popl %%ebp\n\t"
						 "movl %%eax, (%%ebp)\n\t"
						 "movl %%ebx, 4(%%ebp)\n\t"
						 "movl %%ecx, 8(%%ebp)\n\t"
						 "movl %%edx, 12(%%ebp)\n\t"
						 "movl %%edi, 16(%%ebp)\n\t"
						 "movl %%esi, 20(%%ebp)\n\t"
						 "popa\n\t"
						 :
						 :"m" (bios_routine), "m" (buf)
						 :"%eax", "%ebx", "%ecx", "%edx", "%edi", "%esi", "%ebp"
						 );
      local_irq_enable();
  }
#endif
}

static asmlinkage void call_bios_52x(struct register_buffer *buf)
{
#ifndef __x86_64__
  if (bios_routine) {
      local_irq_disable();
	__asm__ __volatile__(
						 "movl %2,%%edx\n\t" 
						 "pusha\n\t"
 						 "movl %%edx,%%ebp\n\t"
						 "movl (%%ebp),%%eax\n\t"
						 "movl 4(%%ebp),%%ebx\n\t"
						 "movl 8(%%ebp),%%ecx\n\t"
						 "movl 12(%%ebp),%%edx\n\t"
						 "movl 16(%%ebp),%%edi\n\t"
						 "movl 20(%%ebp),%%esi\n\t"
						 "pushl %%ebp\n\t"
						 "movl %1, %%ebp\n\t"
						 "call *%0\n\t"
						 "popl %%ebp\n\t"
						 "movl %%eax, (%%ebp)\n\t"
						 "movl %%ebx, 4(%%ebp)\n\t"
						 "movl %%ecx, 8(%%ebp)\n\t"
						 "movl %%edx, 12(%%ebp)\n\t"
						 "movl %%edi, 16(%%ebp)\n\t"
						 "movl %%esi, 20(%%ebp)\n\t"
						 "popa\n\t"
						 :
						 :"m" (bios_routine), "m" (preg400), "m" (buf)
						 :"%eax", "%ebx", "%ecx", "%edx", "%edi", "%esi", "%ebp"
						 );
      local_irq_enable();
  }
#endif
}

#define PRINT_BUFFER(x) \
  printk(KERN_INFO"acerhk: eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n" \
		 "acerhk: edi=0x%x esi=0x%x ebp=0x%x\n", \
		 x.eax, x.ebx, x.ecx, x.edx, x.edi, x.esi, x.ebp);

/* get_fnkey_event
 *
 * gets the first (oldest) key id from the queue of events
 * 
 * return value: id of key
 */
static int get_fnkey_event(void)
{
  struct register_buffer regs;
  regs.eax = 0x9610;
  regs.ebx = 0x61C;
  /* clear other registers, some models need this */
  regs.ecx = 0;
  regs.edx = 0;
  preempt_disable();
  call_bios(&regs);
  preempt_enable_no_resched();
  return regs.eax & 0xffff;
}

/* get_thermal_event
 *
 * does what?
 * 
 * return value: event ?
 */
static int get_thermal_event(void)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_THERMAL) {
    regs.eax = 0x9612;
    regs.ebx = 0x12e;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: thermal event = 0x%x\n", regs.eax);
  } else {
    regs.eax = 0x00;
    if (verbose > 3)
      printk(KERN_INFO"acerhk: thermal event not supported\n");
  }
  return regs.eax & 0xffff;
}

#ifdef ACERDEBUG
/* pbutton_fct
 *
 * does what?
 * 
 * return value: ?
 */
static int pbutton_fct(void)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_PBUTTON) {
    regs.eax = 0x9612;
    regs.ebx = 0x10b;
    regs.ecx = 0x2;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: pbutton = 0x%x\n", regs.eax);
  } else {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: pbutton function not supported\n");
    regs.eax = 0x00;
  }
  return regs.eax & 0xffff;
}
#endif

/* wbutton_fct_1
 *
 * turn on installed Bluetooth hardware together with the corresponding LED
 * 
 * val: 0       turns off the LED
 *      1       turns the LED to green/blue
 *
 * return value: ?
 */
static int wbutton_fct_1(int val)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_WBUTTON) {
    acerhk_bluetooth_state = val;
    regs.eax = 0x9610;
    regs.ebx = ((val & 0xff) << 8) | 0x34;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: wbutton1 = 0x%x\n", regs.eax);
  } else {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: wbutton function 1 not supported\n");
    regs.eax = 0x00;
  }
  return regs.eax & 0xffff;
}

/* wbutton_fct_2
 *
 * turn on installed WLAN hardware together with the corresponding LED
 * 
 * val: 0       turns off the LED
 *      1       turns the LED to orange
 *
 * return value: ?
 */
static int wbutton_fct_2(int val)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_WBUTTON) {
    acerhk_wlan_state = val;
    regs.eax = 0x9610;
    regs.ebx = ((val & 0xff) << 8) | 0x35;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: wbutton2 = 0x%x\n", regs.eax);
  } else {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: wbutton function 2 not supported\n");
    regs.eax = 0x00;
  }
  return regs.eax & 0xffff;
}

/* get_cmos_index
 * 
 * gets index of CMOS port from ROM. The number of events is monitored
 * in this port.
 *
 * return value: index of CMOS port
 */
static int get_cmos_index(void)
{
  struct register_buffer regs;
  regs.eax = 0x9610;
  regs.ebx = 0x51C;
  preempt_disable();
  call_bios(&regs);
  preempt_enable_no_resched();
  cmos_index = regs.ecx & 0xff;
  if (verbose)
    printk(KERN_INFO"acerhk: cmos index set to 0x%x\n", cmos_index);
  return cmos_index;
}

/* get_nr_events
 * 
 * gets the number of cached events (keys pressed) in queue. Up to 31 events
 * are cached.
 *
 * return value: number of events in queue
 */
static int get_nr_events(void)
{
  unsigned long flags;
  unsigned char c = 0;
  
  spin_lock_irqsave (&rtc_lock, flags);
/* #ifndef DUMMYHW */
#if !(defined(DUMMYHW) || defined(__x86_64__))
  if (cmos_index)
    c = CMOS_READ(cmos_index);
  else if (verbose > 3)
    printk(KERN_INFO"acerhk: get_nr_events - no valid cmos index set\n");
#endif
  spin_unlock_irqrestore (&rtc_lock, flags);
  return c;
}

/* set_mail_led
 * 
 * change state of mail led
 *
 * val: 0 - switch led off
 *		1 - switch led on (blinking)
 *
 * return value: 1 - action succesfull (val valid)
 *				 0 - no action taken (val invalid)
 */
static int set_mail_led(int val)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_MAIL_LED) {
    regs.eax = 0x9610;
    regs.ebx = ((val & 0xff) << 8) | 0x31;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: mail led set to = 0x%x\n", val);
  } else if (acerhk_model_features & TM_F_MAIL_LED_EC) {
    if (val == 1)
      enable_mail_led_ec_1();
    else  if (val == 0)
      disable_mail_led_ec_1();
  } else if (acerhk_model_features & TM_F_MAIL_LED_EC2) {
    if (val == 1)
      enable_mail_led_ec_2();
    else  if (val == 0)
      disable_mail_led_ec_2();
  } else if (acerhk_model_features & TM_F_MAIL_LED_EC3) {
    if (val == 1)
      enable_mail_led_ec_3();
    else  if (val == 0)
      disable_mail_led_ec_3();
  } else {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: mail led not supported\n");
    regs.eax = 0x00;
  }
  return regs.eax & 0xffff;
}

/* launch_connect
 * 
 * does what?
 * val: 1 - only known value from windows driver
 */
static int launch_connect(int val)
{
  struct register_buffer regs;
  if (acerhk_model_features & TM_F_CONNECT) {
    regs.eax = 0x9610;
    regs.ebx = ((val & 0xff) << 8) | 0x2e;
    preempt_disable();
    call_bios(&regs);
    preempt_enable_no_resched();
    if (verbose > 3)
      printk(KERN_INFO"acerhk: connect(%d) = 0x%x\n", val, regs.eax);
  } else {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: connect not supported\n");
    regs.eax = 0x00;
  }
  return regs.eax & 0xffff;
}

/* }}} */

/* {{{ hardware probing */

static struct proc_dir_entry *proc_acer_dir;

static unsigned long __init find_hk_area(void)
{
  long offset, sig;
  unsigned int fkt;
  fkt = 0;
  sig = -1; /* offset to signature in io area */
  /* Look for signature, start at 0xf0000, search until 0xffff0 */
  for (offset = 0;offset < 0xfffd; offset += 16) {
    if (readl(reg1 + offset) == 0x30552142) {
      sig = offset;
      offset = 0xffff;
    }
  }
  if (sig < 0)
    printk(KERN_WARNING"acerhk: could not find request handler, possibly not all functions available\n");
  else {
    /* compute location of bios routine */
    fkt = readl(reg1 + sig + 5);
    /* adjust fkt to address of mapped IO area */
    if (fkt >= 0xf0000)
      fkt = (unsigned long)reg1 + fkt - 0xf0000;
    else if (fkt >= 0xe0000)
      fkt = (unsigned long)reg1 + fkt - 0xe0000;
    else
      fkt = 0;
  }
  return fkt;
}

static void print_features(void)
{
  int i;
  printk(KERN_INFO"acerhk: supported keys:");
  for (i = 0; i < 255; i++) {
    switch (acerhk_key2name[i]) {
    case k_help: printk(" help"); break;
    case k_setup: printk(" setup"); break;
    case k_p1: printk(" p1"); break;
    case k_p2: printk(" p2"); break;
    case k_p3: printk(" p3"); break;
    case k_www: printk(" www"); break;
    case k_mail: printk(" mail"); break;
    case k_wireless: printk(" wireless"); break;
    case k_power: printk(" power"); break;
    case k_mute: printk(" mute"); break;
    case k_volup: printk(" volup"); break;
    case k_voldn: printk(" voldn"); break;
    case k_res: printk(" res"); break;
    case k_close: printk(" close"); break;
    case k_open: printk(" open"); break;
    case k_wireless2: printk(" wireless2"); break;
    case k_play: printk(" play"); break;
    case k_stop: printk(" stop"); break;
    case k_prev: printk(" prev"); break;
    case k_next: printk(" next"); break;
    case k_display: printk(" display"); break;
    default: break;
    }
  }
  printk("\n");
  if (acerhk_model_features & TM_F_MUTE_LED_EC)
    printk(KERN_INFO"acerhk: mute led is supported\n");
  if (acerhk_model_features & TM_F_MAIL_LED)
    printk(KERN_INFO"acerhk: mail led is supported\n");
  else if (acerhk_model_features & TM_F_MAIL_LED_EC)
    printk(KERN_INFO"acerhk: mail led (EC) is supported\n");
  else if (acerhk_model_features & TM_F_MAIL_LED_EC2)
    printk(KERN_INFO"acerhk: mail led (EC2) is supported\n");
  else if (acerhk_model_features & TM_F_MAIL_LED_EC3)
    printk(KERN_INFO"acerhk: mail led (EC3) is supported\n");
  if (acerhk_model_features & TM_F_WLAN_EC1)
    printk(KERN_INFO"acerhk: wlan control (EC1) is supported\n");
  else if (acerhk_model_features & TM_F_WLAN_EC2)
    printk(KERN_INFO"acerhk: wlan control (EC2) is supported\n");
  if (acerhk_model_features & TM_F_BLUE_EC1)
    printk(KERN_INFO"acerhk: bluetooth control (EC1) is supported\n");
  else if (acerhk_model_features & TM_F_BLUE_EC2)
    printk(KERN_INFO"acerhk: bluetooth control (EC2) is supported\n");
  printk(KERN_INFO"acerhk: supported functions:");
  if (acerhk_model_features & TM_F_CONNECT)
    printk(" connect");
  if (acerhk_model_features & TM_F_THERMAL)
    printk(" thermal");
  if (acerhk_model_features & TM_F_PBUTTON)
    printk(" pbutton");
  if (acerhk_model_features & TM_F_WBUTTON)
    printk(" wbutton");
  printk("\n");
}

static void __init setup_keymap_model(unsigned int series)
{
  /* clear mapping keycode -> keyname, */
  memset(&acerhk_key2name[0], k_none, sizeof(acerhk_key2name));
  /* first set the common keys, namely FnF1 and FnF2, */
  acerhk_key2name[1] = k_help;
  acerhk_key2name[2] = k_setup;
  /* then set known keycodes according to model */
  switch (series) {
   case 110:
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[3]  = k_power;
    acerhk_key2name[8]  = k_mute;
    acerhk_key2name[32] = k_volup;
    acerhk_key2name[33] = k_voldn;
    /* C110 generates 2 extra codes when opening/closing the lid */
    acerhk_key2name[74] = k_close;
    acerhk_key2name[75] = k_open;
    break;
  case 300: /* treat C300 like C100 with Bluetooth button */
    acerhk_key2name[68] = k_wireless2;
  case 100:
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[49] = k_www;
    acerhk_key2name[54] = k_mail;
    acerhk_key2name[3]  = k_power;
    acerhk_key2name[8]  = k_mute;
    acerhk_key2name[32] = k_volup;
    acerhk_key2name[33] = k_voldn;
    break;
  default:
    /* only the two common keys are supported */
    break;
  case 210:
    acerhk_key2name[19] = k_p1;
    acerhk_key2name[20] = k_p2;
    acerhk_key2name[17] = k_www;
    acerhk_key2name[18] = k_mail;
    break;
  case 220:
  case 260: /* 260 with same keys? */
    acerhk_key2name[49] = k_p1;
    acerhk_key2name[19] = k_p2;
    acerhk_key2name[18] = k_www;
    acerhk_key2name[17] = k_mail;
    break;
  case 230:
  case 280: /* 280 with same keys? */
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    break;
  case 1500:
    acerhk_key2name[0x49] = k_setup;
    acerhk_key2name[0x36] = k_www;
    acerhk_key2name[0x31] = k_mail;
    acerhk_key2name[0x11] = k_p1;
    acerhk_key2name[0x12] = k_p2;
    acerhk_key2name[0x30] = k_wireless;
    acerhk_key2name[0x44] = k_wireless2;
    acerhk_key2name[0x03] = k_power;
    break;
  case 240:
    acerhk_key2name[0x31] = k_www;
    acerhk_key2name[0x36] = k_mail;
    acerhk_key2name[0x11] = k_p1;
    acerhk_key2name[0x12] = k_p2;
    acerhk_key2name[0x44] = k_wireless;
    acerhk_key2name[0x30] = k_wireless2;
    acerhk_key2name[0x03] = k_power;
    acerhk_key2name[0x08] = k_mute;
    //  acerhk_key2name[] = k_volup;
    //  acerhk_key2name[] = k_voldn;
    break;
  case 2900:
    acerhk_key2name[0x31] = k_mail; /* with led */
    acerhk_key2name[0x36] = k_www;
    acerhk_key2name[0x11] = k_p1;
    acerhk_key2name[0x12] = k_p2;
    acerhk_key2name[0x30] = k_wireless; /* wireless, with led, related with autowlan=1 */
    break;
  case 250: /* enriqueg@altern.org */
    /* TravelMate 254LMi_DT manual common for 240/250 series, but key order
       differ from 240 already present on acerhk driver */
    /* TravelMate 254LMi_DT: 6 buttons: left to right: mail, www, p1, p2, bluetooth, wireless */
    acerhk_key2name[0x31] = k_mail; /* with led */
    acerhk_key2name[0x36] = k_www;
    acerhk_key2name[0x11] = k_p1;
    acerhk_key2name[0x12] = k_p2;
    acerhk_key2name[0x44] = k_wireless2; /* bluetooth, hw optional */
    acerhk_key2name[0x30] = k_wireless; /* wireless, with led, related with autowlan=1 */
    acerhk_key2name[0x03] = k_power; /* Fn+F3 */
    acerhk_key2name[0x08] = k_mute; /* Fn+F8 */
    break;
  case 380:
    /* TM 380 has same codes as TM 370, with an additional one */
    acerhk_key2name[0x03] = k_power;
  case 370:
    acerhk_key2name[0x30] = k_wireless;
    acerhk_key2name[0x11] = k_p1;
    acerhk_key2name[0x12] = k_p2;
    acerhk_key2name[0x13] = k_p3;
    acerhk_key2name[0x36] = k_www;
    acerhk_key2name[0x31] = k_mail;
    break;
  case 360:
    /* 360 series has the same layout as 350, with an
       additional wireless key */
    acerhk_key2name[64] = k_wireless;
  case 350:
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[20] = k_p3;
    acerhk_key2name[21] = k_www;
    acerhk_key2name[19] = k_mail;
    break;
  case 520:
    acerhk_key2name[19] = k_p1;
    acerhk_key2name[20] = k_p2;
    acerhk_key2name[17] = k_www;
    acerhk_key2name[18] = k_mail;
    break;
  case 610:
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[21] = k_www;
    acerhk_key2name[20] = k_mail;
    acerhk_key2name[64] = k_wireless;
    break;
  case 630:
    /* 630 has all keys of 620 plus one */
    acerhk_key2name[8] = k_mute;
  case 620:
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[3] = k_power;
    acerhk_key2name[32] = k_volup;
    acerhk_key2name[33] = k_voldn;
    break;
  case 290:
  case 420:
  case 430:
  case 530:
  case 540:
  case 650:
  case 660:
  case 800:
  case 1450:
  case 2300:
  case 2350:
  case 4000:
  case 4050:
  case 6000:
  case 8000:
  case 4100:
  case 4150:
  case 4500:
  case 4600:
  case 4650:
  case 1680:
  case 1690:
    /* keys are handled by dritek EC */
    acerhk_key2name[1] = k_none;
    acerhk_key2name[2] = k_none;
    break;
  case 1300:
  case 1310:
  case 1350:
  case 1360:
  case 1400:
  case 1700:
  case 1800:
  case 2000:
  case 2010:
  case 2020:
  case 5100:
    /* Aspire 13xx series laptops use dritek hardware, no
       acerhk-mapping needed
       VolUp and VolDown are managed as normal keys
       1300/1310 series should have P1, P2, Mail, WWW, Mute buttons
       1353 has bluetooth, wifi, p1, p2, www, mail, help, setup, power
       and mute
       Aspire 1400/1450/Ferrari use dritek EC, too
       1450 should have bluetooth, wifi, p1, p2, www, mail, help,
       setup, power and mute
       Aspire 1700 uses dritek EC, too
       1700 should have bluetooth, wifi, p1, p2, www, mail, help,
       setup, power and mute
       need the MM-buttons Activation? (forward, shuffle, ...)
       2000 hast lots of MM buttons
       2010 should have bluetooth, wifi, p1, p2, www, mail, help,
       setup, power and mute
    */
    acerhk_key2name[1] = k_none;
    acerhk_key2name[2] = k_none;
    break;
  case 1600:
    /* Aspire 1600 has acer keycode 0x49 for FnF2 */
    acerhk_key2name[73] = k_setup;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[3] = k_power;
    acerhk_key2name[8] = k_mute;
    /* VolUp and VolDown keys doesn't seem to be managed as special keys
       but as normal keys ! */
    break;
  case 5020: /* Aspire 5020 has 0x6a for Fn+F2 */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[106] = k_setup;
    acerhk_key2name[3] = k_power;
    acerhk_key2name[5] = k_display;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[68] = k_wireless2;
    break;
  case 2410: /* TM 2410 is very similar to Aspire 5020, but has 0x6s for Fn-F3 */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[106] = k_setup;
    acerhk_key2name[109] = k_power;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[68] = k_wireless2;
    break;
  case 40100:
    /* Medion MD40100, 4 keys */
    acerhk_key2name[54] = k_www;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[55] = k_res;
    break;
  case 96500:
  case 95400:
    /* Medion MD95400, many keys */
    acerhk_key2name[49] = k_mail;       /*  1 */
    acerhk_key2name[54] = k_www;        /*  2 */
    acerhk_key2name[48] = k_wireless;   /*  3 */
    acerhk_key2name[68] = k_wireless2;  /*  4 (Bluetooth) */

    acerhk_key2name[17] = k_p1;         /*  5 */
    acerhk_key2name[18] = k_p2;         /*  6 */
    acerhk_key2name[36] = k_play;       /*  7 */
    acerhk_key2name[37] = k_stop;       /*  8 */
    acerhk_key2name[34] = k_prev;       /*  9 */
    acerhk_key2name[35] = k_next;       /* 10 */
    acerhk_key2name[33] = k_voldn;      /* 11 */
    acerhk_key2name[32] = k_volup;      /* 12 */
    acerhk_key2name[38] = k_p3;         /* 13 */
    acerhk_key2name[8]  = k_mute;       /* 14 */

    acerhk_key2name[1]  = k_help;       /* FN+F1 (Help) */
    acerhk_key2name[5]  = k_display;    /* FN+F3 (Display switch) */
    acerhk_key2name[6]  = k_res;        /* FN+F4 (Display ein/ausschalten) */
    break;
  case 97600:
    /* Medion MD97600, 7 keys, no setup */
    acerhk_key2name[1]  = k_help;       /* FN+F1 (Help) */
    acerhk_key2name[2]	= k_none;
    acerhk_key2name[5]  = k_display;    /* FN+F3 (Display switch) */
    acerhk_key2name[6]  = k_res;        /* FN+F4 (Display ein/ausschalten) */
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[48] = k_wireless;
    break; 
  case 42200:
    /* Medion MD42200, 7 keys, no setup */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[5] = k_display;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    break;
  case 9783:
    /* Medion MD9783, 6 keys + info, no setup */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[8] = k_mute;
    break;
  case 7400:
    /* Amilo Pro V2000 does not have Help and Setup key (?)
       Amilo M 7400 has Help key, disabling only setup
     */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    break;
  case 1559:
    acerhk_key2name[6] = k_display; /* FN+F4 (Display ein/ausschalten) */
  case 1555:
    /* AOpen (Ahtec Signal 1555M) is similar to FS Amilo M */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[34] = k_prev;
    acerhk_key2name[35] = k_next;
    acerhk_key2name[36] = k_play;
    acerhk_key2name[37] = k_stop;
    break;
  case 6800:
  case 7820:
    /* Amilo D does not have Setup key */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[49] = k_mail;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    acerhk_key2name[19] = k_p3;
    acerhk_key2name[8] = k_mute;
    break;
  case 6805: /* Added by damagedspline@aim.com */
    /* Amilo A1xxx does not have Setup key nor a mail key */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[54] = k_www;
    acerhk_key2name[5] = k_display;
    acerhk_key2name[110] = k_setup; //This is the Fancy Fan (cool-n'-quiet) key on A1650g
    acerhk_key2name[48] = k_wireless;
    break;
  case 98200:
    /* Medion MD98200, 4 keys, no setup */
    acerhk_key2name[2] = k_none;
    acerhk_key2name[48] = k_wireless;
    acerhk_key2name[0x79] = k_play;
    acerhk_key2name[17] = k_p1;
    acerhk_key2name[18] = k_p2;
    break;
  }
}

static void __init setup_model_features(unsigned int series)
{
  switch (series) {
  case 200:
  case 210:
  case 520:
    /* nothing special */
    acerhk_model_features = 0;
    acerhk_type = TM_old;
    break;
  case 220:
  case 230:    
  case 260:
  case 280:    
  case 360:
  case 40100: /* Medion MD40100 */
  case 95400: /* Medion MD95400 */
  case 96500: /* Medion MD96500 */
    /* all special functions, no mail led */
    acerhk_model_features = 0x00f00000;
    acerhk_type = TM_new;
    break;
  case 97600:
    /* has WLAN button */
    /* The MD97600 seems to require TM_F_CONNECT at least
       once after cold boot, otherwise enabling the WLAN
       radio does not work */
    acerhk_model_features = TM_F_WBUTTON | TM_F_CONNECT;
    acerhk_type = TM_new;
    break;
  case 42200: /* Medion MD42200 */
    /* has WLAN button, should call connect() */
    acerhk_model_features = TM_F_WBUTTON | TM_F_CONNECT;
    acerhk_type = TM_old;
    break;
  case 98200: /* Medion MD98200 */
    /* has WLAN button, should call connect() */
    acerhk_model_features = TM_F_WBUTTON;
    acerhk_type = TM_old;
    break;
  case 9783: /* Medion MD9783 */
    /* only email led */
    acerhk_model_features = TM_F_MAIL_LED;
    acerhk_type = TM_new;
    break;
  case 1600:
    acerhk_type = TM_new;
    /* Do Aspire 1600 series have special functions or not ? I enable
       them, perhaps it helps with problems Francois Valenduc has */
    acerhk_model_features = 0x00f00000;
    break;
  case 300:
  case 100:
  case 110:
  case 240:
  case 350:
  case 610:
  case 620:
  case 630:
    /* all special functions, mail led */
    acerhk_model_features = TM_F_MAIL_LED | 0x00f00000;
    acerhk_type = TM_new;
    break;
  case 370:
  case 380:
  case 2410:
  case 2900: /* Medion MD2900 */
  case 2100: /* TM 2100 uses same driver as 5020 */
  case 5020: /* Aspire 5020 is still old hardware */
    acerhk_model_features = TM_F_MAIL_LED | TM_F_CONNECT| TM_F_WBUTTON;
    acerhk_type = TM_new;
    break;
  case 7400:
  case 1555:
  case 1559:
    /* all special functions for Fujitsu-Siemens Amilo M7400, Pro V2000; AOpen */
    acerhk_model_features = 0x00f00000;
    acerhk_type = TM_new;
    break;
  case 6800:
  case 7820:
    /* mail led and all special functions for FS Amilo D */
    acerhk_model_features = TM_F_MAIL_LED | 0x00f00000;
    acerhk_type = TM_new;
    break;
  case 6805: /* Added by damagedspline@aim.com */
    /* Amilo A1xxx does not have a mail led */
    acerhk_model_features = 0x00f00000;
    acerhk_type = TM_new;
    break;
  case 2350:
  case 4050:
    acerhk_wlan_state = 1;	// Default state is on
  case 290:
    /* no special functions, wireless hardware controlled by EC */
    acerhk_model_features = TM_F_WLAN_EC2 | TM_F_BLUE_EC2;
    acerhk_type = TM_dritek;
    break;
  case 650:
  case 1300:
  case 1310:
  case 1400:
  case 1700:
    /* all special functions, wireless hardware can be controlled */
    acerhk_model_features = 0x00f00000;
    acerhk_type = TM_dritek;
    break;
  case 4100:
  case 4600:
  case 1680:
  case 1690: /* Aspire 1680/1690 should be similar to TM 4100/4600 */
    /* mail led, wireless and bluetooth controlled the old way, but keys are
       controlled by normal keyboard controller, so mark as dritek and
       deactivate dritek use */
    acerhk_model_features = TM_F_MAIL_LED | TM_F_WBUTTON;
    acerhk_type = TM_dritek;
    usedritek=0;
    break;
  case 660:
  case 800:
    /* all special functions, mail led */
    acerhk_model_features = TM_F_MAIL_LED | 0x00f00000;
    acerhk_type = TM_dritek;
    break;
  case 1350:
  case 1360:
    /* mail led, handled by EC, wireless HW is not (yet) controllable ? */
    acerhk_model_features = TM_F_MAIL_LED_EC|TM_F_WLAN_EC1;
    acerhk_type = TM_dritek;
    break;
  case 1450:
    /* Bluetooth/Wlan led, Mail led handled by EC (variant 3) */
    acerhk_model_features = TM_F_MAIL_LED_EC3|TM_F_WBUTTON;
    acerhk_type = TM_dritek;
    break;
  case 1500:
    /* Bluetooth/Wlan led */
    acerhk_model_features = TM_F_WBUTTON;
    acerhk_type = TM_new;
    break;
  case 420:
  case 430:
    /* all functions and dritek EC, mail LED is handled by EC, second
       variant. An additional led is available, mute. (really?)
       */
    acerhk_type = TM_dritek;
    acerhk_model_features = TM_F_MUTE_LED_EC|TM_F_MAIL_LED_EC2;
    break;
  case 2300:
  case 4000:
  case 4500:
    /* wireless hardware, hopefully under control of my driver */
    acerhk_type = TM_dritek;
    acerhk_model_features = TM_F_BLUE_EC1|TM_F_WLAN_EC1;
    break;
  case 3200:
    /* test, if this model uses old style wlan control */
    acerhk_model_features = TM_F_WBUTTON;
    acerhk_type = TM_dritek;
    break;
  case 6000:
  case 8000:
    /* 6000 and 8000 have wireless hardware, but I don't know how to handle,
       so I choose no features */
    acerhk_type = TM_dritek;
    break;
  case 530:
  case 540:
  case 2000:
    /* No features (?) dritek EC, mail LED is handled by EC but
       different from other Aspire series */
    acerhk_type = TM_dritek;
    acerhk_model_features = TM_F_MAIL_LED_EC2;
    break;
  case 4150:
  case 4650:
    /* Dritek EC, bluetooth, wifi, mail */
    /* According to Andreas Stumpfl his TM 4652LMi does also work as series
       3200, which might mean that the BIOS function accesses the EC */ 
    acerhk_type = TM_dritek;
    acerhk_model_features = TM_F_MAIL_LED_EC2 | TM_F_WLAN_EC2 | TM_F_BLUE_EC2;
    break;
  case 1800:
  case 2010:
  case 2020:
  case 5100:
    /* Dritek EC, bluetooth, wifi, mail */
    acerhk_type = TM_dritek;
    acerhk_model_features = TM_F_MAIL_LED_EC2 | TM_F_WLAN_EC2 | TM_F_BLUE_EC2;
    acerhk_wlan_state = 1;	// Default state is on
    break;
  case 250: /* enriqueg@altern.org */
    /* TravelMate254LMi_DT : mail led, bluetooth (button present, hw optional), wifi (with led) */
    acerhk_model_features = TM_F_MAIL_LED| 
                            TM_F_WBUTTON ;
    acerhk_type = TM_new;
    acerhk_wlan_state = 0;	//Initial state is off on 254LMi_DT
    break;
  default:
    /* nothing special */
    acerhk_model_features = 0;
    acerhk_type = TM_unknown;
    break;
  }
  /* set the correct bios call function according to type */
  if ((acerhk_type == TM_new) || (acerhk_type == TM_dritek)) {
    call_bios = call_bios_6xx;
    if (verbose > 2)
      printk(KERN_INFO"acerhk: using call_bios_6xx mode\n");
  } else {
    call_bios = call_bios_52x;
    if (verbose > 2)
      printk(KERN_INFO"acerhk: using call_bios_52x mode\n");
  }
  /* remove key file on dritek hardware */
  if (acerhk_type == TM_dritek) {
    remove_proc_entry("key", proc_acer_dir);
  }
  /* setup available keys */
  setup_keymap_model(acerhk_series);
  if (verbose > 1)
    print_features();
}

static unsigned int __init determine_laptop_series(char * str)
{
  /* 0 means unknown series, handled like TM 200 */
  unsigned int series = 0;
  if (strncmp(str, "TravelMate ", 11) == 0) {
    switch (str[11]) {
    case 'C':
      if (str[12] == '1') {
        if (str[13] == '0') {
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates TM C100 series\n");
          series = 100;
        } else if (str[13] == '1') {
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates TM C110 series\n");
          series = 110;
        }
      } else if (str[12] == '3') {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates TM C300 series\n");
        series = 300;
      }
      break;
    case 'F':
      if (str[12] == '4') {
        series = 230;
      }
      break;
    case '2':
      if (str[14] == '0') {
        /* newer Travelmate 2xxx series */
        switch (str[12]) {
        case '0':
        case '5':
          series = 2000; // 2000 and 2500 are the same
          break;
        case '1':
          if (str[13] == '0')
            series = 2100;
          break;
        case '2':
        case '7':
          series = 2200; // 2200 and 2700 are the same
          break;
        case '3':
          if (str[13] == '0')
            series = 4000; // 2300 is the same as 4000
          else if (str[13] == '5')
            series = 4050; // 2350 is the same as 4050
          break;
        case '4':
          if (str[13] == '1')
            series = 2410;
          break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown TM 2xxx series\n");
          break;
        }
      } else {
        /* older Travelmate 2xx series */
        switch (str[12]) {
        case '0': series = 200; break;
        case '1': series = 210; break;
        case '2': series = 220; break;
        case '4': series = 240; break;
        case '5': series = 250; break; /* enriqueg@altern.org */
        case '6': series = 260; break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown TM 2xx series\n");
          break;
        }
      }
      break;
    case '3':
      switch (str[12]) {
      case '0': series = 3200; break; /* TM 3000 works like TM 3200 */
        /* Travelmate 3xx series */
      case '5': series = 350; break;
      case '6': series = 360; break;
      case '7': series = 370; break;
      case '8': series = 380; break;
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM 3xx series\n");
        break;
      }
      break;
    case '4':
      if ( (strnlen(str, ACERHK_MODEL_STRLEN-1) == 15) &&
           (str[14] == '0') ) { /* Travelmate 4xxx series */
        switch (str[12]) {
        case '0': /* 4000 and 4500 are the same */
        case '5':
          series = 4000;
          break;
        case '1':
        case '6': /* 4100 and 4600 are the same */
          series = 4100;
          break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown TM 4xxx series\n");
          break;
        }
      } else { /* Travelmate 4xx series */
        switch (str[12]) {
        case '2': series = 420; break;
        case '3': series = 430; break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown TM 4xx series\n");
          break;
        }
      }
      break;
    case '5': /* Travelmate 5xx series */
      if (str[12] == '2')
        series = 520;
      else if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates unknown TM 5xx series\n");
      break;
    case '6': /* older Travelmate 6xx series */
      switch (str[12]) {
      case '1': series = 610; break;
      case '2': series = 620; break;
      case '3': series = 630; break;
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM 6xx series\n");
        break;
      }
      break;
    default:
      printk(KERN_INFO"acerhk: model string indicates unknown TM xxx series\n");
      break;
    }
    if (series && verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates TM %d series\n", series);
  }
  /* newer Travelmate series do not have a space after 'TravelMate' */
  else if (strncmp(str, "TravelMate", 10) == 0) {
    switch (str[10]) {
    case '2':
      if (str[11] == '9') {
        series = 290;
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM2xx series\n");
      }
      break;
    case '3':
      if (str[11] == '2' && str[14] == '3') {
        // TM 3200 uses "TravelMate32003"
        series = 3200;
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM3xxx series\n");
      }
      break;
    case '4':
      switch (str[11]) {
      case '3': series = 430; break;
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM4xx series\n");
        break;
      }
      break;
    case '5':
      switch (str[11]) {
      case '3': series = 530; break;
      case '4': series = 540; break;
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM5xx series\n");
        break;
      }
      break;
    case '6':
      switch (str[11]) {
      case '5': series = 650; break;
      case '6': series = 660; break;
      case '0':
        if (strncmp(str, "TravelMate60003", 15) == 0) {
          series = 6000; break;
        }
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM6xx series\n");
        break;
      }
      break;
    case '8':
      if (strncmp(str, "TravelMate80003", 15) == 0) {
        series = 8000;
      } else if (str[11] == '0') {
        series = 800;
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown TM8xx series\n");
      }
      break;
    default:
      printk(KERN_INFO"acerhk: model string indicates unknown TMxxx series\n");
      break;
    }
    if (series && verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates TM%d series\n", series);
  }
  else if (strncmp(str, "Aspire ", 7) == 0) {
    switch(str[7]) {
    case '1': /* Aspire 1xxx series */
      switch(str[8]) {
      case '3': /* Aspire 13xx series */
        switch (str[9]) {
        case '0': series = 1300; break;
        case '1': series = 1310; break;
        case '5': series = 1350; break;
        case '6': series = 1360; break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 13xx series\n");
          break;
        }
        break;
      case '4': /* Aspire 14xx series */
        switch (str[9]) {
        case '0': series = 1400; break;
        case '5': series = 1450; break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 14xx series\n");
          break;
        }
        break;
      case '5': series = 1500; break;
      case '6': /* Aspire 14xx series */
        switch (str[9]) {
        case '0': series = 1600; break;
        case '8':
        case '9': series = 1680; break;
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 16xx series\n");
          break;
        }
        break;
      case '7': series = 1700; break;
      case '8': series = 1800; break;
      default:
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown Aspire 1xxx series\n");
        break;
      }
      break;
    case '2': /* Aspire 2xxx series */
      if (str[8] == '0') {
        switch (str[9]) {
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 20xx series\n");
          break;
        case '0': series = 2000; break;
        case '1': series = 2010; break;
        case '2': series = 2020; break;
        }
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown Aspire 2xxx series\n");
      }
      break;
    case '3': /* Aspire 3xxx series */
      if (str[8] == '0') {
        switch (str[9]) {
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 30xx series\n");
          break;
        case '2': series = 5020; break; /* Aspire 3020/5020 are identical */
        }
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown Aspire 3xxx series\n");
      }
      break;
    case '5': /* Aspire 5xxx series */
      if (str[8] == '0') {
        switch (str[9]) {
        default:
          if (verbose > 1)
            printk(KERN_INFO"acerhk: model string indicates unknown Aspire 50xx series\n");
          break;
        case '2': series = 5020; break;
        }
      } else if (str[8] == '1' && str[9] == '0') {
       series = 5100;
      } else {
        if (verbose > 1)
          printk(KERN_INFO"acerhk: model string indicates unknown Aspire 5xxx series\n");
      }
      break;
    default:
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates unknown Aspire series\n");
      break;
    }
    if (series && verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates Aspire %d series\n", series);
  }
  else if (strncmp(str, "Extensa ", 8) == 0) {
    /* Extensa series */
    switch (str[8]) {
    case '3': 
      switch (str[9]) {
      case '0':
        series = 3000; break;
      default: break;
      }
      break;
    default: break;
    }
    if (series && verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates Extensa %d series\n", series);
    else if (verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates unknown Extensa series\n");
  }
  else if (strncmp(str, "Amilo ", 6) == 0) {
    switch (str[6]) {
    case 'D':   /* complete string is "Amilo D-Series", there seems to be no model number */ 
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates FS Amilo D series\n");
      /* this is the model number of my Amilo */
      series = 7820;
      break;
    default:
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates unknown FS Amilo XX series\n");
      series = 7820;
    }
  }
  else if (strncmp(str, "AMILO ", 6) == 0) {
    switch (str[6]) {
    case 'D':   /* AMILO D 6800 P4-2000 */ 
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates FS AMILO D series\n");
      series = 6800;
      break;
    case 'M':
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates FS AMILO M(7400) series\n");
      series = 7400;
      break;
    case 'P':
      /* it is assumed, that 'AMILO P' appears only on Amilo Pro Series */
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates FS AMILO Pro (V2000) series\n");
      series = 7400;
      break;
    case 'A':   /* AMILO Axxxx - added by damagedspline@aim.com */ 
      switch (str[7]) {
         case '1': /* AMILO A1xxx */
           if (verbose > 1)
             printk(KERN_INFO"acerhk: model string indicates FS AMILO A1xxx series\n");
           series = 6805;
           break;
      };
      break;
    default:
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates unknown FS AMILO XX series\n");
      series = 6800;
    } 
  }
  else if (strncmp(str, "MEDIONPC", 8) == 0) {
    uint medionmodel;
    if ((medionmodel = COLUSSI("WIM 2090", 8, reg1, AREA_SIZE)) >= 0) {
      printk(KERN_INFO"acerhk: found Medion model string:'%s'\n", (char*)reg1+medionmodel);
      series = 97600;
    } 
    else if ((medionmodel = COLUSSI("WIM 2040", 4, reg1, AREA_SIZE)) >= 0) {
      printk(KERN_INFO"acerhk: found Medion model string:'%s'\n", (char*)reg1+medionmodel);
      series = 96500;			
    } else {
      if ((medionmodel = COLUSSI("MD 9", 4, reg1, AREA_SIZE)) >= 0) {
        printk(KERN_INFO"acerhk: found Medion model string:'%s'\n", (char*)reg1+medionmodel);
      }
      series = 95400;			
    }
    if (verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates a medion MD %d\n", series);
  }
  else if (strncmp(str, "MEDIONNB", 8) == 0) {
    /* Search for the Product string of the MD9783. */
    if (COLUSSI("MD 42200", 8, reg1, AREA_SIZE) >= 0) {
      if (verbose>1)
        printk(KERN_INFO"acerhk: model string indicates a Medion MD 42200\n");
      series = 42200;
    } else if (COLUSSI("MD 9783", 7, reg1, AREA_SIZE) >= 0){
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates a medion MD 9783\n");
      series = 9783;
    } else if (COLUSSI("WIM 2000", 7, reg1, AREA_SIZE) >= 0){
      if (verbose>1)
        printk(KERN_INFO"acerhk: model string indicates a Medion MD 2900\n");
      series = 2900;
    } else {
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates a medion MD40100\n");
      series = 40100;
    }
  } else if (strncmp(str, "MEDION", 6) == 0) {
	if (COLUSSI("WIM2120", 7, reg1, AREA_SIZE) >= 0) {
         if (verbose>1)
           printk(KERN_INFO"acerhk: model string indicates a Medion MD 98200\n");
         series = 98200;
        }
  } else if (strncmp(str, "AOpen", 5) == 0) {
    if (strncmp(str, "AOpen*EzRestore", 15) == 0) {
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates a AOpen 1559\n");
      series = 1559;
    } else {
      /* Unless I know of other models no further differentiation,
         although there is a second part of the model string */
      if (verbose > 1)
        printk(KERN_INFO"acerhk: model string indicates a AOpen\n");
      series = 1555;
    }
  } else if (strncmp(str, "CL56", 4) == 0) {
    /* Unless I know of other models no further differentiation,
       although there are strings with more numbers ("CL561" on a Compal
       CL56/Zepto 4200, reported by Stian B. Barmen)
       It has the same functions as Acer Aspire 2010
    */
    if (verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates a Compal CL56 (or similar)\n");
    series = 2010;
  } else if (strncmp(str, "Geneva2", 7) == 0) {
    /* This might be an Aspire 9110 which is very similar to 4650 */
    if (verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates an Aspire 9110\n");
    series = 4650;
  } else {
    if (verbose > 1)
      printk(KERN_INFO"acerhk: model string indicates no supported hardware\n");
  }
  return (series);
}

static void __init probe_model(void) {
  int offset; /* offset from beginning of reg1 to Model string */
  if (verbose)
    printk(KERN_INFO"acerhk: start search for model string at %p\n", reg1);
  /* first we look for Travelmate, if it isn't one we try to identify other
     laptops, such as Medion or Aspire */
  offset = COLUSSI("Travel", 6, reg1, AREA_SIZE);
  /* Try to detect Aspire laptops */
  if (offset < 0)
    offset = COLUSSI("Aspire", 6, reg1, AREA_SIZE);
  /* Try to detect Extensa laptops */
  if (offset < 0)
    offset = COLUSSI("Extensa", 7, reg1, AREA_SIZE);
  /* Try to detect Medion laptops */
  if (offset < 0)
    offset = COLUSSI("MEDION", 6, reg1, AREA_SIZE);
  /* Try to detect AOpen laptops */
  if (offset < 0)
    offset = COLUSSI("AOpen", 5, reg1, AREA_SIZE);
  /* Try to detect Fujitsu Siemens Amilo laptops */
  if (offset < 0)
    offset = COLUSSI("Amilo", 5, reg1, AREA_SIZE);
  if (offset < 0)
    offset = COLUSSI("AMILO", 5, reg1, AREA_SIZE);
  /* Try to detect Compal */
  if (offset < 0)
    offset = COLUSSI("CL56", 4, reg1, AREA_SIZE);
  /* That might be an Aspire 9110 */
  if (offset < 0)
    offset = COLUSSI("Geneva2", 7, reg1, AREA_SIZE);
  if (offset >= 0) {
    acerhk_model_addr = reg1 + offset;
    /* copy the string, but not more than 15 characters */
    strncpy(acerhk_model_string, acerhk_model_addr, ACERHK_MODEL_STRLEN-1);
    if (verbose)
      printk(KERN_INFO"acerhk: found model string '%s' at %p\n",
             acerhk_model_string, acerhk_model_addr);
    if (bios_routine && verbose > 2)
      printk(KERN_INFO"acerhk: offset from model string to function address: 0x%lx\n",
             bios_routine - (unsigned long)acerhk_model_addr);
    acerhk_series = determine_laptop_series(acerhk_model_string);
  } else {
    printk(KERN_WARNING"acerhk: Could not find model string, will assume type 200 series\n");
    acerhk_series = 200;
  }
}

/* }}} */

/* {{{ key polling and translation */

static void print_mapping(void)
{
  printk(KERN_INFO"acerhk: key mapping:\n");
  printk("acerhk: help     0x%x\n", acerhk_name2event[k_help]);
  printk("acerhk: setup    0x%x\n", acerhk_name2event[k_setup]);
  printk("acerhk: p1       0x%x\n", acerhk_name2event[k_p1]);
  printk("acerhk: p2       0x%x\n", acerhk_name2event[k_p2]);
  printk("acerhk: p3       0x%x\n", acerhk_name2event[k_p3]);
  printk("acerhk: www      0x%x\n", acerhk_name2event[k_www]);
  printk("acerhk: mail     0x%x\n", acerhk_name2event[k_mail]);
  printk("acerhk: wireless 0x%x\n", acerhk_name2event[k_wireless]);
  printk("acerhk: power    0x%x\n", acerhk_name2event[k_power]);
  printk("acerhk: mute     0x%x\n", acerhk_name2event[k_mute]);
  printk("acerhk: volup    0x%x\n", acerhk_name2event[k_volup]);
  printk("acerhk: voldn    0x%x\n", acerhk_name2event[k_voldn]);
  printk("acerhk: res      0x%x\n", acerhk_name2event[k_res]);
  printk("acerhk: close    0x%x\n", acerhk_name2event[k_close]);
  printk("acerhk: open     0x%x\n", acerhk_name2event[k_open]);
  printk("acerhk: wireless2 0x%x\n", acerhk_name2event[k_wireless2]);
  printk("acerhk: play     0x%x\n", acerhk_name2event[k_play]);
  printk("acerhk: stop     0x%x\n", acerhk_name2event[k_stop]);
  printk("acerhk: prev     0x%x\n", acerhk_name2event[k_prev]);
  printk("acerhk: next     0x%x\n", acerhk_name2event[k_next]);
  printk("acerhk: display  0x%x\n", acerhk_name2event[k_display]);
}

static void set_keymap_name(t_key_names name, unsigned int key)
{
  acerhk_name2event[name] = key;
}

static void init_keymap_input(void)
{
  /* these values for input keys are chosen to match the key names on the
     actual Acer laptop */
  set_keymap_name(k_none, KEY_RESERVED);
  set_keymap_name(k_help, KEY_HELP);
  set_keymap_name(k_setup, KEY_CONFIG);
  set_keymap_name(k_p1, KEY_PROG1);
  set_keymap_name(k_p2, KEY_PROG2);
  set_keymap_name(k_p3, KEY_PROG3);
  set_keymap_name(k_www, KEY_WWW);
  set_keymap_name(k_mail, KEY_MAIL);
  set_keymap_name(k_wireless, KEY_XFER);
  set_keymap_name(k_power, KEY_POWER);
  set_keymap_name(k_mute, KEY_MUTE);
  set_keymap_name(k_volup, KEY_VOLUMEUP);
  set_keymap_name(k_voldn, KEY_VOLUMEDOWN);
  set_keymap_name(k_res, KEY_CONFIG);
  set_keymap_name(k_close, KEY_CLOSE);
  set_keymap_name(k_open, KEY_OPEN);
  /* I am not really happy with the selections for wireless and wireless2,
     but coffee looks good. Michal Veselenyi proposed this value */
  set_keymap_name(k_wireless2, KEY_COFFEE);
  set_keymap_name(k_play, KEY_PLAYPAUSE);
  set_keymap_name(k_stop, KEY_STOPCD);
  set_keymap_name(k_prev, KEY_PREVIOUSSONG);
  set_keymap_name(k_next, KEY_NEXTSONG);
  set_keymap_name(k_display, KEY_MEDIA); /* also not happy with this */
  if (verbose > 1)
    print_mapping();
}

static int filter_idle_value(int keycode)
{
  int validkey = 0;
  if (keycode != 0x0 &&
      keycode != 0x9610 &&
      keycode != 0xc100 && /* Francois Valenduc, Aspire 1601 LC */
      keycode != 0x8610 &&
      keycode != 0x861 &&
      keycode != 0x8650 &&
      keycode != 0x865)
    validkey = keycode;
  if (verbose > 4 && !validkey)
    printk(KERN_INFO"acerhk: throw away idle value 0x%x\n", keycode);
  return validkey;
}

static void send_key_event(t_key_names key)
{
  unsigned int input_key;
  if (key != k_none) {
    /* convert key name to kernel keycode */
    input_key = acerhk_name2event[key];
    if (verbose > 2)
      printk(KERN_INFO"acerhk: translated acer key name 0x%x to input key 0x%x\n",
             key, input_key);
    /* send press and release together, as there is no such event from acer as 'release' */
    input_report_key(acerhk_input_dev_ptr, input_key, 1);
    input_report_key(acerhk_input_dev_ptr, input_key, 0);
  }
}

static t_key_names transl8_key_code(int keycode)
{
  t_key_names keyname = k_none;
  /* first filter out idle values */
  if ( (keycode = filter_idle_value(keycode)) ) {
    if (verbose > 3)
      printk(KERN_INFO"acerhk: received key code 0x%x\n", keycode);
    /* translate keycode to key name */
    if (keycode >= 0 && keycode <= 255)
      keyname = acerhk_key2name[keycode];
    else {
      if (verbose > 3)
        printk(KERN_INFO"acerhk: keycode 0x%x too big, will use only 8 bits\n", keycode);
      /* use only lower 8 bits of value to distinguish keys */
      keyname = acerhk_key2name[keycode&0xff];
    }
    /* produce some log information for higher verbosity levels */
    if (keyname != k_none && verbose > 2)
      printk(KERN_INFO"acerhk: translated acer key code 0x%x to key name 0x%x\n",
             keycode, keyname);
    else if (keyname == k_none && verbose > 3)
      printk(KERN_INFO"acerhk: translated acer key code 0x%x to no key\n",
             keycode);
    if (autowlan) {
      /* if automatic switching of wlan hardware is enabled, do it here
         on wireless key press */
      if (keyname == k_wireless2) {
        if (acerhk_bluetooth_state)
          wbutton_fct_1(0);
        else
          wbutton_fct_1(1);
      }
      if (keyname == k_wireless) {
        if (acerhk_wlan_state)
          wbutton_fct_2(0);
        else
          wbutton_fct_2(1);
      }
    }
  }
  return keyname;
}

/* polling timer handler */
static void acerhk_poll_event(unsigned long save_size)
{
/* #ifndef DUMMYHW */
#if !(defined(DUMMYHW) || defined(__x86_64__))
  unsigned int max = MAX_POLLING_LOOPS;
  /* make sure not to loop more then 32 times */
  if (!max || max > 32)
    max = 32;
  if (acerhk_type != TM_dritek) {
    while (get_nr_events() && max--) {
      send_key_event(transl8_key_code(get_fnkey_event()));
    }
  } else {
    send_key_event(transl8_key_code(get_fnkey_event()));
  }
#endif
  acerhk_timer_poll.expires = jiffies + acerhk_polling_delay;
  add_timer(&acerhk_timer_poll);
}

/* blinking timer handler; added by Antonio Cuni */
static void acerhk_blink_event(unsigned long not_used)
{
  if (acerhk_blueled_blinking != -1) {
    acerhk_blueled_blinking = !acerhk_blueled_blinking;
/* #ifndef DUMMYHW */
#if !(defined(DUMMYHW) || defined(__x86_64__))
    wbutton_fct_1(acerhk_blueled_blinking);
#endif
    acerhk_timer_blinking.expires = jiffies + acerhk_blueled_blinking_delay;
    add_timer(&acerhk_timer_blinking);
  }
  else 
    printk(KERN_WARNING "acerhk: blinking event called, but blinking not active\n");
}

static void init_input(void)
{
  int i;

#ifndef KERNEL26
  /* request keyboard input module */
  request_module("keybdev");
  if (verbose > 3)
    printk(KERN_INFO"requested keyboard input driver\n");
#endif

#ifndef STATIC_INPUT_DEV
  /* allocate acerhk input device */
  acerhk_input_dev_ptr=input_allocate_device();
  /* enter some name */
  acerhk_input_dev_ptr->name = "Acer hotkey driver";
#else
  acerhk_input_dev_ptr=&acerhk_input_dev;
#endif   
  
  /* some laptops have a mail led, should I announce it here? */
  acerhk_input_dev_ptr->evbit[0] = BIT(EV_KEY);
  /* announce keys to input system
   * the generated keys can be changed on runtime,
   * but to publish those changes the device needs to
   * get reconnected (I dont't know any other way)
   * Therefore I enable all possible keys */
  for (i = KEY_RESERVED; i < BTN_MISC; i++)
    set_bit(i, acerhk_input_dev_ptr->keybit);
  /* set mapping keyname -> input event */
  init_keymap_input();
  if (verbose)
    printk(KERN_INFO"acerhk: registered input device\n");
  input_register_device(acerhk_input_dev_ptr);
  init_timer(&acerhk_timer_poll);
  acerhk_polling_state = 0;
}

static void stop_polling(void)
{
  if (acerhk_polling_state == 1) {
    del_timer(&acerhk_timer_poll);
    if (verbose)
      printk(KERN_INFO"acerhk: key polling stopped\n");
    acerhk_polling_state = 0;
  } else
    if (verbose)
      printk(KERN_INFO"acerhk: key polling not active\n");
}

static void start_polling(void)
{
  if (acerhk_polling_state != 1) {
    acerhk_timer_poll.function = acerhk_poll_event;
    acerhk_timer_poll.expires = jiffies + acerhk_polling_delay;
    acerhk_timer_poll.data = get_nr_events();
    add_timer(&acerhk_timer_poll);
    acerhk_polling_state = 1;
    if (acerhk_type == TM_dritek) {
      printk(KERN_INFO"acerhk: Your hardware does not need polling enabled for hotkeys to work, "
             "you can safely disable polling by using the module parameter poll=0 (unless you "
             "want to play around with the driver and see if there are buttons which need polling).\n");
    }
    if (verbose)
      printk(KERN_INFO"acerhk: starting key polling, every %d ms\n", acerhk_polling_delay);
  } else
    if (verbose)
      printk(KERN_INFO"acerhk: key polling already active\n");
}

/* addedd by Antonio Cuni */
static void start_blinking(void)
{
  if (acerhk_blueled_blinking == -1) {
    // blinking was disabled... enable it!
    acerhk_timer_blinking.function = acerhk_blink_event;
    acerhk_timer_blinking.expires = jiffies + acerhk_blueled_blinking_delay;
    acerhk_timer_blinking.data = 0; // not used
    add_timer(&acerhk_timer_blinking);
    acerhk_blueled_blinking = 0;
    if (verbose)
      printk(KERN_INFO "acerhk: starting blueled blinking\n");
  } else
    if (verbose)
      printk(KERN_INFO "acerhk: blueled already blinking\n");
}

/* Added by Antonio Cuni */
static void stop_blinking(void)
{
  if (acerhk_blueled_blinking != -1) {
    del_timer(&acerhk_timer_blinking);
    if (verbose)
      printk(KERN_INFO "acerhk: blueled blinking stopped\n");
    acerhk_blueled_blinking = -1;
  }
}

static void release_input(void)
{
  stop_polling();
  input_unregister_device(acerhk_input_dev_ptr);
}

/* }}} */

/* {{{ procfs functions */

#ifndef CONFIG_PROC_FS

static int acerhk_proc_init(void)
{
  return 1;
}
#else

/* This macro frees the machine specific function from bounds checking and
 * things like that... */
#define	PRINT_PROC(fmt,args...)                     \
  do {												\
    *len += sprintf( buffer+*len, fmt, ##args );	\
    if (*begin + *len > offset + size)				\
      return( 0 );                                  \
    if (*begin + *len < offset) {					\
      *begin += *len;								\
      *len = 0;                                     \
    }												\
  } while(0)

static int pc_proc_infos( char *buffer, int *len,
                          off_t *begin, off_t offset, int size )
{
  PRINT_PROC( "Acer hotkeys version %s\n", ACERHK_VERSION);
  PRINT_PROC( "Model(Type)\t: %s(", acerhk_model_string);
  switch(acerhk_type) {
  default:
    PRINT_PROC( "unknown)\n");
    break;
  case TM_old:
    PRINT_PROC( "old)\n");
    break;
  case TM_new:
    PRINT_PROC( "new)\n");
    break;
  case TM_dritek:
    PRINT_PROC( "Dritek)\n");
    break;
  }
  if (bios_routine != 0) {
    PRINT_PROC( "request handler\t: 0x%x\n", bios_routine);
    if (cmos_index) {
      PRINT_PROC( "CMOS index\t: 0x%x\n", cmos_index);
      PRINT_PROC( "events pending\t: %u\n", get_nr_events());
    } else {
      PRINT_PROC( "CMOS index\t: not available\n");
    }
    if (acerhk_polling_state == 1)
      PRINT_PROC( "kernel polling\t: active\n");
    else
      PRINT_PROC( "kernel polling\t: inactive\n");
    PRINT_PROC( "autoswitch wlan\t: ");
    if (autowlan == 1)
      PRINT_PROC( "enabled\n");
    else
      PRINT_PROC( "disabled\n");
  } else {
    PRINT_PROC( "request handler\t: not found\n");
    PRINT_PROC( "kernel polling\t: not possible\n");
  }
  /* model specific infos */
  if (acerhk_type == TM_dritek) {
    PRINT_PROC( "use of Dritek EC: ");
    if (usedritek)
      PRINT_PROC( "enabled\n");
    else
      PRINT_PROC( "disabled\n");
  }
  if (acerhk_type == TM_old)
    PRINT_PROC( "preg400\t\t: 0x%p\n", preg400);
  return (1);
}

static int acerhk_proc_info( char *buffer, char **start, off_t offset,
                             int size, int *eof, void *data )
{
  int len = 0;
  off_t begin = 0;
  
  *eof = pc_proc_infos( buffer, &len, &begin, offset, size );
  
  if (offset >= begin + len)
    return( 0 );
  *start = buffer + (offset - begin);
  return( size < begin + len - offset ? size : begin + len - offset );
  
}

static int acerhk_proc_key( char *buffer, char **start, off_t offset,
                            int size, int *eof, void *data )
{
  if (size >= 5 && offset == 0) {
    if (acerhk_type == TM_dritek || acerhk_polling_state == 1) {
      snprintf(buffer+offset, size, "n/a\n");
    } else {
      snprintf(buffer+offset, size, "0x%02x\n", filter_idle_value(get_fnkey_event()));
    }
    *eof = 1;
    return 5;
  }
  *eof = 1;
  return 0;
}

static int acerhk_proc_led(struct file* file, const char* buffer,
                           unsigned long count, void* data)
{
  char str[4];
  int len;
  if (count > 4)
    len = 4;
  else
    len = count;
  if (copy_from_user(str, buffer, len))
    return -EFAULT;
  str[3] = '\0';
  if ( ( (len >= 2) && (!strncmp(str, "on", 2) || !strncmp(str, "an", 2)) )
       || str[0] == '1')
    set_mail_led(1);
  else
    set_mail_led(0);
  return len;
}

static int acerhk_proc_wirelessled(struct file* file, const char* buffer,
                                   unsigned long count, void* data)
{
  char str[4];
  int len;
  if (count > 4)
    len = 4;
  else
    len = count;
  if (copy_from_user(str, buffer, len))
    return -EFAULT;
  str[3] = '\0';
  if ( ( (len >= 2) && (!strncmp(str, "on", 2) || !strncmp(str, "an", 2)) )
       || str[0] == '1') {
    if (acerhk_model_features & TM_F_WLAN_EC1)
      enable_wlan_ec_1();
    else if (acerhk_model_features & TM_F_WLAN_EC2)
      enable_wlan_ec_2();
    else
      wbutton_fct_2(1);
  }
  else {
    if (acerhk_model_features & TM_F_WLAN_EC1)
      disable_wlan_ec_1();
    else if (acerhk_model_features & TM_F_WLAN_EC2)
      disable_wlan_ec_2();
    else
      wbutton_fct_2(0);
  }
  return len;
}


/* Modified by Antonio Cuni: added support for blinking
   possible values:
   - off, 0:       led always off
   - on, an,  1:   led alway on
   - n (a number): led blinking; n is the delay between 
   two changes of state, in jiffies; n must
   be > 50, to prevent the user from overloading
   the kernel.

 */
static int acerhk_proc_blueled(struct file* file, const char* buffer,
                               unsigned long count, void* data)
{
  const int MAXLEN=11;
  char str[MAXLEN];
  int len;
  int isNumber;

  if (count > MAXLEN)
    len = MAXLEN;
  else
    len = count;
  if (copy_from_user(str, buffer, len))
    return -EFAULT;
  str[MAXLEN - 1] = '\0';

  /* try to parse a number */
  isNumber = sscanf(str, "%u", &acerhk_blueled_blinking_delay);
  /* if the delay is 0, turn off the led */
  if (isNumber && acerhk_blueled_blinking_delay != 0 && acerhk_blueled_blinking_delay != 1) {
    if (acerhk_blueled_blinking_delay < 50)
      printk(KERN_INFO"acerhk: blinking request rejected. The delay must be > 50.\n");
    else {
      if (verbose)
        printk(KERN_INFO"acerhk: blinking delay set to %u.\n", acerhk_blueled_blinking_delay);
      start_blinking();
    }
  } else if (acerhk_blueled_blinking_delay == 1 || !strncmp(str, "on", 2) || !strncmp(str, "an", 2)) {
    stop_blinking();
    if (acerhk_model_features & TM_F_BLUE_EC1)
      enable_bluetooth_ec_1();
    else if (acerhk_model_features & TM_F_BLUE_EC2)
      enable_bluetooth_ec_2();
    else
      wbutton_fct_1(1);
  } else {
    /* it's 0 or everything else */
    stop_blinking();
    if (acerhk_model_features & TM_F_BLUE_EC1)
      disable_bluetooth_ec_1();
    else if (acerhk_model_features & TM_F_BLUE_EC2)
      disable_bluetooth_ec_2();
    else
      wbutton_fct_1(0);
  }
  return len;
}

#ifdef ACERDEBUG
static void do_debug(const char* buffer, unsigned long len)
{
  unsigned int h, i;
  switch (buffer[0]) {
  case 'b':
    /* test WLAN on TM 4001 */
    switch (buffer[1]) {
    case '0':
      disable_wlan_ec_1();
      break;
    case '1':
    default:
      enable_wlan_ec_1();
    }
    break;
  case 'B':
    /* test BLUETOOTH on TM 4001 */
    switch (buffer[1]) {
    case '0':
      disable_bluetooth_ec_1();
      break;
    case '1':
    default:
      enable_bluetooth_ec_1();
    }
    break;
  case 'D':
    /* test "DMM Function Enabled" entry of TM 4150/4650 */
    enable_dmm_function();
    break;
  case 'i':
  case '1':
#ifndef KERNEL26
    MOD_INC_USE_COUNT;
#endif
    break;
  case 'e':
    switch (buffer[1]) {
    case '1':
      start_polling();
      break;
    default:
      stop_polling();
    }
    break;
  case 'k':
    for (i = 0; i <= 255;i++) {
      input_report_key(acerhk_input_dev_ptr, i, 1);
      input_report_key(acerhk_input_dev_ptr, i, 0);
    }
    break;
  case 'm':
    /* set mapping key names -> input events */
    sscanf(&buffer[2],"%x", &i);
    h = buffer[1] - '0' + 1;
    printk("acerhk: key name %x maps to %x\n", h, i);
    acerhk_name2event[h] = i;
    break;
  case 'M':
    /* test mute LED on dritek hardware */
    switch (buffer[1]) {
    case '0':
      disable_mute_led_ec();
      break;
    case '1':
    default:
      enable_mute_led_ec();
    }
    break;
  case 'p':
    printk("acerhk: pbutton = 0x%x\n", pbutton_fct());
    break;
  case 's':
    /* send key event to test the key translation in input system */
    sscanf(&buffer[1],"%x", &h);
    printk("acerhk: sending key event 0x%x\n", h);
    input_report_key(acerhk_input_dev_ptr, h, 1);
    input_report_key(acerhk_input_dev_ptr, h, 0);
    break;
  case 'S':
    /* simulate key codes to test the key translation in acerhk */
    sscanf(&buffer[1],"%x", &h);
    send_key_event(transl8_key_code(h));
    break;
  case 't':
    printk("acerhk: thermal event = 0x%x\n", get_thermal_event());
    break;
  case 'w':
    /* test the wbutton functions, someone really needs to have another look
       at the windows driver */
    switch (buffer[1]) {
    case '2':
      printk("acerhk: wbutton_2(%d) = 0x%x\n", buffer[2]-'0', wbutton_fct_2(buffer[2]-'0'));
      break;
    case '1':
    default:
      printk("acerhk: wbutton_1(%d) = 0x%x\n", buffer[2]-'0', wbutton_fct_1(buffer[2]-'0'));
    }
    break;
  case 'W':
    /* test wireless HW/LED on some models using dritek hardware */
    switch (buffer[1]) {
    case '0':
      disable_wireless_ec();
      break;
    case '1':
    default:
      enable_wireless_ec();
    }
    break;
  case 'v':
    verbose = buffer[1]-'0';
    printk("acerhk: verbosity level changed to %d\n", verbose);
    break;
  case 'd':
  case '0':
  default:
#ifndef KERNEL26
    MOD_DEC_USE_COUNT;
#endif
    break;
  }
}

static int acerhk_proc_debug(struct file* file, const char* buffer,
                             unsigned long count, void* data)
{
  char str[5];
  int len;
  if (count > 5)
    len = 5;
  else
    len = count;
  if (copy_from_user(str, buffer, len))
    return -EFAULT;
  str[4] = '\0';
  do_debug(str, len);
  return len;
}
#endif

static int acerhk_proc_init(void)
{
  int retval;
  struct proc_dir_entry *entry;
  /* create own directory */
  proc_acer_dir = proc_mkdir("driver/acerhk", NULL);
  if (proc_acer_dir == NULL) {
    retval = 0;
    printk(KERN_INFO"acerhk: could not create /proc/driver/acerhk\n");
  }
  else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
    proc_acer_dir->owner = THIS_MODULE;
#endif
    /* now create several files, first general info ... */
    entry = create_proc_read_entry("info",
                                   0444, proc_acer_dir, acerhk_proc_info, NULL);
    if (entry == NULL) {
      printk(KERN_INFO"acerhk: cannot create info file\n");
      remove_proc_entry("driver/acerhk", NULL);
      retval = 0;
    } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
      entry->owner = THIS_MODULE;
#endif
      /* ... last pressed key ... */
      entry = create_proc_read_entry("key",
                                     0444, proc_acer_dir, acerhk_proc_key, NULL);
      if (entry == NULL) {
        printk(KERN_INFO"acerhk: cannot create key file\n");
        remove_proc_entry("info", proc_acer_dir);
        remove_proc_entry("driver/acerhk", NULL);
        retval = 0;
      } else {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
        entry->owner = THIS_MODULE;
#endif
        /* ... and led control file */
        entry = create_proc_entry("led", 0222, proc_acer_dir);
        if (entry == NULL) {
          printk(KERN_INFO"acerhk: cannot create LED file\n");
          remove_proc_entry("info", proc_acer_dir);
          remove_proc_entry("key", proc_acer_dir);
          remove_proc_entry("driver/acerhk", NULL);
          retval = 0;
        }
        else {
          entry->write_proc = acerhk_proc_led;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
          entry->owner = THIS_MODULE;
#endif
          /* ... and wireless led controll file */
          entry = create_proc_entry("wirelessled", 0222, proc_acer_dir);
          if (entry == NULL) {
            printk(KERN_INFO"acerhk: cannot create wirelessled file\n");
            remove_proc_entry("info", proc_acer_dir);
            remove_proc_entry("key", proc_acer_dir);
            remove_proc_entry("led", proc_acer_dir);
            remove_proc_entry("driver/acerhk", NULL);
            retval = 0;
          }
          else {
            entry->write_proc = acerhk_proc_wirelessled;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
            entry->owner = THIS_MODULE;
#endif
            /* ... and bluetooth led controll file */
            entry = create_proc_entry("blueled", 0222, proc_acer_dir);
            if (entry == NULL) {
              printk(KERN_INFO"acerhk: cannot create blueled file\n");
              remove_proc_entry("info", proc_acer_dir);
              remove_proc_entry("key", proc_acer_dir);
              remove_proc_entry("led", proc_acer_dir);
              remove_proc_entry("wirelessled", proc_acer_dir);
              remove_proc_entry("driver/acerhk", NULL);
              retval = 0;
            } else {
              entry->write_proc = acerhk_proc_blueled;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
              entry->owner = THIS_MODULE;
#endif
              retval = 1;
#ifdef ACERDEBUG
              /* add extra file for debugging purposes */
              entry = create_proc_entry("debug", 0222, proc_acer_dir);
              if (entry == NULL) {
                printk(KERN_INFO"acerhk: cannot create debug file\n");
                remove_proc_entry("info", proc_acer_dir);
                remove_proc_entry("key", proc_acer_dir);
                remove_proc_entry("led", proc_acer_dir);
                remove_proc_entry("wirelessled", proc_acer_dir);
                remove_proc_entry("blueled", proc_acer_dir);
                remove_proc_entry("driver/acerhk", NULL);
                retval = 0;
              }
              else {
                entry->write_proc = acerhk_proc_debug;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
                entry->owner = THIS_MODULE;
#endif
                retval = 1;
              }
#endif
            }
          }
        }
      }
    }
  }
  return retval;
}

static void acerhk_proc_cleanup(void)
{
  if (proc_acer_dir) {
    remove_proc_entry("info", proc_acer_dir);
    /* On dritek type hardware key file is already removed */
    if (acerhk_type != TM_dritek)
      remove_proc_entry("key", proc_acer_dir);
    remove_proc_entry("led", proc_acer_dir);
    remove_proc_entry("wirelessled", proc_acer_dir);
    remove_proc_entry("blueled", proc_acer_dir);
#ifdef ACERDEBUG
    remove_proc_entry("debug", proc_acer_dir);
#endif
    remove_proc_entry("driver/acerhk", NULL);
    proc_acer_dir = NULL;
  }
}

#endif /* CONFIG_PROC_FS */

/* }}} */

/* {{{ file operations */

static int acerhk_ioctl( struct inode *inode, struct file *file,
                         unsigned int cmd, unsigned long arg )
{
  int retval;
  switch( cmd ) {
  case ACERHK_GET_KEYCOUNT:
    {
      char nr;
      nr = get_nr_events();
      put_user(nr, (char*)arg);
      retval = 0;
      break;
    }
  case ACERHK_GET_KEYID:
    {
      char id;
      id = get_fnkey_event();
      put_user(id, (char*)arg);
      retval = 0;
      break;
    }
  case ACERHK_CONNECT:
    launch_connect(1);
    retval = 0;
    break;
  case ACERHK_START_POLLING:
    start_polling();
    retval = 0;
    break;
  case ACERHK_STOP_POLLING:
    stop_polling();
    retval = 0;
    break;
  case ACERHK_DISCONNECT:
    launch_connect(0);
    retval = 0;
    break;
  case ACERHK_GET_THERMAL_EVENT:
    {
      short event;
      event = get_thermal_event();
      put_user(event, (short*)arg);
      retval = 0;
      break;
    }
  case ACERHK_MAIL_LED_OFF:
    set_mail_led(0);
    retval = 0;
    break;
  case ACERHK_MAIL_LED_ON:
    set_mail_led(1);
    retval = 0;
    break;
  case ACERHK_GET_KEY_MAP:
    if (copy_to_user((t_map_name2event*)arg, &acerhk_name2event, sizeof(acerhk_name2event)))
      retval = -EFAULT;
    else
      retval = 0;
    break;
  case ACERHK_SET_KEY_MAP:
    if (copy_from_user(&acerhk_name2event, (t_map_name2event*)arg, sizeof(acerhk_name2event)))
      retval = -EFAULT;
    else {
      if (verbose) {
        printk(KERN_INFO"acerhk: changed key mapping\n");
        print_mapping();
      }
      retval = 0;
    }
    break;
  default:
    retval = -EINVAL;
  }
  return retval;
}

#ifdef ACERDEBUG
static ssize_t acerhk_write (struct file* file, const char* buffer, size_t length, loff_t* offset)
{
  if (length)
    do_debug(buffer, length);
  return length;
}
#endif

static int acerhk_open( struct inode *inode, struct file *file )
{
  return 0;
}

static int acerhk_release( struct inode *inode, struct file *file )
{
  return 0;
}

#ifdef CONFIG_PM
static int acerhk_resume(struct platform_device *dev)
{
	printk(KERN_INFO"acerhk: Resuming. Setting wlan_state to: %d\n", acerhk_wlan_state);

	if (acerhk_wlan_state)
	  wbutton_fct_2(1);
	else
	  wbutton_fct_2(0);

    return 0;
}
#endif

static struct file_operations acerhk_fops = {
  owner:        THIS_MODULE,
  ioctl:        acerhk_ioctl,
  open:         acerhk_open,
#ifdef ACERDEBUG
  write:        acerhk_write,
#endif
  release:      acerhk_release,
};

static struct miscdevice acerhk_misc_dev = {
  .minor = MISC_DYNAMIC_MINOR,
  .name  = "acerhk",
  .fops  = &acerhk_fops,
};

/* }}} */

static void __devinit model_init(void)
{
  /* set callroutine, features and keymap for model */
  setup_model_features(acerhk_series);
  /* override initial state of wireless hardware if specified by module options */
  if (wlan_state >= 0) acerhk_wlan_state = wlan_state;
  if (bluetooth_state >= 0) acerhk_bluetooth_state = bluetooth_state;
  /* Launch connect only if available */
  if (acerhk_model_features & TM_F_CONNECT) {
    if (verbose)
      printk(KERN_INFO"acerhk: Model type %d, calling launch_connect(1)\n",
             acerhk_type);
    launch_connect(1);
  }
  if ( acerhk_type != TM_dritek ) {
    get_cmos_index();
  }
  if ( acerhk_type == TM_dritek ) {
    enable_dritek_keyboard();
  }
  /* added by Antonio Cuni */
  init_timer(&acerhk_timer_blinking);
}


static int __devexit acerhk_remove(struct platform_device *dev);

static int __devinit acerhk_probe(struct platform_device *dev)
{
  int ret;

  ret = misc_register(&acerhk_misc_dev);
  if (ret) {
    printk(KERN_ERR "acerhk: can't misc_register on minor=%d\n", ACERHK_MINOR);
    ret = -EAGAIN;
  }
  else if (!acerhk_proc_init()) {
    printk(KERN_ERR "acerhk: can't create procfs entries\n");
    ret = -ENOMEM;
    misc_deregister( &acerhk_misc_dev );
  }
  else {
    reg1 = ioremap(0xf0000, 0xffff);
    if (verbose > 1)
      printk(KERN_INFO"acerhk: area from 0xf000 to 0xffff mapped to %p\n", reg1);
    reg2 = ioremap(0xe0000, 0xffff);
    if (verbose > 1)
      printk(KERN_INFO"acerhk: area from 0xe000 to 0xffff mapped to %p\n", reg2);
    /* the area 0x400 is used as data area by earlier (520) series  */
    preg400 = ioremap(0x400, 0xfff);
    if (verbose > 1)
      printk(KERN_INFO"acerhk: area from 0x400 to 0x13ff mapped to %p\n", preg400);
    /* attach to input system */
    init_input();
    memset(acerhk_model_string, 0x00, ACERHK_MODEL_STRLEN);
/* #ifdef DUMMYHW */
#if (defined(DUMMYHW) || defined(__x86_64__))
    acerhk_model_addr = (void*)0x12345678;
    /* copy the string, but not more than 15 characters */
    strncpy(acerhk_model_string, "TravelmateDummy", ACERHK_MODEL_STRLEN-1);
    /* set callroutine for model */
    if (force_series)
      acerhk_series = force_series;
    else
      acerhk_series = 2000;
    setup_model_features(acerhk_series);
    printk(KERN_INFO "Acer Travelmate hotkey driver v" ACERHK_VERSION " dummy\n");
    if ( acerhk_type == TM_dritek )
      enable_dritek_keyboard();
    if (poll)
      start_polling();
    init_timer(&acerhk_timer_blinking);
#else
    bios_routine = find_hk_area();
    if (!force_series)
      probe_model();
    else {
      if (verbose)
        printk(KERN_INFO"acerhk: forced laptop series to %d\n", force_series);
      acerhk_series = force_series;
    }
    /* do model specific initialization */
    model_init();
    /* Without a bios routine we cannot do anything except on dritek
       type HW, unload on other types */
    if (bios_routine || (acerhk_type == TM_dritek)) {
      ret = 0;
      if (verbose && bios_routine)
        printk(KERN_INFO"acerhk: bios routine found at 0x%x\n", bios_routine);
      printk(KERN_INFO "Acer Travelmate hotkey driver v" ACERHK_VERSION "\n");
      /* If automatic switching of wlan is wanted but polling is disabled,
         automatically enable it */
      if (!poll && autowlan) {
        printk(KERN_INFO "Automatic switching of wireless hardware needs polling, enabling it\n");
        poll = 1;
      }
      /* start automatic polling of key presses if wanted and bios routine found */
      if (poll && bios_routine)
        start_polling();
    } else {
      printk(KERN_ERR "acerhk: can't find bios routine, cannot do anything for you, sorry!\n");
      ret = -ENOMEM;
      return acerhk_remove(dev);
    }
#endif
  }
  return ret;
}

static int __devexit acerhk_remove(struct platform_device *dev)
{
  acerhk_proc_cleanup();
  stop_blinking();
  if (reg1)
    iounmap(reg1);
  if (reg2)
    iounmap(reg2);
  if (preg400)
    iounmap(preg400);
  release_input();
  misc_deregister(&acerhk_misc_dev);
  if ( acerhk_type == TM_dritek ) {
    disable_dritek_keyboard();
  }
  if (verbose > 2)
    printk(KERN_INFO "acerhk: unloaded\n");

  return 0;
}

static struct platform_driver acerhk_driver = {
	.driver		= {
		.name	= "acerhk",
		.owner	= THIS_MODULE,
	},
	.probe		= acerhk_probe,
	.remove		= __devexit_p(acerhk_remove),
#ifdef CONFIG_PM
	.resume		= acerhk_resume,
#endif
};

static struct platform_device *acerhk_platform_device;

static int __init acerhk_init(void)
{
	int error;

	error = platform_driver_register(&acerhk_driver);
	if (error)
		return error;

	acerhk_platform_device = platform_device_alloc("acerhk", -1);
	if (!acerhk_platform_device) {
		error = -ENOMEM;
		goto err_driver_unregister;
	}

	error = platform_device_add(acerhk_platform_device);
	if (error)
		goto err_free_device;


	return 0;

 err_free_device:
	platform_device_put(acerhk_platform_device);
 err_driver_unregister:
	platform_driver_unregister(&acerhk_driver);
	return error;
}

static void __exit acerhk_exit(void)
{
	platform_device_unregister(acerhk_platform_device);
	platform_driver_unregister(&acerhk_driver);
	printk(KERN_INFO "acerhk: removed.\n");
}


module_init(acerhk_init);
module_exit(acerhk_exit);

MODULE_AUTHOR("Olaf Tauber");
MODULE_DESCRIPTION("AcerHotkeys extra buttons keyboard driver");
MODULE_LICENSE("GPL");

#ifndef KERNEL26
EXPORT_NO_SYMBOLS;
#endif

#else
#error This driver is only available for X86 architecture
#endif
/*
 * Local variables:
 *  c-indent-level: 4
 *  tab-width: 4
 * End:
 */

