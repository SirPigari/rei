extern puts(s: *u8) -> i32;

main :: () -> i32 {
    /*   v --- 'c' because we need a "cstr" (null-terminated thin ptr) for puts */
    puts(c"Hello world"); /* puts adds the newline for us */
    return 0;
}
