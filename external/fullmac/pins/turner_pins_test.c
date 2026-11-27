#include <linux/module.h>
#include <linux/of.h>
#include "blob.h"
static int id;
static int __init apply(void) { return of_overlay_fdt_apply(pins, pins_len, &id, NULL); }
static void __exit undo(void) { of_overlay_remove(&id); }
module_init(apply);
module_exit(undo);
MODULE_LICENSE("GPL");
