/*
 * twl4030_keypad.c - driver for 8x8 keypad controller in twl4030 chips
 *
 * Copyright (C) 2007 Texas Instruments, Inc.
 * Copyright (C) 2008 Nokia Corporation
 *
 * Code re-written for 2430SDP by:
 * Syed Mohammed Khasim <x0khasim@ti.com>
 *
 * Initial Code:
 * Manjunatha G K <manjugk@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/i2c/twl.h>
#include <plat/keypad.h>
#include <asm/cacheflush.h>           // cacheflush

#define KEY_PROC_FS

#ifdef KEY_PROC_FS
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#endif

/*
 * The TWL4030 family chips include a keypad controller that supports
 * up to an 8x8 switch matrix.  The controller can issue system wakeup
 * events, since it uses only the always-on 32KiHz oscillator, and has
 * an internal state machine that decodes pressed keys, including
 * multi-key combinations.
 *
 * This driver lets boards define what keycodes they wish to report for
 * which scancodes, as part of the "struct twl4030_keypad_data" used in
 * the probe() routine.
 *
 * See the TPS65950 documentation; that's the general availability
 * version of the TWL5030 second generation part.
 */
#define TWL4030_MAX_ROWS	8	/* TWL4030 hard limit */
#define TWL4030_MAX_COLS	8
#define TWL4030_ROW_SHIFT	3
#define TWL4030_KEYMAP_SIZE	(TWL4030_MAX_ROWS * TWL4030_MAX_COLS)

extern void check_whether_x_is_fixed(void);

struct twl4030_keypad {
	unsigned short	keymap[TWL4030_KEYMAP_SIZE];
	u16		kp_state[TWL4030_MAX_ROWS];
	unsigned	n_rows;
	unsigned	n_cols;
	unsigned	irq;
	int keymapsize;
	struct device *dbg_dev;
	struct input_dev *input;
};

/*----------------------------------------------------------------------*/

/* arbitrary prescaler value 0..7 */
#define PTV_PRESCALER			4

/* Register Offsets */
#define KEYP_CTRL			0x00
#define KEYP_DEB			0x01
#define KEYP_LONG_KEY			0x02
#define KEYP_LK_PTV			0x03
#define KEYP_TIMEOUT_L			0x04
#define KEYP_TIMEOUT_H			0x05
#define KEYP_KBC			0x06
#define KEYP_KBR			0x07
#define KEYP_SMS			0x08
#define KEYP_FULL_CODE_7_0		0x09	/* row 0 column status */
#define KEYP_FULL_CODE_15_8		0x0a	/* ... row 1 ... */
#define KEYP_FULL_CODE_23_16		0x0b
#define KEYP_FULL_CODE_31_24		0x0c
#define KEYP_FULL_CODE_39_32		0x0d
#define KEYP_FULL_CODE_47_40		0x0e
#define KEYP_FULL_CODE_55_48		0x0f
#define KEYP_FULL_CODE_63_56		0x10
#define KEYP_ISR1			0x11
#define KEYP_IMR1			0x12
#define KEYP_ISR2			0x13
#define KEYP_IMR2			0x14
#define KEYP_SIR			0x15
#define KEYP_EDR			0x16	/* edge triggers */
#define KEYP_SIH_CTRL			0x17

/* KEYP_CTRL_REG Fields */
#define KEYP_CTRL_SOFT_NRST		BIT(0)
#define KEYP_CTRL_SOFTMODEN		BIT(1)
#define KEYP_CTRL_LK_EN			BIT(2)
#define KEYP_CTRL_TOE_EN		BIT(3)
#define KEYP_CTRL_TOLE_EN		BIT(4)
#define KEYP_CTRL_RP_EN			BIT(5)
#define KEYP_CTRL_KBD_ON		BIT(6)

#define PERSISTENT_KEY(col, row) (((col) << 28) | ((row) << 24) | \
                                                KEY_PERSISTENT)

//#define KEY(row, col, val)      (((row) << 28) | ((col) << 24) | (val))
#define ROWCOL_MASK     KEY(0xf, 0xf, 0)
//#define KEYNUM_MASK     ~PERSISTENT_KEY(0xf, 0xf)

      

/* KEYP_DEB, KEYP_LONG_KEY, KEYP_TIMEOUT_x*/
#define KEYP_PERIOD_US(t, prescale)	((t) / (31 << (prescale + 1)) - 1)

/* KEYP_LK_PTV_REG Fields */
#define KEYP_LK_PTV_PTV_SHIFT		5

