extern puts(s: *u8) -> i32;

main :: () -> i32 {
    puts(c"Hello world"); /* puts adds the newline for us */
    return 0;
}
