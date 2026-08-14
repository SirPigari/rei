/* 
 * Rule 110 in Rei
 * 
 * This is a simple implementation of the Rule 110 cellular automaton in the Rei programming language.
 * Inspired by the B implementation at https://github.com/bext-lang/b/blob/master/examples/rule110.b
 */

extern calloc(count: usize, size: usize) -> *void;
extern free(ptr: *void) -> void;
extern putchar(s: u8) -> i32;

N :: 100;

display :: (base: *u8) -> void {
    i: i32 = 0;
    while (i < N) {
        if base[i]
            putchar('#');
        else
            putchar('.');
        i++;
    }
    putchar(10);
}

next :: (base: *u8) -> void {
    state: u8 = base[0] | base[1] << 1;
    i: i32 = 2;
    while (i < N) {
        state <<= 1;
        state |= base[i];
        state &= 0b111;
        base[i - 1] = (110>>state) & 1;
        i++;
    }
}

main :: () -> i32 {
    base: *u8 = calloc(1, N);
    base[N - 2] = 1;

    i: i32 = 0;

    display(base);
    while (i < N - 3) {
        next(base);
        display(base);
        i++;
    }

    free(base);

    return 0;
}
