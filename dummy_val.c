#include "types.h"
#include "defs.h"

static int dummy_value = 0;

int sys_dummy_val(void)
{
    return dummy_value;
}

int sys_dummy_val_set(void)
{
    int value;
    if (argint(0, &value) < 0) {
        return -1;
    }
    dummy_value = value;
    return 0;
}