/* KEYP_{IMR,ISR,SIR} Fields */
#define KEYP_IMR1_MIS			BIT(3)
#define KEYP_IMR1_TO			BIT(2)
#define KEYP_IMR1_LK			BIT(1)
#define KEYP_IMR1_KP			BIT(0)

/* KEYP_EDR Fields */
#define KEYP_EDR_KP_FALLING		0x01
#define KEYP_EDR_KP_RISING		0x02
#define KEYP_EDR_KP_BOTH		0x03
#define KEYP_EDR_LK_FALLING		0x04
#define KEYP_EDR_LK_RISING		0x08
#define KEYP_EDR_TO_FALLING		0x10
#define KEYP_EDR_TO_RISING		0x20
#define KEYP_EDR_MIS_FALLING		0x40
#define KEYP_EDR_MIS_RISING		0x80


/*might have errors */

#ifdef KEY_PROC_FS
int key_proc_ioctl(struct inode *, struct file *, unsigned int, unsigned long);
int key_proc_read(char *page, char **start, off_t off,int count, int *eof, void *data);
int key_proc_write(struct file *file, const char __user *buffer, unsigned long count, void *data);

#define IOCTL_KEY_PROC 'K'
#define KEY_PROC_FILE_NAME "key_proc"
enum 
{
        GET_DOME_KEY_COUNT = _IOR(IOCTL_KEY_PROC, 0, int),
};

struct proc_dir_entry *key_proc;
struct file_operations key_proc_fops = 
{
        .ioctl=key_proc_ioctl,
};

int pressed_count=0;
u16 pressed_state[TWL4030_MAX_ROWS];
unsigned int key_irq=0;

static void count_number_of_pressed_buttons(u16 *matrix, int *count);
#endif


/*end of might have errors*/


static int twl4030_find_key(struct twl4030_keypad *kp, int col, int row)
{
        int i, rc;

        rc = KEY(col, row, 0);
        for (i = 0; i < kp->keymapsize; i++)
                if ((kp->keymap[i] & ROWCOL_MASK) == rc)
                        return kp->keymap[i] & (KEYNUM_MASK | KEY_PERSISTENT);

        return -EINVAL;
}




/*----------------------------------------------------------------------*/

static int twl4030_kpread(struct twl4030_keypad *kp,
		u8 *data, u32 reg, u8 num_bytes)
{
	int ret = twl_i2c_read(TWL4030_MODULE_KEYPAD, data, reg, num_bytes);

	if (ret < 0)
		dev_warn(kp->dbg_dev,
			"Couldn't read TWL4030: %X - ret %d[%x]\n",
			 reg, ret, ret);

	return ret;
}

static int twl4030_kpwrite_u8(struct twl4030_keypad *kp, u8 data, u32 reg)
{
	int ret = twl_i2c_write_u8(TWL4030_MODULE_KEYPAD, data, reg);

	if (ret < 0)
		dev_warn(kp->dbg_dev,
			"Could not write TWL4030: %X - ret %d[%x]\n",
			 reg, ret, ret);

	return ret;
}

static inline u16 twl4030_col_xlate(struct twl4030_keypad *kp, u8 col)
{
	/* If all bits in a row are active for all coloumns then
	 * we have that row line connected to gnd. Mark this
	 * key on as if it was on matrix position n_cols (ie
	 * one higher than the size of the matrix).
	 */
	if (col == 0xFF)
		return 1 << kp->n_cols;
	else
		return col & ((1 << kp->n_cols) - 1);
}

static int twl4030_read_kp_matrix_state(struct twl4030_keypad *kp, u16 *state)
{
	u8 new_state[TWL4030_MAX_ROWS];
	int row;
	int ret = twl4030_kpread(kp, new_state,
				 KEYP_FULL_CODE_7_0, kp->n_rows);


#ifdef CONFIG_ARCHER_KOR_DEBUG_USER	// forced panic
#if ( CONFIG_ARCHER_HW_REV >= 12 )
	if ( new_state[1] == 2 && new_state[2] == 4 )
#elif ( CONFIG_ARCHER_HW_REV == 11 )
	if ( new_state[1] == 2 && new_state[2] == 4 )
#elif ( CONFIG_ARCHER_HW_REV == 10 )
	if ( new_state[1] == 6 )
#else
	if ( new_state[0] == 6 && new_state[2] == 6 )
#endif
		panic("__forced_upload");
#endif

#if ( CONFIG_ARCHER_HW_REV >= 12 )
	if ( new_state[1] == 4 && new_state[2] == 4 )
	{
    	/* flush cache back to ram */
    	flush_cache_all();
    }
#endif

	if (ret >= 0)
		for (row = 0; row < kp->n_rows; row++)
			state[row] = twl4030_col_xlate(kp, new_state[row]);

	return ret;
}

