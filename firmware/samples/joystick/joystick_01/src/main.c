#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>

static void js_cb(struct input_event *evt, void *ud)
{
	if(evt->sync == 0)
	{
		return;
	}

	switch(evt->type)
	{
		case INPUT_EV_ABS:
			if(evt->code == INPUT_ABS_X || evt->code == INPUT_ABS_Y)
			{
				printk("Joystick moved: code=%d, value=%d\n", evt->code, evt->value);
			}
			else
			{
				printk("Analog event: code=%d, value=%d\n", evt->code, evt->value);
			}
			
			break;
		case INPUT_EV_KEY:
			if(!evt->value)
			break;
			switch(evt->code)
			{
				case INPUT_KEY_KPENTER: printk("short\n");  break;
				case INPUT_KEY_PLAYPAUSE : printk("double\n"); break;
				case INPUT_KEY_MENU : printk("longpress\n"); break;
			}
			break;
		default:
			printk("Unknown event type: %d\n", evt->type);
	}
}

INPUT_CALLBACK_DEFINE(NULL, js_cb, NULL);

int main(void)
{ 
    printk("joystick sample application started\n");
	return 0;
}
