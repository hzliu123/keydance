/*
 * Key dancing Linux kernel module
 *
 * This module uses Numlock, Capslock and Scrolllock LED as indicator.
 * When a LED lights up, a matching key should be pressed. Different  
 * LEDs would light up simultaneously. When the game starts, LED pattern
 * changes every 2 second. The speed of the change would increase over
 * time. The game stops when a player reaches level 10 or when he misses
 * to react to 10 LED patterns.
 * 
 * Key matches:
 * Numlock    LED: number key 1
 * Capslock   LED: number key 2
 * Scrolllock LED: number key 3
 *
 * There are two /proc files for starting games and displaying results:
 * /proc/keydance-start: start by writing any chars to it
 * /proc/keydance-result: game statistics (hits, misses, current level)
 *
 * As a kernel programming homework, it cover topics of:
 * kernel module, dynamic timer, IRQ handler, IRQ thread, spin_lock, 
 * IO port access, procfs, seq_file and etc.
 */

#include <linux/module.h>		/* for module_init() */
#include <linux/seq_file.h>		/* for single_open() */
#include <linux/proc_fs.h>		/* for proc_create() */
#include <linux/random.h>		/* for get_random_bytes() */
#include <linux/interrupt.h>		/* for request_irq() */
#include "i8042.h"			/* for LED control */

/* filename for /proc interface */
static const char *keydance_start_fname = "keydance-start";
static const char *keydance_result_fname = "keydance-result";

static struct timer_list keydance_timer;

/* Tracking the states of all LEDs 
 * bit 0: scrolllock, key
 * bit 1: numlock, 
 * bit 2: capslock */
static unsigned char lock_state;

/* Protect i8042 LED operations, lock_state, timer.
   Source of concurrency: timer, interrupt, irq thread, proc write process */
static spinlock_t keydance_lock;

/* 4, 2, 3 are the scancodes for key 1, key 2, key 3 respectively */
static const char dancekey_scancode_table[3] = { 4, 2, 3 };

/* Game statistics */
static bool game_running = false; /* two modes: running and stop mode */ 
static int extras;  /* wrong key pressed? To indicate a miss */
static int misses;  /* Total patterns players reacts wrong */
static int hits;    /* Total patterns players reacts correctly */
static int level;   /* Game level, control pattern changing speed */

#define HITS_PER_LEVEL 10  /* increase game level every 10 hits */
#define MISSES_TO_STOP 10  /* stop game when misses >= 10 */
#define LEVEL_TO_STOP 10   /* stop game when level = 10 */

/* delay time before changing to next LED pattern */
static int step_time(int level)
{
	return HZ*(20-2*level)/10;
}

/* Main logics of this game is here 
 * 1. lock_state should be 0 if users hits all required key 
 * 2. calculate new lock_state
 * 3. update extras, hits, misses and level, etc.
 * 4. update LEDs
 * 5. set timer for next expire
 */
static void keydance_timerfn(unsigned long unused)
{
	unsigned char old_state;

	spin_lock(&keydance_lock);
	if (!game_running)
		goto stop;

	/* refresh lock_state and save its old value */
	old_state = lock_state;
	get_random_bytes(&lock_state, 1);
	lock_state &= 0x07;

        /* if user hits all required keys, old_state should be 0 */
	if (old_state || extras) {
		misses++;
		if (misses >= MISSES_TO_STOP)
			goto stop;
	} else {
		hits++;
		level = hits / HITS_PER_LEVEL;
		if (level >= LEVEL_TO_STOP)
			goto stop;
	}
	extras = 0;

	/* set LEDs to new state */
	i8042_led_blink(lock_state);
	mod_timer(&keydance_timer, jiffies + step_time(level));
	spin_unlock(&keydance_lock);
	return;
stop:
	game_running = false;
	i8042_led_blink(0);
	spin_unlock_irq(&keydance_lock);
	return;
}

/* Before starting the game:
 * 1. Reset all states: lock_state, misses, hits, level and etc.
 * 2. Reset LEDs
 * 3. setup timer
 */
static ssize_t write_keydance_start(struct file *file, const char __user *buf,
                                    size_t count, loff_t *ppos)
{
        if (!game_running && count) {
		/* reset stats and clear LEDs before game starts */
		lock_state = 0;
		i8042_led_blink(lock_state);
		extras = 0;
		misses = 0;
		hits = 0;
		level = 0;
		game_running = true;
		mod_timer(&keydance_timer, jiffies + step_time(level));
        }
        return count;
}