static int twl4030_is_in_ghost_state(struct twl4030_keypad *kp, u16 *key_state)
{
	int i;
	u16 check = 0;

	for (i = 0; i < kp->n_rows; i++) {
		u16 col = key_state[i];

		if ((col & check) && hweight16(col) > 1)
			return 1;

		check |= col;
	}

	return 0;
}

static void twl4030_kp_scan(struct twl4030_keypad *kp, bool release_all)
{
	struct input_dev *input = kp->input;
	u16 new_state[TWL4030_MAX_ROWS];
	int col, row, key;

	if (release_all)
		memset(new_state, 0, sizeof(new_state));
	else {
		/* check for any changes */
		int ret = twl4030_read_kp_matrix_state(kp, new_state);

		if (ret < 0)	/* panic ... */
			return;

		if (twl4030_is_in_ghost_state(kp, new_state))
			return;
	}

	/* check for changes and print those */
	for (row = 0; row < kp->n_rows; row++) {
		int changed = new_state[row] ^ kp->kp_state[row];

		if (!changed)
			continue;

		for (col = 0; col < kp->n_cols; col++) {
			int code;

			if (!(changed & (1 << col)))
				continue;

			dev_dbg(kp->dbg_dev, "key [%d:%d] %s\n", row, col,
				(new_state[row] & (1 << col)) ?
				"press" : "release");

      		        key = twl4030_find_key(kp, col, row);
			//printk("[KEY] [value = %d], status = %d]\n", key, new_state[row]); // klaatu log

			{

				if(key == 103 && new_state[row] == 2 )
				{
					//check_whether_x_is_fixed();
				}
			}



			code = MATRIX_SCAN_CODE(row, col, TWL4030_ROW_SHIFT);
			input_event(input, EV_MSC, MSC_SCAN, code);
			printk("key[%d] : %d \n", code, kp->keymap[code]);
			input_report_key(input, kp->keymap[code],
					 new_state[row] & (1 << col));
		}
		kp->kp_state[row] = new_state[row];

#ifdef KEY_PROC_FS
		pressed_state[row] = new_state[row];
#endif

	}
	input_sync(input);
}

/*
 * Keypad interrupt handler
 */
static irqreturn_t do_kp_irq(int irq, void *_kp)
{
	struct twl4030_keypad *kp = _kp;
	u8 reg;
	int ret;

#ifdef CONFIG_LOCKDEP
	/* WORKAROUND for lockdep forcing IRQF_DISABLED on us, which
	 * we don't want and can't tolerate.  Although it might be
	 * friendlier not to borrow this thread context...
	 */
	local_irq_enable();
#endif

	/* Read & Clear TWL4030 pending interrupt */
	ret = twl4030_kpread(kp, &reg, KEYP_ISR1, 1);

	/* Release all keys if I2C has gone bad or
	 * the KEYP has gone to idle state */
	if (ret >= 0 && (reg & KEYP_IMR1_KP))
		twl4030_kp_scan(kp, false);
	else
		twl4030_kp_scan(kp, true);

	return IRQ_HANDLED;
}

static int __devinit twl4030_kp_program(struct twl4030_keypad *kp)
{
	u8 reg;
	int i;

	/* Enable controller, with hardware decoding but not autorepeat */
	reg = KEYP_CTRL_SOFT_NRST | KEYP_CTRL_SOFTMODEN
		| KEYP_CTRL_TOE_EN | KEYP_CTRL_KBD_ON;
	if (twl4030_kpwrite_u8(kp, reg, KEYP_CTRL) < 0)
		return -EIO;

	/* NOTE:  we could use sih_setup() here to package keypad
	 * event sources as four different IRQs ... but we don't.
	 */

	/* Enable TO rising and KP rising and falling edge detection */
	reg = KEYP_EDR_KP_BOTH | KEYP_EDR_TO_RISING;
	if (twl4030_kpwrite_u8(kp, reg, KEYP_EDR) < 0)
		return -EIO;

	/* Set PTV prescaler Field */
	reg = (PTV_PRESCALER << KEYP_LK_PTV_PTV_SHIFT);
	if (twl4030_kpwrite_u8(kp, reg, KEYP_LK_PTV) < 0)
		return -EIO;

	/* Set key debounce time to 20 ms */
	i = KEYP_PERIOD_US(20000, PTV_PRESCALER);
	if (twl4030_kpwrite_u8(kp, i, KEYP_DEB) < 0)
		return -EIO;

	/* Set timeout period to 100 ms */
	i = KEYP_PERIOD_US(200000, PTV_PRESCALER);
	if (twl4030_kpwrite_u8(kp, (i & 0xFF), KEYP_TIMEOUT_L) < 0)
		return -EIO;

	if (twl4030_kpwrite_u8(kp, (i >> 8), KEYP_TIMEOUT_H) < 0)
		return -EIO;

	/*
	 * Enable Clear-on-Read; disable remembering events that fire
	 * after the IRQ but before our handler acks (reads) them,
	 */
	reg = TWL4030_SIH_CTRL_COR_MASK | TWL4030_SIH_CTRL_PENDDIS_MASK;
	if (twl4030_kpwrite_u8(kp, reg, KEYP_SIH_CTRL) < 0)
		return -EIO;

	/* initialize key state; irqs update it from here on */
	if (twl4030_read_kp_matrix_state(kp, kp->kp_state) < 0)
		return -EIO;

	return 0;
}

