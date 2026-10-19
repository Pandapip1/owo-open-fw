#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>

int main(void)
{
	int err = bt_enable(NULL);

	printk("bt_enable: %d\n", err);
	return 0;
}