/* /proc/keydance-start is write only. Define only the write method */
static const struct file_operations keydance_start_proc_fops = {
        .write = write_keydance_start,
};

/* /proc/keydance-result seq_file show method 
 * This file shows game status.
 */
static int keydance_result_proc_show(struct seq_file *m, void *v)
{
	if (!game_running)
		seq_printf(m, "**** STOPPED ****\n" \
		           "To start: echo 1 > /proc/%s\n" \
			   "Game over when misses >= %d\n", \
			   keydance_start_fname, MISSES_TO_STOP);
	else
		seq_printf(m, ">>>> RUNNING >>>>\n");
	seq_printf(m, "\nGame stats:\n" \
                   "Level: %d (step time = %d ms)\n" \
                   "Hits: %d, Misses: %d\n", \
                   level, jiffies_to_msecs(step_time(level)), \
		   hits, misses);
	return 0;
}

static int keydance_result_proc_open(struct inode *inode, struct file *file)
{
        return single_open(file, keydance_result_proc_show, NULL);
}

static const struct file_operations keydance_result_proc_fops = {
        .open           = keydance_result_proc_open,
        .read           = seq_read,
        .llseek         = seq_lseek,
        .release        = single_release,
};

/* flashing all 3 LEDs 5 times */
static void led_test(void) {
	int total, delay = 200;
	char state = 0;

	for (total = 0; total < 1200; ) {
		state ^= I8042_LED_CAPSLOCK | I8042_LED_NUMLOCK | \
                         I8042_LED_SCROLLLOCK;
		total += i8042_led_blink(state);
		msleep(delay);
		total += delay;
	}
}

/* IRQ thread:
 * 1. Find input key in the dancekey scancode table. 
 * 2. If it's a match key, clear corresponding bit of lock_state and update 
 *    LED accordingly.
 * 3. If it's a wrong key, count it in extras, which is used by timerfn to 
 *    calculate misses 
 */
static irqreturn_t keydance_threadfn(int irq, void *id) {
	int i;
	unsigned char scancode;

	/* timerfn and threadfn may run on different CPUs */
	spin_lock_irq(&keydance_lock);
	if (!game_running)
		goto end;

	scancode = i8042_read_data();
	for (i=0; i<3; i++)
		if (scancode == dancekey_scancode_table[i])
			break;
	if (i<3) {
		if (lock_state & (1<<i)) {
			lock_state &= ~(1<<i);
			i8042_led_blink(lock_state);
		} else
			extras++;
	}
end:
	spin_unlock_irq(&keydance_lock);
	return IRQ_HANDLED;
}

/* interrupt handler: 
 * Only wake up irq thread to do the job.
 */
static irqreturn_t keydance_interrupt(int irq, void *id) {
	if (!game_running)
		return IRQ_NONE;

	return IRQ_WAKE_THREAD;
}

static int __init keydance_init(void) {
	struct proc_dir_entry *entry;
	int error;

	spin_lock_init(&keydance_lock);
	error = request_threaded_irq(I8042_KBD_IRQ, keydance_interrupt,
				     keydance_threadfn, IRQF_SHARED, "keydance", 
                                     &lock_state);
	if (error)
		return error;

	entry = proc_create(keydance_start_fname, S_IWUGO, NULL, \
			    &keydance_start_proc_fops);
	if (IS_ERR_OR_NULL(entry))
		goto fail1;
	entry = proc_create(keydance_result_fname, S_IRUGO, NULL, \
                            &keydance_result_proc_fops);
	if (IS_ERR_OR_NULL(entry))
		goto fail2;

	init_timer(&keydance_timer);
	keydance_timer.function = keydance_timerfn;

	led_test();
	return 0;
fail2:
	remove_proc_entry(keydance_start_fname, NULL);
fail1:
	return -ENOMEM;
}

static void __exit keydance_exit(void) {
	/* CAUTION: Undo in the right order and note possible race conditions!
                    May need to wait for a game to end */
	game_running = false;
	del_timer_sync(&keydance_timer);
	i8042_led_blink(0);
	remove_proc_entry(keydance_result_fname, NULL);
	remove_proc_entry(keydance_start_fname, NULL);
	free_irq(I8042_KBD_IRQ, &lock_state);
}

MODULE_LICENSE ("GPL");
MODULE_AUTHOR("Joseph Liu");
module_init(keydance_init);
module_exit(keydance_exit);