/*
 * Registers keypad device with input subsystem
 * and configures TWL4030 keypad registers
 */
static int __devinit twl4030_kp_probe(struct platform_device *pdev)
{
	struct twl4030_keypad_data *pdata = pdev->dev.platform_data;
	const struct matrix_keymap_data *keymap_data = pdata->keymap_data;
	struct twl4030_keypad *kp;
	struct input_dev *input;
	u8 reg;
	int error;
	
//	printk(KERN_EMERG "$$$$    %s -  %i $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n", __FUNCTION__, __LINE__);

	if (!pdata || !pdata->rows || !pdata->cols ||
	    pdata->rows > TWL4030_MAX_ROWS || pdata->cols > TWL4030_MAX_COLS) {
		dev_err(&pdev->dev, "Invalid platform_data\n");
		return -EINVAL;
	}

	kp = kzalloc(sizeof(*kp), GFP_KERNEL);
	input = input_allocate_device();
	if (!kp || !input) {
		error = -ENOMEM;
		goto err1;
	}

	/* Get the debug Device */
	kp->dbg_dev = &pdev->dev;
	kp->input = input;

	kp->n_rows = pdata->rows;
	kp->n_cols = pdata->cols;
	kp->irq = platform_get_irq(pdev, 0);

	/* setup input device */
	__set_bit(EV_KEY, input->evbit);

	/* Enable auto repeat feature of Linux input subsystem */
//	if (pdata->rep)
//		__set_bit(EV_REP, input->evbit);

#if 0 // def CONFIG_ARCHER_TARGET_SK // kmj_cm10.varation_of_keylayout
#if (CONFIG_ARCHER_REV <= ARCHER_REV05)
	kp->input->name     = "TWL4030 Keypad rev00";
	kp->input->phys     = "twl4030_keypad_rev00/input0";
#elif (CONFIG_ARCHER_REV == ARCHER_REV10)     
	kp->input->name     = "TWL4030 Keypad rev10";
	kp->input->phys     = "twl4030_keypad_rev10/input0";
#elif (CONFIG_ARCHER_REV == ARCHER_REV11)     
	kp->input->name     = "TWL4030 Keypad rev11";
	kp->input->phys     = "twl4030_keypad_rev11/input0";
#elif (CONFIG_ARCHER_REV == ARCHER_REV12)     
	kp->input->name     = "TWL4030 Keypad rev12";
	kp->input->phys     = "twl4030_keypad_rev12/input0";
#elif (CONFIG_ARCHER_REV == ARCHER_REV13)     
	kp->input->name     = "TWL4030 Keypad rev13";
	kp->input->phys     = "twl4030_keypad_rev13/input0";
#elif (CONFIG_ARCHER_REV == ARCHER_REV20)     
	kp->input->name     = "TWL4030 Keypad rev20";
	kp->input->phys     = "twl4030_keypad_rev20/input0";
#else
//#error " Key Layout file doesn't exist."
#endif

/* may have errors */

#else
	kp->input->name     = "TWL4030 Keypad";
	kp->input->phys     = "twl4030_keypad/input0";
	printk("kp->input->name :%s ",kp->input->name);
#endif
	kp->input->dev.parent	= &pdev->dev;

/*end of may have erros*/

	input_set_capability(input, EV_MSC, MSC_SCAN);

	input->name		= "TWL4030 Keypad";
	input->phys		= "twl4030_keypad/input0";
	input->dev.parent	= &pdev->dev;

	input->id.bustype	= BUS_HOST;
	input->id.vendor	= 0x0001;
	input->id.product	= 0x0001;
	input->id.version	= 0x0003;

	input->keycode		= kp->keymap;
	input->keycodesize	= sizeof(kp->keymap[0]);
	input->keycodemax	= ARRAY_SIZE(kp->keymap);

	matrix_keypad_build_keymap(keymap_data, TWL4030_ROW_SHIFT,
				   input->keycode, input->keybit);

	error = input_register_device(input);
	if (error) {
		dev_err(kp->dbg_dev,
			"Unable to register twl4030 keypad device\n");
		goto err1;
	}

	error = twl4030_kp_program(kp);
	if (error)
		goto err2;

	/*
	 * This ISR will always execute in kernel thread context because of
	 * the need to access the TWL4030 over the I2C bus.
	 *
	 * NOTE:  we assume this host is wired to TWL4040 INT1, not INT2 ...
	 */
	error = request_irq(kp->irq, do_kp_irq, 0, pdev->name, kp);
	if (error) {
		dev_info(kp->dbg_dev, "request_irq failed for irq no=%d\n",
			kp->irq);
		goto err3;
	}

	/* Enable KP and TO interrupts now. */
	reg = (u8) ~(KEYP_IMR1_KP | KEYP_IMR1_TO);
	if (twl4030_kpwrite_u8(kp, reg, KEYP_IMR1)) {
		error = -EIO;
		goto err4;
	}

	platform_set_drvdata(pdev, kp);

/* may have errors */

#ifdef KEY_PROC_FS
	key_irq = kp->irq;
	key_proc = create_proc_entry(KEY_PROC_FILE_NAME, S_IFREG | S_IRUGO, 0);
	if (key_proc)
	{
		key_proc->proc_fops = &key_proc_fops;
		printk(" succeeded in initializing touch proc file\n");
	}
	else
	{
		printk(" error occured in initializing touch proc file\n");
	}
#endif
/* end of may have errors */

	return 0;

err4:
	/* mask all events - we don't care about the result */
	(void) twl4030_kpwrite_u8(kp, 0xff, KEYP_IMR1);
err3:
	free_irq(kp->irq, NULL);
err2:
	input_unregister_device(input);
	input = NULL;
err1:
	input_free_device(input);
	kfree(kp);
	return error;
}

