#include "types.h"
#include "user.h"

int main(int argc, char **argv)
{
    if (argc == 1) {
        printf(1, "%d\n", dummy_val());
    } else if (argc == 2) {
        int value = atoi(argv[1]);
        if (dummy_val_set(value) < 0) {
            printf(2, "dummy_test: Could not set dummy_val\n");
        }
    } else {
        printf(2, "dummy_test [optional_int_value]");
    }
    exit();
}