static int __devexit twl4030_kp_remove(struct platform_device *pdev)
{
	struct twl4030_keypad *kp = platform_get_drvdata(pdev);

	free_irq(kp->irq, kp);
	input_unregister_device(kp->input);
	platform_set_drvdata(pdev, NULL);
	kfree(kp);

	return 0;
}

/*
 * NOTE: twl4030 are multi-function devices connected via I2C.
 * So this device is a child of an I2C parent, thus it needs to
 * support unplug/replug (which most platform devices don't).
 */

static struct platform_driver twl4030_kp_driver = {
	.probe		= twl4030_kp_probe,
	.remove		= __devexit_p(twl4030_kp_remove),
	.driver		= {
		.name	= "twl4030_keypad",
		.owner	= THIS_MODULE,
	},
};

static int __init twl4030_kp_init(void)
{
	//printk(KERN_EMERG "$$$$    %s -  %i $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n", __FUNCTION__, __LINE__);
	return platform_driver_register(&twl4030_kp_driver);
}
module_init(twl4030_kp_init);

static void __exit twl4030_kp_exit(void)
{
	platform_driver_unregister(&twl4030_kp_driver);
}
module_exit(twl4030_kp_exit);


#ifdef KEY_PROC_FS
int key_proc_ioctl(struct inode *p_node, struct file *p_file, unsigned int cmd, unsigned long arg)
{
        switch(cmd)
        {
                case GET_DOME_KEY_COUNT :
                {
                        pressed_count = 0;
                        disable_irq(key_irq);
                        count_number_of_pressed_buttons(pressed_state, &pressed_count);
                        enable_irq(key_irq);
                        if(copy_to_user((void*)arg, (const void*)&pressed_count, sizeof(int)))
                          return -EFAULT;
                }
                break;
        }
        return 0;
}
static void count_number_of_pressed_buttons(u16 *matrix, int *count)
{
        int i, j;

        for(i=0; i<TWL4030_MAX_ROWS; i++)
        {
                u16 temp_state = matrix[i];
                for( j=0; j<16; j++)
                {
                        if(((temp_state>>j)&0x1) == 0x1)
                                (*count)++;
                }

        }
}
#endif

MODULE_AUTHOR("Texas Instruments");
MODULE_DESCRIPTION("TWL4030 Keypad Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:twl4030_keypad");

